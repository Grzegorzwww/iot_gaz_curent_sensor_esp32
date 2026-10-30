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

 #define ESP32_C6 1

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
    // Temat MQTT = <token> + TOPIC_SUFFIX, np. "ab12cd34.../zurzycie_pradu".
    inline constexpr const char *TOPIC_SUFFIX = "/zurzycie_pradu";

    // --- Licznik energii: impulsy diody na 1 kWh (typowo 1000, 2000, 10000) ---
    inline constexpr const char *IMP_PER_KWH = "1000";

    // --- Cena 1 kWh [zl]. Puste = koszty nie sa publikowane. ---
    inline constexpr const char *ENERGY_PRICE = "";

    // --- Zasilanie / strategia wysylania (wybierana w portalu) ---
    // Sekundy: "0" = zasilanie sieciowe (domyslnie): WiFi i MQTT stale polaczone, raport co kilkanascie sekund.
    // Inaczej bateria: ESP32-C6 spi w deep sleep, impulsy liczy rdzen LP, a co tyle sekund urzadzenie
    //      budzi sie, laczy z WiFi/MQTT, wysyla raport i znow zasypia. Dozwolone: 60, 180, 300, 600, 3600.
    inline constexpr const char *REPORT_PERIOD = "0";

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
    std::string main_topic; // wyliczane z token: token + "/zurzycie_pradu" (puste, gdy token pusty)
    std::string imp_per_kwh;  // impulsow na kWh, np. "1000"
    std::string energy_price; // cena 1 kWh w zl, np. "1.10" lub "1,10"
    std::string report_period; // strategia wysylania w sekundach: "0" (siec), "60", "180", "300", "600", "3600"
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

    // Parsuje cene (przecinek lub kropka, 0 < cena <= 1000). Zwraca false dla pustej/nieprawidlowej.
    static bool parse_price(const std::string &text, double &price);

    // Parsuje liczbe impulsow na kWh (liczba calkowita 1..100000).
    static bool parse_imp_per_kwh(const std::string &text, uint32_t &imp);

    // Liczba impulsow na kWh z konfiguracji (przy bledzie/pustej: 1000).
    static uint32_t imp_per_kwh(const DeviceConfig &cfg);

    // Parsuje okres raportowania [s]: dozwolone 0 (zasilanie sieciowe), 60, 180, 300, 600, 3600.
    static bool parse_report_period(const std::string &text, uint32_t &period_s);

    // Okres raportowania z konfiguracji (przy bledzie/pustej: wartosc domyslna).
    static uint32_t report_period_s(const DeviceConfig &cfg);

    // Czy token ma poprawny format (6-64 znakow: litery, cyfry, '_', '-'). Pusty = nieskonfigurowany (OK).
    static bool valid_token(const std::string &token);

    // Temat MQTT z tokenu: token + "/zurzycie_pradu". Pusty token -> pusty temat (nic nie publikujemy).
    static std::string build_topic(const std::string &token);
};

#endif
