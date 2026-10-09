#include <string>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_portal/config_portal.hpp"
#include "configuration/Configuration.hpp"
#include "mqtt/mqtt.hpp"
#include "storage/storage.hpp"
#include "wifi_manager.h"

static const char *TAG = "MAIN";

static const int WIFI_CONNECT_TIMEOUT_MS = 20000;
static const int STATUS_PERIOD_MS = 30000;

extern "C" void app_main(void)
{
    Storage::GetInstance(); // inicjalizuje NVS

    // Po starcie przez chwile wystawiamy siec konfiguracyjna.
    // Gdy nie ma jeszcze zadnego SSID, portal dziala az do zapisania ustawien.
    DeviceConfig cfg;
    for (;;)
    {
        ConfigPortal::run(config_defaults::CONFIG_MODE_TIMEOUT_S);
        cfg = Configuration::load();
        if (!cfg.wifi_ssid.empty())
            break;
        ESP_LOGW(TAG, "Brak SSID WiFi - uruchamiam ponownie tryb konfiguracji");
    }

    if (!wifi_manager_connect_sta(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str(), WIFI_CONNECT_TIMEOUT_MS))
    {
        ESP_LOGE(TAG, "Nie mozna polaczyc z WiFi - restart");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    static Mqtt mqtt(cfg);
    if (!mqtt.start())
    {
        ESP_LOGE(TAG, "Start MQTT nie powiodl sie");
    }

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
        if (mqtt.is_connected())
            mqtt.publish("status", "uptime_s=" + std::to_string(esp_timer_get_time() / 1000000));
    }
}
