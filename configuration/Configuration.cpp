#include "configuration/Configuration.hpp"
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
    cfg.main_topic = config_defaults::MAIN_TOPIC;
    cfg.imp_per_kwh = config_defaults::IMP_PER_KWH;
    cfg.energy_price = config_defaults::ENERGY_PRICE;
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
    storage->read_id(cfg.main_topic);
    storage->read_imp_per_kwh(cfg.imp_per_kwh);
    storage->read_price(cfg.energy_price);
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
    if (cfg.main_topic.empty())
        cfg.main_topic = def.main_topic;
    if (cfg.imp_per_kwh.empty())
        cfg.imp_per_kwh = def.imp_per_kwh;
    if (cfg.energy_price.empty())
        cfg.energy_price = def.energy_price;
    return cfg;
}

esp_err_t Configuration::save(const DeviceConfig &cfg)
{
    Storage *storage = Storage::GetInstance();
    if (storage->save_wifi_settings(cfg.wifi_ssid, cfg.wifi_pass) == ESP_OK &&
        storage->save_mqtt(cfg.mqtt_broker) == ESP_OK &&
        storage->save_login(cfg.mqtt_login) == ESP_OK &&
        storage->save_pass(cfg.mqtt_pass) == ESP_OK &&
        storage->save_id(cfg.main_topic) == ESP_OK &&
        storage->save_imp_per_kwh(cfg.imp_per_kwh) == ESP_OK &&
        storage->save_price(cfg.energy_price) == ESP_OK)
    {
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool Configuration::parse_price(const std::string &text, double &price)
{
    std::string t = text;
    for (char &c : t)
        if (c == ',')
            c = '.';

    char *end = nullptr;
    const double v = strtod(t.c_str(), &end);
    while (end && (*end == ' ' || *end == '\r' || *end == '\n'))
        ++end;
    if (t.empty() || end == t.c_str() || *end != '\0' || !(v > 0.0) || v > 1000.0)
        return false;

    price = v;
    return true;
}

bool Configuration::parse_imp_per_kwh(const std::string &text, uint32_t &imp)
{
    char *end = nullptr;
    const unsigned long v = strtoul(text.c_str(), &end, 10);
    while (end && (*end == ' ' || *end == '\r' || *end == '\n'))
        ++end;
    if (text.empty() || end == text.c_str() || *end != '\0' || v < 1 || v > 100000)
        return false;

    imp = (uint32_t)v;
    return true;
}

uint32_t Configuration::imp_per_kwh(const DeviceConfig &cfg)
{
    uint32_t imp = 0;
    if (!parse_imp_per_kwh(cfg.imp_per_kwh, imp))
        parse_imp_per_kwh(config_defaults::IMP_PER_KWH, imp);
    return imp;
}
