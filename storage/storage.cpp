#include "storage/storage.hpp"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "STORAGE";
static const char *NAMESPACE = "config";

static const char *KEY_SSID = "ssid";
static const char *KEY_WIFI_PASS = "wifi_pass";
static const char *KEY_BROKER = "broker";
static const char *KEY_LOGIN = "login";
static const char *KEY_MQTT_PASS = "mqtt_pass";
static const char *KEY_TOKEN = "token";
static const char *KEY_PULSES = "pulses";
static const char *KEY_PRICE = "price";
static const char *KEY_IMP = "imp_kwh";
static const char *KEY_PERIOD = "period_s";

Storage::Storage()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open(NAMESPACE, NVS_READWRITE, &handle_);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
}

Storage *Storage::GetInstance()
{
    static Storage instance;
    return &instance;
}

esp_err_t Storage::set_str(const char *key, const std::string &value)
{
    esp_err_t err = nvs_set_str(handle_, key, value.c_str());
    if (err == ESP_OK)
        err = nvs_commit(handle_);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "save '%s' failed: %s", key, esp_err_to_name(err));
    return err;
}

esp_err_t Storage::get_str(const char *key, std::string &value)
{
    value.clear();
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle_, key, nullptr, &len);
    if (err != ESP_OK)
        return err;

    std::string buf(len, '\0');
    err = nvs_get_str(handle_, key, &buf[0], &len);
    if (err != ESP_OK)
        return err;

    buf.resize(len > 0 ? len - 1 : 0); // bez konczacego '\0'
    value = std::move(buf);
    return ESP_OK;
}

esp_err_t Storage::save_wifi_settings(const std::string &ssid, const std::string &pass)
{
    esp_err_t err = set_str(KEY_SSID, ssid);
    if (err != ESP_OK)
        return err;
    return set_str(KEY_WIFI_PASS, pass);
}

esp_err_t Storage::read_wifi_settings(std::string &ssid, std::string &pass)
{
    esp_err_t e1 = get_str(KEY_SSID, ssid);
    esp_err_t e2 = get_str(KEY_WIFI_PASS, pass);
    return e1 != ESP_OK ? e1 : e2;
}

esp_err_t Storage::save_token(const std::string &token) { return set_str(KEY_TOKEN, token); }
esp_err_t Storage::read_token(std::string &token) { return get_str(KEY_TOKEN, token); }

esp_err_t Storage::save_mqtt(const std::string &broker) { return set_str(KEY_BROKER, broker); }
esp_err_t Storage::read_mqtt(std::string &broker) { return get_str(KEY_BROKER, broker); }

esp_err_t Storage::save_login(const std::string &login) { return set_str(KEY_LOGIN, login); }
esp_err_t Storage::read_login(std::string &login) { return get_str(KEY_LOGIN, login); }

esp_err_t Storage::save_pass(const std::string &pass) { return set_str(KEY_MQTT_PASS, pass); }
esp_err_t Storage::read_pass(std::string &pass) { return get_str(KEY_MQTT_PASS, pass); }

esp_err_t Storage::save_price(const std::string &price) { return set_str(KEY_PRICE, price); }
esp_err_t Storage::read_price(std::string &price) { return get_str(KEY_PRICE, price); }

esp_err_t Storage::save_imp_per_kwh(const std::string &imp) { return set_str(KEY_IMP, imp); }
esp_err_t Storage::read_imp_per_kwh(std::string &imp) { return get_str(KEY_IMP, imp); }

esp_err_t Storage::save_report_period(const std::string &period) { return set_str(KEY_PERIOD, period); }
esp_err_t Storage::read_report_period(std::string &period) { return get_str(KEY_PERIOD, period); }

esp_err_t Storage::save_pulses(uint32_t pulses)
{
    esp_err_t err = nvs_set_u32(handle_, KEY_PULSES, pulses);
    if (err == ESP_OK)
        err = nvs_commit(handle_);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "save pulses failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t Storage::read_pulses(uint32_t &pulses)
{
    return nvs_get_u32(handle_, KEY_PULSES, &pulses);
}
