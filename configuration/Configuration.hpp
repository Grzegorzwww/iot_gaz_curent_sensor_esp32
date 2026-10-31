#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <cstdint>
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

    // --- Token urzadzenia z api.warcgr.pl (konto uzytkownika -> token -> ESP, 1 konto moze miec wiele ESP). ---
    // Brak domyslnego - urzadzenie bez tokenu nie publikuje danych (nie wiadomo, do kogo naleza).
    inline constexpr const char *TOKEN = "";
    // Temat MQTT = <token> + TOPIC_SUFFIX, np. "ab12cd34.../temperatura_domu".
    inline constexpr const char *TOPIC_SUFFIX = "/temperatura_domu";

    // --- Zasilanie / strategia wysylania (wybierana w portalu) ---
    // Sekundy: "0" = zasilanie sieciowe: WiFi i MQTT stale polaczone, odczyt co kilkanascie sekund.
    // Inaczej bateria (domyslnie): ESP32-S3 spi w deep sleep, a co tyle sekund budzi sie, mierzy BME680,
    //      laczy z WiFi/MQTT, wysyla raport i znow zasypia. Dozwolone: 60, 180, 300, 600, 1800, 3600.
    inline constexpr const char *REPORT_PERIOD = "300";

    // --- Tryb konfiguracji (siec wystawiana przez urzadzenie po restarcie) ---
    inline constexpr const char *AP_SSID = "ESP32Config";
    inline constexpr const char *AP_PASS = "22446688"; // min. 8 znakow
    inline constexpr int CONFIG_MODE_TIMEOUT_S = 180;
} // namespace config_defaults

struct DeviceConfig
{
    std::string wifi_ssid;
    std::string wifi_pass;
    std::string mqtt_broker;
    std::string mqtt_login;
    std::string mqtt_pass;
    std::string token;      // identyfikator urzadzenia z api.warcgr.pl (losowy, nie e-mail)
    std::string main_topic; // wyliczane z token: token + "/temperatura_domu" (puste, gdy token pusty)
    std::string report_period; // strategia wysylania w sekundach: "0" (siec), "60", "180", "300", "600", "1800", "3600"
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

    // Parsuje okres raportowania [s]: dozwolone 0 (zasilanie sieciowe), 60, 180, 300, 600, 1800, 3600.
    static bool parse_report_period(const std::string &text, uint32_t &period_s);

    // Okres raportowania z konfiguracji (przy bledzie/pustej: wartosc domyslna).
    static uint32_t report_period_s(const DeviceConfig &cfg);

    // Czy token ma poprawny format (6-64 znakow: litery, cyfry, '_', '-'). Pusty = nieskonfigurowany (OK).
    static bool valid_token(const std::string &token);

    // Temat MQTT z tokenu: token + "/temperatura_domu". Pusty token -> pusty temat (nic nie publikujemy).
    static std::string build_topic(const std::string &token);
};

#endif
