#ifndef STORAGE_H
#define STORAGE_H

#include <cstdint>
#include <string>
#include "esp_err.h"
#include "nvs.h"

// Singleton do zapisu ustawien w pamieci NVS (jedna przestrzen nazw "config").
// Funkcje read_* zwracaja ESP_OK, gdy wartosc istnieje; w przeciwnym razie
// wynik jest pusty, a funkcja zwraca blad.
class Storage
{
public:
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    static Storage *GetInstance();

    esp_err_t save_wifi_settings(const std::string &ssid, const std::string &pass);
    esp_err_t read_wifi_settings(std::string &ssid, std::string &pass);

    esp_err_t save_token(const std::string &token);
    esp_err_t read_token(std::string &token);

    esp_err_t save_mqtt(const std::string &broker);
    esp_err_t read_mqtt(std::string &broker);

    esp_err_t save_login(const std::string &login);
    esp_err_t read_login(std::string &login);

    esp_err_t save_pass(const std::string &pass);
    esp_err_t read_pass(std::string &pass);

    // Cena 1 kWh (tekst, np. "1.10").
    esp_err_t save_price(const std::string &price);
    esp_err_t read_price(std::string &price);

    // Liczba impulsow diody licznika na 1 kWh (tekst, np. "1000").
    esp_err_t save_imp_per_kwh(const std::string &imp);
    esp_err_t read_imp_per_kwh(std::string &imp);

    // Strategia wysylania: okres raportowania w sekundach (tekst, np. "300"; "0" = zasilanie sieciowe).
    esp_err_t save_report_period(const std::string &period);
    esp_err_t read_report_period(std::string &period);

    // Licznik impulsow diody (1 impuls = 1/imp_per_kwh kWh).
    esp_err_t save_pulses(uint32_t pulses);
    esp_err_t read_pulses(uint32_t &pulses);

private:
    Storage();

    esp_err_t set_str(const char *key, const std::string &value);
    esp_err_t get_str(const char *key, std::string &value);

    nvs_handle_t handle_ = 0;
};

#endif
