#include "wifi_manager.h"

#include <algorithm>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI";

static const EventBits_t CONNECTED_BIT = BIT0;
static const EventBits_t FAIL_BIT = BIT1;
static const int INITIAL_MAX_RETRY = 5;

static EventGroupHandle_t s_events = nullptr;
static bool s_inited = false;
static volatile bool s_sta_wanted = false;
static volatile bool s_initial_connect = false;
static volatile int s_retry = 0;

static void event_handler(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            if (s_sta_wanted)
                esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (!s_sta_wanted)
                break;
            if (s_initial_connect && s_retry >= INITIAL_MAX_RETRY)
            {
                xEventGroupSetBits(s_events, FAIL_BIT);
            }
            else
            {
                s_retry = s_retry + 1;
                ESP_LOGW(TAG, "Disconnected, reason %d, retrying (%d)",
                         static_cast<wifi_event_sta_disconnected_t *>(data)->reason, (int)s_retry);
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP: client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "AP: client disconnected");
            break;
        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

static bool wifi_init_once()
{
    if (s_inited)
        return true;

    s_events = xEventGroupCreate();
    if (!s_events)
        return false;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr));

    s_inited = true;
    return true;
}

void wifi_manager_stop(void)
{
    if (!s_inited)
        return;
    s_sta_wanted = false;
    esp_wifi_stop();
}

// Diagnostyka: wypisuje sieci widoczne dla urzadzenia (SSID w cudzyslowach pokazuje spacje na koncu).
static void log_visible_networks()
{
    wifi_scan_config_t scan = {};
    scan.show_hidden = true;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK)
    {
        ESP_LOGW(TAG, "Scan failed");
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    uint16_t n = count > 20 ? 20 : count;
    wifi_ap_record_t records[20];
    if (esp_wifi_scan_get_ap_records(&n, records) != ESP_OK)
        return;

    ESP_LOGI(TAG, "Scan: %u networks visible (2.4 GHz only)", (unsigned)count);
    for (uint16_t i = 0; i < n; ++i)
    {
        ESP_LOGI(TAG, "  '%s' ch%d rssi %d auth %d", reinterpret_cast<const char *>(records[i].ssid),
                 records[i].primary, records[i].rssi, (int)records[i].authmode);
    }
}

bool wifi_manager_connect_sta(const char *ssid, const char *pass, int timeout_ms)
{
    if (!wifi_init_once())
        return false;

    esp_wifi_stop();
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);

    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid));
    strlcpy(reinterpret_cast<char *>(cfg.sta.password), pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    s_retry = 0;
    s_initial_connect = true;
    s_sta_wanted = true;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK)
    {
        s_sta_wanted = false;
        return false;
    }

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    s_initial_connect = false;

    if (bits & CONNECTED_BIT)
        return true;

    ESP_LOGE(TAG, "Connection to '%s' failed", ssid);
    s_sta_wanted = false; // nie pozwol zdarzeniom rozlaczenia wznawiac laczenia w trakcie skanu
    log_visible_networks();
    wifi_manager_stop();
    return false;
}

bool wifi_manager_start_ap(const char *ssid, const char *pass)
{
    if (!wifi_init_once())
        return false;

    esp_wifi_stop();
    s_sta_wanted = false;

    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char *>(cfg.ap.ssid), ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    if (strlen(pass) >= 8)
    {
        strlcpy(reinterpret_cast<char *>(cfg.ap.password), pass, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    return esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK &&
           esp_wifi_set_config(WIFI_IF_AP, &cfg) == ESP_OK &&
           esp_wifi_start() == ESP_OK;
}

int wifi_manager_ap_clients(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK)
        return 0;
    return list.num;
}

std::vector<std::string> wifi_manager_scan_networks()
{
    std::vector<std::string> result;
    if (!wifi_init_once())
        return result;

    // Skan wymaga aktywnego interfejsu STA. W portalu konfiguracyjnym dzialamy w trybie AP,
    // wiec na czas skanu przelaczamy sie na APSTA (polaczeni klienci AP nie sa rozlaczani),
    // po czym wracamy do trybu AP.
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const bool was_ap_only = (mode == WIFI_MODE_AP);
    if (was_ap_only)
        esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    const esp_err_t err = esp_wifi_scan_start(&scan, true);

    if (err == ESP_OK)
    {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        uint16_t n = count > 30 ? 30 : count;
        if (n > 0)
        {
            std::vector<wifi_ap_record_t> records(n);
            if (esp_wifi_scan_get_ap_records(&n, records.data()) == ESP_OK)
            {
                std::sort(records.begin(), records.begin() + n,
                          [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });
                for (uint16_t i = 0; i < n; ++i)
                {
                    const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
                    if (ssid[0] == '\0')
                        continue;
                    std::string name(ssid);
                    bool dup = false;
                    for (const auto &s : result)
                    {
                        if (s == name)
                        {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup)
                        result.push_back(name);
                }
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
    }

    if (was_ap_only)
        esp_wifi_set_mode(WIFI_MODE_AP);

    return result;
}
