/*
 * Licznik energii elektrycznej - zlicza blyski diody licznika (np. 1000 imp/kWh) i raportuje po MQTT.
 *
 * Urzadzenie jest zasilane z sieci, wiec nie uzywa deep sleep: WiFi i MQTT sa stale polaczone.
 *
 *   start --> licznik impulsow (EnergyMeter) dziala w tle od razu
 *         --> D8 zwarte z GND przy starcie? --> tryb konfiguracji (siec AP + strona WWW)
 *         --> laczenie z WiFi + MQTT, raport co REPORT_PERIOD_S
 *
 * Brak polaczenia dluzej niz OFFLINE_RESTART_S => zapis licznika i restart.
 */
#include <cstdio>
#include <memory>
#include <string>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_portal/config_portal.hpp"
#include "configuration/Configuration.hpp"
#include "energy_meter/consumption_history.hpp"
#include "energy_meter/energy_meter.hpp"
#include "mqtt/mqtt.hpp"
#include "storage/storage.hpp"
#include "wifi_manager.h"

static const char *TAG = "MAIN";



#ifdef ESP32_C3
// XIAO ESP32-C3: D8 = GPIO10 --- IGNORE ---
// Zwarcie D8 (GPIO19 na XIAO ESP32-C6!) z GND przy starcie => tryb konfiguracji (siec AP + strona WWW).
static constexpr gpio_num_t CONFIG_GPIO = GPIO_NUM_8;
#elif ESP32_C6
// Zwarcie D8 (GPIO19 na XIAO ESP32-C6!) z GND przy starcie => tryb konfiguracji (siec AP + strona WWW).
static constexpr gpio_num_t CONFIG_GPIO = GPIO_NUM_19;
#endif

static constexpr int WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr int64_t WIFI_RETRY_S = 30;         // przerwa miedzy nieudanymi probami polaczenia
static constexpr int64_t REPORT_PERIOD_S = 15;      // co ile publikujemy dane
static constexpr int64_t FLUSH_PERIOD_S = 300;       // co ile zapisujemy licznik w NVS
static constexpr int64_t OFFLINE_RESTART_S = 15 * 60;

// Ustawiane, gdy przyszla komenda "set" - raport od razu.
static volatile bool s_report_now = false;

static int64_t uptime_s()
{
    return esp_timer_get_time() / 1000000;
}

static bool config_jumper_closed()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    vTaskDelay(pdMS_TO_TICKS(10)); // ustalenie pull-upa
    for (int i = 0; i < 5; ++i)    // 5 x 10 ms zwarcia z rzedu
    {
        if (gpio_get_level(CONFIG_GPIO) != 0)
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// Komenda "<main_topic>/set" ze stanem licznika w kWh (np. "12345.678").
static void handle_message(Mqtt &mqtt, const std::string &subtopic, const std::string &payload)
{
    if (subtopic != "set")
        return;

    EnergyMeter &meter = EnergyMeter::instance();
    uint32_t pulses = 0;
    if (EnergyMeter::parse_kwh(payload, meter.imp_per_kwh(), pulses))
    {
        meter.set_pulses(pulses);
        s_report_now = true;
    }
    else
    {
        ESP_LOGW(TAG, "Nieprawidlowa wartosc licznika: '%s'", payload.c_str());
    }
    mqtt.publish("set", "", 1, true); // czyscimy retained, zeby nie ustawiac licznika przy kazdym polaczeniu
}

static bool report(Mqtt &mqtt, const DeviceConfig &cfg)
{
    EnergyMeter &meter = EnergyMeter::instance();
    const uint32_t imp = meter.imp_per_kwh();
    const uint32_t pulses = meter.pulses();
    bool ok = true;

    auto pub = [&](const char *topic, const std::string &value) { ok = mqtt.publish(topic, value, 0, true) && ok; };

    // Licznik: kWh (<main_topic> i <main_topic>/kwh - ta sama wartosc) oraz surowe impulsy.
    const std::string kwh = EnergyMeter::format_kwh(pulses, imp);
    pub("", kwh);
    pub("kwh", kwh);
    pub("pulses", std::to_string(pulses));

    // Chwilowa moc [W] (ujemna = jeszcze nieznana).
    const double power = meter.power_w();
    char buf[24];
    if (power >= 0.0)
    {
        snprintf(buf, sizeof(buf), "%.0f", power);
        pub("power_w", buf);
    }

    // Zuzycie [kWh] w kroczacych oknach 1 h / 3 h / 24 h (rozdzielczosc 5 min).
    struct Window
    {
        int hours;
        const char *topic;
    };
    static const Window windows[] = {{1, "kwh_last_1h"}, {3, "kwh_last_3h"}, {24, "kwh_last_24h"}};
    uint32_t pulses_24h = 0;
    for (const Window &w : windows)
    {
        const uint32_t p = ConsumptionHistory::pulses_in_last(w.hours);
        if (w.hours == 24)
            pulses_24h = p;
        pub(w.topic, EnergyMeter::format_kwh(p, imp));
    }

    // Ile godzin historii jest dostepne (po restarcie okna sa zanizone, dopoki historia sie nie zapelni).
    snprintf(buf, sizeof(buf), "%.1f", ConsumptionHistory::covered_s() / 3600.0);
    pub("history_hours", buf);

    // Koszt ostatnich 24 h [zl] - tylko gdy podano cene.
    double price = 0.0;
    if (Configuration::parse_price(cfg.energy_price, price))
    {
        snprintf(buf, sizeof(buf), "%.2f", (double)pulses_24h / imp * price);
        pub("cost_last_24h", buf);
    }

    // Diagnostyka czujnika.
    pub("pulse_width_ms", std::to_string(meter.last_pulse_width_ms()));
    pub("rejected_flashes", std::to_string(meter.rejected()));

    ESP_LOGI(TAG, "Raport: %s kWh, moc %.0f W, 24h %s kWh, %s", kwh.c_str(), power,
             EnergyMeter::format_kwh(pulses_24h, imp).c_str(), ok ? "OK" : "BLAD");
    return ok;
}

static void run_online(const DeviceConfig &cfg)
{
    EnergyMeter &meter = EnergyMeter::instance();

    std::unique_ptr<Mqtt> mqtt;
    int64_t last_flush = uptime_s();
    int64_t last_report = -REPORT_PERIOD_S;
    int64_t offline_since = uptime_s();
    int64_t next_wifi_try = 0;

    if (cfg.wifi_ssid.empty())
        ESP_LOGW(TAG, "Brak SSID WiFi - tylko liczenie impulsow. Ustaw WiFi w trybie konfiguracji (zwarcie D8-GND przy starcie)");
    if (cfg.main_topic.empty())
        ESP_LOGW(TAG, "Brak tokenu urzadzenia - dane nie beda publikowane. Zeskanuj/wklej token z api.warcgr.pl w trybie konfiguracji");

    for (;;)
    {
        int64_t now = uptime_s();

        if (now - last_flush >= FLUSH_PERIOD_S)
        {
            meter.flush();
            last_flush = now;
        }

        if (mqtt && mqtt->is_connected())
        {
            offline_since = now;
            if (s_report_now || now - last_report >= REPORT_PERIOD_S)
            {
                s_report_now = false;
                report(*mqtt, cfg);
                last_report = now;
            }
        }
        else if (!cfg.wifi_ssid.empty() && !cfg.main_topic.empty())
        {
            // WiFi laczy sie ponownie samo; tutaj tylko pierwsze polaczenie po starcie.
            if (!mqtt && now >= next_wifi_try)
            {
                ESP_LOGI(TAG, "WiFi: ssid='%s', broker: %s", cfg.wifi_ssid.c_str(), cfg.mqtt_broker.c_str());
                if (wifi_manager_connect_sta(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str(), WIFI_CONNECT_TIMEOUT_MS))
                {
                    mqtt = std::make_unique<Mqtt>(cfg);
                    Mqtt *m = mqtt.get();
                    mqtt->on_message([m](const std::string &sub, const std::string &payload) { handle_message(*m, sub, payload); });
                    if (!mqtt->start())
                        mqtt.reset();
                }
                if (!mqtt)
                    next_wifi_try = uptime_s() + WIFI_RETRY_S;
            }

            if (now - offline_since >= OFFLINE_RESTART_S)
            {
                ESP_LOGE(TAG, "Brak polaczenia od %lld s - restart", (long long)(now - offline_since));
                meter.flush();
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void)
{
    Storage::GetInstance(); // inicjalizuje NVS

    DeviceConfig cfg = Configuration::load();
    EnergyMeter::instance().start(Configuration::imp_per_kwh(cfg)); // liczenie impulsow dziala od teraz

    if (config_jumper_closed())
    {
        ESP_LOGW(TAG, "D8 zwarte z GND - tryb konfiguracji (siec '%s')", config_defaults::AP_SSID);
        ConfigPortal::run(config_defaults::CONFIG_MODE_TIMEOUT_S); // po zapisaniu ustawien restartuje urzadzenie
        cfg = Configuration::load();
    }

    run_online(cfg);
}
