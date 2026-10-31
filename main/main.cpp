/*
 * Czujnik temperatury, wilgotnosci i cisnienia (BME680, XIAO ESP32-S3) - raportuje po MQTT.
 *
 * Tryb bateryjny (domyslny; okres raportowania > 0 s, ustawiany w portalu):
 *   deep sleep --> co okres: pomiar BME680 --> WiFi + MQTT --> raport --> deep sleep.
 *
 * Tryb sieciowy (okres 0 s): WiFi i MQTT stale polaczone, odczyt co MAINS_REPORT_PERIOD_S.
 * Brak polaczenia dluzej niz OFFLINE_RESTART_S => restart.
 *
 * Konfiguracja (siec AP + strona WWW): D3 zwarte z GND przy starcie (zasilanie / reset). Przy pierwszym
 * uruchomieniu, gdy nie ustawiono jeszcze WiFi albo tokenu, portal otwiera sie sam.
 * Przy wybudzeniach z deep sleep zworka nie jest sprawdzana (oszczednosc pradu).
 */
#include <cstdio>
#include <memory>
#include <string>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bme680/bme680.hpp"
#include "config_portal/config_portal.hpp"
#include "configuration/Configuration.hpp"
#include "lm35/lm35.hpp"
#include "mqtt/mqtt.hpp"
#include "storage/storage.hpp"
#include "wifi_manager.h"

static const char *TAG = "MAIN";

// XIAO ESP32-S3: D3 = GPIO4. Zwarcie D3 z GND przy starcie => tryb konfiguracji.
static constexpr gpio_num_t CONFIG_GPIO = GPIO_NUM_4;

static constexpr int WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr int64_t WIFI_RETRY_S = 30;           // przerwa miedzy nieudanymi probami polaczenia (tryb sieciowy)
static constexpr int64_t MAINS_REPORT_PERIOD_S = 30;  // co ile publikujemy odczyt (tryb sieciowy)
static constexpr int64_t OFFLINE_RESTART_S = 15 * 60;

static constexpr int BATTERY_WIFI_TIMEOUT_MS = 10000; // krocej niz w trybie sieciowym - kazda sekunda radia to prad
static constexpr int BATTERY_MQTT_TIMEOUT_MS = 8000;
static constexpr int BATTERY_FLUSH_TIMEOUT_MS = 4000;
static constexpr int64_t BATTERY_MIN_SLEEP_US = 1000000;

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
    {
        // DIAGNOSTYKA (tymczasowa): poziom D3 z pull-upem i z pull-downem.
        const int up = gpio_get_level(CONFIG_GPIO);
        gpio_set_pull_mode(CONFIG_GPIO, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(10));
        const int down = gpio_get_level(CONFIG_GPIO);
        gpio_set_pull_mode(CONFIG_GPIO, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGW(TAG, "D3 (GPIO%d): pull-up => %d, pull-down => %d", (int)CONFIG_GPIO, up, down);
    }
    for (int i = 0; i < 5; ++i)    // 5 x 10 ms zwarcia z rzedu
    {
        if (gpio_get_level(CONFIG_GPIO) != 0)
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// Komplet odczytow: czujnik wewnetrzny (BME680) i zewnetrzny (LM35); kazdy moze byc niedostepny.
struct Readings
{
    Bme680Reading inside = {};
    bool have_inside = false;
    float outside_c = 0;
    bool have_outside = false;

    bool any() const { return have_inside || have_outside; }
};

// Odczyt obu czujnikow. Wywolywac przed wlaczeniem radia (nadajnik WiFi grzeje plytke).
static Readings measure(Bme680 &bme, Lm35 &lm35)
{
    Readings r;
    r.have_inside = (bme.ready() || bme.begin()) && bme.read(r.inside);
    if (!r.have_inside)
    {
        ESP_LOGW(TAG, "Odczyt BME680 nieudany");
        bme.end(); // kolejna proba zacznie od ponownej inicjalizacji czujnika
    }
    r.have_outside = (lm35.ready() || lm35.begin()) && lm35.read(r.outside_c);
    if (!r.have_outside)
        ESP_LOGW(TAG, "Odczyt LM35 nieudany");
    return r;
}

// Publikuje odczyty:
//   <main_topic> i <main_topic>/temperature = temperatura w domu [st. C], /humidity [%], /pressure [hPa],
//   <main_topic>/temperature_outside = temperatura na zewnatrz (LM35) [st. C].
static bool report(Mqtt &mqtt, const Readings &r)
{
    bool ok = true;
    auto pub = [&](const char *topic, const char *value) { ok = mqtt.publish(topic, value, 0, true) && ok; };
    char buf[16];

    if (r.have_inside)
    {
        snprintf(buf, sizeof(buf), "%.1f", r.inside.temperature_c);
        pub("", buf);
        pub("temperature", buf);
        if (r.inside.has_humidity)
        {
            snprintf(buf, sizeof(buf), "%.1f", r.inside.humidity_pct);
            pub("humidity", buf);
        }
        snprintf(buf, sizeof(buf), "%.1f", r.inside.pressure_hpa);
        pub("pressure", buf);
    }
    if (r.have_outside)
    {
        snprintf(buf, sizeof(buf), "%.1f", r.outside_c);
        pub("temperature_outside", buf);
    }

    ESP_LOGI(TAG, "Raport: dom %.1f C, %.1f %%, %.1f hPa; zewnatrz %.1f C; %s", r.inside.temperature_c,
             r.inside.humidity_pct, r.inside.pressure_hpa, r.outside_c, ok ? "OK" : "BLAD");
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Tryb sieciowy: polaczenie stale, odczyt co MAINS_REPORT_PERIOD_S.
// ---------------------------------------------------------------------------------------------

static void run_online(const DeviceConfig &cfg)
{
    Bme680 sensor;
    Lm35 lm35;
    std::unique_ptr<Mqtt> mqtt;
    int64_t last_report = -MAINS_REPORT_PERIOD_S;
    int64_t offline_since = uptime_s();
    int64_t next_wifi_try = 0;

    if (cfg.wifi_ssid.empty())
        ESP_LOGW(TAG, "Brak SSID WiFi - ustaw WiFi w trybie konfiguracji (zwarcie D3-GND przy starcie)");
    if (cfg.main_topic.empty())
        ESP_LOGW(TAG, "Brak tokenu urzadzenia - dane nie beda publikowane. Wklej token z api.warcgr.pl w trybie konfiguracji");

    for (;;)
    {
        const int64_t now = uptime_s();

        if (mqtt && mqtt->is_connected())
        {
            offline_since = now;
            if (now - last_report >= MAINS_REPORT_PERIOD_S)
            {
                last_report = now;
                const Readings readings = measure(sensor, lm35);
                if (readings.any())
                    report(*mqtt, readings);
                else
                    ESP_LOGW(TAG, "Brak odczytow - raport pominiety");
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
                    if (!mqtt->start())
                        mqtt.reset();
                }
                if (!mqtt)
                    next_wifi_try = uptime_s() + WIFI_RETRY_S;
            }

            if (now - offline_since >= OFFLINE_RESTART_S)
            {
                ESP_LOGE(TAG, "Brak polaczenia od %lld s - restart", (long long)(now - offline_since));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------------------------
// Tryb bateryjny: pomiar, raport i deep sleep.
// ---------------------------------------------------------------------------------------------

static void run_battery(const DeviceConfig &cfg, uint32_t period_s)
{
    // Pomiar jako pierwszy, przed wlaczeniem radia - nadajnik WiFi grzeje plytke i zawyzalby temperature.
    Readings reading;
    {
        Bme680 sensor;
        Lm35 lm35;
        reading = measure(sensor, lm35);
    }

    if (!reading.any())
    {
        ESP_LOGW(TAG, "Brak odczytow - pomijam polaczenie z siecia");
    }
    else if (cfg.wifi_ssid.empty() || cfg.main_topic.empty())
    {
        ESP_LOGW(TAG, "Brak WiFi lub tokenu - nic nie wysylam (konfiguracja: zwarcie D3-GND i reset)");
    }
    else if (wifi_manager_connect_sta(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str(), BATTERY_WIFI_TIMEOUT_MS))
    {
        Mqtt mqtt(cfg);
        if (mqtt.start() && mqtt.wait_connected(BATTERY_MQTT_TIMEOUT_MS))
        {
            report(mqtt, reading);
            mqtt.wait_flushed(BATTERY_FLUSH_TIMEOUT_MS);
        }
        else
        {
            ESP_LOGW(TAG, "Brak polaczenia z brokerem - raport pominiety");
        }
        mqtt.stop();
    }
    wifi_manager_stop();

    // Czas pracy radia skracamy z okresu uspienia, zeby raporty szly w zadanych odstepach.
    int64_t sleep_us = (int64_t)period_s * 1000000 - esp_timer_get_time();
    if (sleep_us < BATTERY_MIN_SLEEP_US)
        sleep_us = BATTERY_MIN_SLEEP_US;

    ESP_LOGI(TAG, "Deep sleep na %lld s", (long long)(sleep_us / 1000000));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_us));
    esp_deep_sleep_start();
}

extern "C" void app_main(void)
{
    Storage::GetInstance(); // inicjalizuje NVS

    DeviceConfig cfg = Configuration::load();

    if (esp_reset_reason() != ESP_RST_DEEPSLEEP)
    {
        const bool unconfigured = cfg.wifi_ssid.empty() || cfg.main_topic.empty();
        if (config_jumper_closed() || unconfigured)
        {
            ESP_LOGW(TAG, "Tryb konfiguracji (siec '%s'): %s", config_defaults::AP_SSID,
                     unconfigured ? "brak WiFi lub tokenu" : "D3 zwarte z GND");
            ConfigPortal::run(config_defaults::CONFIG_MODE_TIMEOUT_S); // po zapisaniu ustawien restartuje urzadzenie
            cfg = Configuration::load();
        }
    }

    const uint32_t period_s = Configuration::report_period_s(cfg);
    if (period_s > 0)
        run_battery(cfg, period_s); // konczy sie deep sleep
    else
        run_online(cfg);
}
