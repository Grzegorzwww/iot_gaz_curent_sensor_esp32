#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <string>
#include "esp_err.h"

/*
 * Domyslne ustawienia urzadzenia.
 * Sa uzywane dopiero wtedy, gdy uzytkownik nie wpisal wlasnych wartosci
 * na stronie konfiguracyjnej (wartosci z pamieci NVS maja pierwszenstwo).
 */
namespace config_defaults
{
    // --- WiFi, z ktorym urzadzenie laczy sie na co dzien ---
    inline constexpr const char *WIFI_SSID = "";
    inline constexpr const char *WIFI_PASS = "";

    // --- MQTT ---
    inline constexpr const char *MQTT_BROKER = "wss://mqtt.warcgr.pl:443/mqtt";
    inline constexpr const char *MQTT_LOGIN = "bob";
    inline constexpr const char *MQTT_PASS = "Bobik111"; // uzupelnij haslo (albo wpisz na stronie konfiguracyjnej)
    inline constexpr const char *MAIN_TOPIC = "piec_weglowy/ruszta";

    // --- Tryb konfiguracji (siec wystawiana przez urzadzenie po restarcie) ---
    inline constexpr const char *AP_SSID = "ESP32Config";
    inline constexpr const char *AP_PASS = "22446688"; // min. 8 znakow
    inline constexpr int CONFIG_MODE_TIMEOUT_S = 60;
} // namespace config_defaults

struct DeviceConfig
{
    std::string wifi_ssid;
    std::string wifi_pass;
    std::string mqtt_broker;
    std::string mqtt_login;
    std::string mqtt_pass;
    std::string main_topic;
};

class Configuration
{
public:
    // Wartosci domyslne z config_defaults.
    static DeviceConfig defaults();

    // Wartosci zapisane przez uzytkownika (pusty string = nie ustawiono).
    static DeviceConfig stored();

    // Wartosci do uzycia: zapisane przez uzytkownika, a gdy brak - domyslne.
    static DeviceConfig load();

    // Zapisuje wartosci uzytkownika w pamieci NVS (pusty string = uzyj domyslnej).
    static esp_err_t save(const DeviceConfig &cfg);
};

#endif
