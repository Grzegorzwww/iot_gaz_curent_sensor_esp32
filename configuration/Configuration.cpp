#include "configuration/Configuration.hpp"
#include <cctype>
#include <cstdlib>
#include "storage/storage.hpp"

DeviceConfig Configuration::defaults()
{
    DeviceConfig cfg;
    cfg.wifi_ssid = config_defaults::WIFI_SSID;
    cfg.wifi_pass = config_defaults::WIFI_PASS;
    cfg.mqtt_broker = config_defaults::MQTT_BROKER;
    cfg.mqtt_login = config_defaults::MQTT_LOGIN;
    cfg.mqtt_pass = config_defaults::MQTT_PASS;
    cfg.token = config_defaults::TOKEN;
    cfg.main_topic = build_topic(cfg.token);
    cfg.report_period = config_defaults::REPORT_PERIOD;
    return cfg;
}

DeviceConfig Configuration::stored()
{
    Storage *storage = Storage::GetInstance();
    DeviceConfig cfg;
    storage->read_wifi_settings(cfg.wifi_ssid, cfg.wifi_pass);
    storage->read_mqtt(cfg.mqtt_broker);
    storage->read_login(cfg.mqtt_login);
    storage->read_pass(cfg.mqtt_pass);
    storage->read_token(cfg.token);
    cfg.main_topic = build_topic(cfg.token);
    storage->read_report_period(cfg.report_period);
    return cfg;
}

DeviceConfig Configuration::load()
{
    const DeviceConfig def = defaults();
    DeviceConfig cfg = stored();

    // SSID i haslo WiFi traktowane jako para (siec otwarta ma puste haslo).
    if (cfg.wifi_ssid.empty())
    {
        cfg.wifi_ssid = def.wifi_ssid;
        cfg.wifi_pass = def.wifi_pass;
    }
    if (cfg.mqtt_broker.empty())
        cfg.mqtt_broker = def.mqtt_broker;
    if (cfg.mqtt_login.empty())
        cfg.mqtt_login = def.mqtt_login;
    if (cfg.mqtt_pass.empty())
        cfg.mqtt_pass = def.mqtt_pass;
    if (cfg.token.empty())
        cfg.token = def.token;
    cfg.main_topic = build_topic(cfg.token);
    if (cfg.report_period.empty())
        cfg.report_period = def.report_period;
    return cfg;
}

esp_err_t Configuration::save(const DeviceConfig &cfg)
{
    Storage *storage = Storage::GetInstance();
    if (storage->save_wifi_settings(cfg.wifi_ssid, cfg.wifi_pass) == ESP_OK &&
        storage->save_mqtt(cfg.mqtt_broker) == ESP_OK &&
        storage->save_login(cfg.mqtt_login) == ESP_OK &&
        storage->save_pass(cfg.mqtt_pass) == ESP_OK &&
        storage->save_token(cfg.token) == ESP_OK &&
        storage->save_report_period(cfg.report_period) == ESP_OK)
    {
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool Configuration::parse_report_period(const std::string &text, uint32_t &period_s)
{
    char *end = nullptr;
    const unsigned long v = strtoul(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0')
        return false;
    if (v != 0 && v != 60 && v != 180 && v != 300 && v != 600 && v != 1800 && v != 3600)
        return false;

    period_s = (uint32_t)v;
    return true;
}

uint32_t Configuration::report_period_s(const DeviceConfig &cfg)
{
    uint32_t period = 0;
    if (!parse_report_period(cfg.report_period, period))
        parse_report_period(config_defaults::REPORT_PERIOD, period);
    return period;
}

bool Configuration::valid_token(const std::string &token)
{
    if (token.size() < 6 || token.size() > 64)
        return false;
    for (char c : token)
    {
        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
            return false;
    }
    return true;
}

std::string Configuration::build_topic(const std::string &token)
{
    if (token.empty())
        return "";
    return token + config_defaults::TOPIC_SUFFIX;
}
