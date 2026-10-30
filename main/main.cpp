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
 *
 * Tryb bateryjny (okres raportowania > 0 s, ustawiany w portalu; tylko ESP32-C6):
 *   impulsy liczy rdzen LP (energy_meter/lp_core/main.c), a rdzen HP spi w deep sleep i co okres:
 *   odczytuje licznik LP --> WiFi + MQTT --> raport --> deep sleep.
 */
#include <cstdio>
#include <memory>
#include <string>

#include <sys/time.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_portal/config_portal.hpp"
#include "configuration/Configuration.hpp"
#include "energy_meter/consumption_history.hpp"
#include "energy_meter/energy_meter.hpp"
#include "energy_meter/lp_counter.hpp"
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

// Stan licznika do raportu - wspolny dla trybu sieciowego (EnergyMeter) i bateryjnego (rdzen LP).
struct MeterSnapshot
{
    uint32_t pulses;
    double power_w; // ujemna = nieznana
    uint32_t last_pulse_width_ms;
    uint32_t rejected;
};

static MeterSnapshot mains_snapshot()
{
    EnergyMeter &meter = EnergyMeter::instance();
    return {meter.pulses(), meter.power_w(), meter.last_pulse_width_ms(), meter.rejected()};
}

static bool report(Mqtt &mqtt, const DeviceConfig &cfg, const MeterSnapshot &meter)
{
    const uint32_t imp = Configuration::imp_per_kwh(cfg);
    const uint32_t pulses = meter.pulses;
    bool ok = true;

    auto pub = [&](const char *topic, const std::string &value) { ok = mqtt.publish(topic, value, 0, true) && ok; };

    // Licznik: kWh (<main_topic> i <main_topic>/kwh - ta sama wartosc) oraz surowe impulsy.
    const std::string kwh = EnergyMeter::format_kwh(pulses, imp);
    pub("", kwh);
    pub("kwh", kwh);
    pub("pulses", std::to_string(pulses));

    // Chwilowa moc [W] (ujemna = jeszcze nieznana).
    const double power = meter.power_w;
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
    pub("pulse_width_ms", std::to_string(meter.last_pulse_width_ms));
    pub("rejected_flashes", std::to_string(meter.rejected));

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
                report(*mqtt, cfg, mains_snapshot());
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

// ---------------------------------------------------------------------------------------------
// Tryb bateryjny: impulsy liczy rdzen LP, HP budzi sie co okres, wysyla raport i zasypia.
// ---------------------------------------------------------------------------------------------

static constexpr int BATTERY_WIFI_TIMEOUT_MS = 10000;  // krocej niz w trybie sieciowym - kazda sekunda radia to prad
static constexpr int BATTERY_MQTT_TIMEOUT_MS = 8000;
static constexpr int BATTERY_FLUSH_TIMEOUT_MS = 4000;
static constexpr int64_t BATTERY_MIN_SLEEP_US = 1000000;
static constexpr uint32_t BATTERY_MAGIC = 0x42415431; // "BAT1"

// Stan licznika miedzy wybudzeniami (pamiec RTC przezywa deep sleep).
struct BatteryState
{
    uint32_t magic;
    uint32_t total_pulses;   // stan licznika energii [impulsy]: z NVS przy zimnym starcie, potem pamiec RTC
    uint32_t last_lp_count;  // odczyt licznika LP przy poprzednim wybudzeniu
    int64_t last_wake_us;    // czas poprzedniego odczytu (zegar systemowy, dziala przez deep sleep)
    bool have_last;
};
RTC_DATA_ATTR static BatteryState s_bat = {};

// Komenda "set" doszla do urzadzenia podczas jednego okna polaczenia - raport trzeba powtorzyc z nowa wartoscia.
static volatile bool s_bat_report_again = false;

static int64_t wall_us()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Komenda "<main_topic>/set" (stan licznika w kWh). Retained jest dostarczana zaraz po polaczeniu, wiec
// zadziala w tym samym wybudzeniu, a najpozniej w nastepnym.
static void handle_battery_message(Mqtt &mqtt, uint32_t imp, const std::string &subtopic, const std::string &payload)
{
    if (subtopic != "set")
        return;

    uint32_t pulses = 0;
    if (EnergyMeter::parse_kwh(payload, imp, pulses))
    {
        s_bat.total_pulses = pulses;
        s_bat.last_lp_count = LpCounter::count(); // kolejne impulsy beda doliczane od tego momentu
        Storage::GetInstance()->save_pulses(pulses);
        s_bat_report_again = true;
    }
    else
    {
        ESP_LOGW(TAG, "Nieprawidlowa wartosc licznika: '%s'", payload.c_str());
    }
    mqtt.publish("set", "", 1, true); // czyscimy retained, zeby nie ustawiac licznika przy kazdym polaczeniu
}

static MeterSnapshot battery_snapshot(double power_w)
{
    return {s_bat.total_pulses, power_w, LpCounter::last_width_ms(), LpCounter::rejected()};
}

static void run_battery(const DeviceConfig &cfg, uint32_t period_s)
{
    const uint32_t imp = Configuration::imp_per_kwh(cfg);
    const bool lp_fresh = LpCounter::start(); // przy wybudzeniu z deep sleep rdzen LP juz liczy

    if (lp_fresh || s_bat.magic != BATTERY_MAGIC)
    {
        // Zimny start (zasilanie / reset): stan licznika z NVS, licznik LP liczy od zera.
        uint32_t saved = 0;
        Storage::GetInstance()->read_pulses(saved);
        s_bat = {};
        s_bat.magic = BATTERY_MAGIC;
        s_bat.total_pulses = saved;
        s_bat.last_lp_count = LpCounter::count();
    }

    // Impulsy zliczone przez rdzen LP od poprzedniego wybudzenia.
    const int64_t now_us = wall_us();
    const uint32_t lp = LpCounter::count();
    const uint32_t delta = lp - s_bat.last_lp_count; // arytmetyka bez znaku znosi zawiniecie licznika
    s_bat.last_lp_count = lp;
    s_bat.total_pulses += delta;

    // Moc srednia z calego okna miedzy odczytami (a nie z odstepu dwoch ostatnich impulsow, jak w trybie sieciowym -
    // przy raporcie co minuty/godzine pojedynczy odstep nic nie mowi o sredniej). Nieznana po zimnym starcie.
    double power_w = -1.0;
    if (s_bat.have_last && now_us > s_bat.last_wake_us)
        power_w = (double)delta * 3.6e12 / ((double)imp * (double)(now_us - s_bat.last_wake_us));
    s_bat.last_wake_us = now_us;
    s_bat.have_last = true;

    if (delta > 0)
    {
        ConsumptionHistory::record(delta);
        Storage::GetInstance()->save_pulses(s_bat.total_pulses); // przed WiFi: zasilanie moze zniknac w kazdej chwili
    }
    ESP_LOGI(TAG, "Wybudzenie: +%lu imp, razem %lu, moc %.0f W", (unsigned long)delta,
             (unsigned long)s_bat.total_pulses, power_w);

    if (cfg.wifi_ssid.empty() || cfg.main_topic.empty())
    {
        ESP_LOGW(TAG, "Brak WiFi lub tokenu - tylko liczenie impulsow (konfiguracja: zwarcie D8-GND i reset)");
    }
    else if (wifi_manager_connect_sta(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str(), BATTERY_WIFI_TIMEOUT_MS))
    {
        Mqtt mqtt(cfg);
        mqtt.on_message([&mqtt, imp](const std::string &sub, const std::string &payload)
                        { handle_battery_message(mqtt, imp, sub, payload); });

        if (mqtt.start() && mqtt.wait_connected(BATTERY_MQTT_TIMEOUT_MS))
        {
            report(mqtt, cfg, battery_snapshot(power_w));
            mqtt.wait_flushed(BATTERY_FLUSH_TIMEOUT_MS);

            if (s_bat_report_again)
            {
                s_bat_report_again = false;
                report(mqtt, cfg, battery_snapshot(power_w));
                mqtt.wait_flushed(BATTERY_FLUSH_TIMEOUT_MS);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Brak polaczenia z brokerem - raport pominiety, impulsy zostaja w liczniku");
        }
        mqtt.stop();
    }
    wifi_manager_stop();

    // Nastepne wybudzenie liczone od poprzedniego odczytu, wiec czas pracy radia nie wydluza okresu.
    int64_t sleep_us = s_bat.last_wake_us + (int64_t)period_s * 1000000 - wall_us();
    if (sleep_us < BATTERY_MIN_SLEEP_US)
        sleep_us = BATTERY_MIN_SLEEP_US;

    ESP_LOGI(TAG, "Deep sleep na %lld s", (long long)(sleep_us / 1000000));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_us));
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup()); // utrzymuje domene RTC (rdzen LP) wlaczona w deep sleep
    esp_deep_sleep_start();
}

static void run_battery_mode(const DeviceConfig &cfg, uint32_t period_s)
{
    // Konfiguracja: zwarcie D8-GND i reset/zasilanie. Przy wybudzeniach z deep sleep nie sprawdzamy zworki (prad).
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP && config_jumper_closed())
    {
        ESP_LOGW(TAG, "D8 zwarte z GND - tryb konfiguracji (siec '%s')", config_defaults::AP_SSID);
        LpCounter::stop(); // pin impulsow wraca do zwyklego GPIO - zliczaniem w portalu zajmuje sie EnergyMeter
        EnergyMeter::instance().start(Configuration::imp_per_kwh(cfg)); // portal pokazuje/ustawia stan licznika
        ConfigPortal::run(config_defaults::CONFIG_MODE_TIMEOUT_S);       // po zapisaniu ustawien restartuje urzadzenie
        esp_restart(); // czysty start: pin impulsow wraca do rdzenia LP
    }

    run_battery(cfg, period_s);
}

extern "C" void app_main(void)
{
    Storage::GetInstance(); // inicjalizuje NVS

    DeviceConfig cfg = Configuration::load();

    const uint32_t period_s = Configuration::report_period_s(cfg);
    if (period_s > 0)
    {
        run_battery_mode(cfg, period_s);
        return;
    }

    LpCounter::stop(); // po przelaczeniu z trybu bateryjnego rdzen LP moglby jeszcze liczyc na tym samym pinie

    EnergyMeter::instance().start(Configuration::imp_per_kwh(cfg)); // liczenie impulsow dziala od teraz

    if (config_jumper_closed())
    {
        ESP_LOGW(TAG, "D8 zwarte z GND - tryb konfiguracji (siec '%s')", config_defaults::AP_SSID);
        ConfigPortal::run(config_defaults::CONFIG_MODE_TIMEOUT_S); // po zapisaniu ustawien restartuje urzadzenie
        cfg = Configuration::load();
    }

    run_online(cfg);
}
