/*
 * Czujnik temperatury wody (LM35) - co REPORT_PERIOD_S publikuje odczyt na broker MQTT,
 * na temat <token>/temperatura_wody.
 *
 * Urzadzenie jest zasilane z sieci, wiec nie uzywa deep sleep: WiFi i MQTT sa stale polaczone.
 *
 *   start --> inicjalizacja ADC/czujnika
 *         --> D8 zwarte z GND przy starcie? --> tryb konfiguracji (siec AP + strona WWW)
 *         --> laczenie z WiFi + MQTT, odczyt i publikacja co REPORT_PERIOD_S
 *
 * Brak polaczenia dluzej niz OFFLINE_RESTART_S => restart.
 */
#include <cmath>
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
#include "mqtt/mqtt.hpp"
#include "storage/storage.hpp"
#include "temperature_sensor/temperature_sensor.hpp"
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
static constexpr int64_t WIFI_RETRY_S = 30;    // przerwa miedzy nieudanymi probami polaczenia
static constexpr int64_t REPORT_PERIOD_S = 30; // co ile publikujemy odczyt temperatury
static constexpr int64_t OFFLINE_RESTART_S = 15 * 60;

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

static bool report(Mqtt &mqtt)
{
    const float temp_c = TemperatureSensor::read_celsius();
    if (std::isnan(temp_c))
    {
        ESP_LOGW(TAG, "Odczyt temperatury nieudany - pomijam raport");
        return false;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", temp_c);
    const bool ok = mqtt.publish("", buf, 0, true);
    ESP_LOGI(TAG, "Raport: %s C, %s", buf, ok ? "OK" : "BLAD");
    return ok;
}

static void run_online(const DeviceConfig &cfg)
{
    std::unique_ptr<Mqtt> mqtt;
    int64_t last_report = -REPORT_PERIOD_S;
    int64_t offline_since = uptime_s();
    int64_t next_wifi_try = 0;

    if (cfg.wifi_ssid.empty())
        ESP_LOGW(TAG, "Brak SSID WiFi. Ustaw WiFi w trybie konfiguracji (zwarcie D8-GND przy starcie)");
    if (cfg.main_topic.empty())
        ESP_LOGW(TAG, "Brak tokenu urzadzenia - dane nie beda publikowane. Zeskanuj/wklej token z api.warcgr.pl w trybie konfiguracji");

    for (;;)
    {
        int64_t now = uptime_s();

        if (mqtt && mqtt->is_connected())
        {
            offline_since = now;
            if (now - last_report >= REPORT_PERIOD_S)
            {
                report(*mqtt);
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

extern "C" void app_main(void)
{
    Storage::GetInstance(); // inicjalizuje NVS

    DeviceConfig cfg = Configuration::load();

    if (!TemperatureSensor::init())
        ESP_LOGE(TAG, "Inicjalizacja czujnika temperatury nieudana");

    if (config_jumper_closed())
    {
        ESP_LOGW(TAG, "D8 zwarte z GND - tryb konfiguracji (siec '%s')", config_defaults::AP_SSID);
        ConfigPortal::run(config_defaults::CONFIG_MODE_TIMEOUT_S); // po zapisaniu ustawien restartuje urzadzenie
        cfg = Configuration::load();
    }

    run_online(cfg);
}
