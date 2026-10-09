#ifndef STORAGE_H
#define STORAGE_H

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

    esp_err_t save_id(const std::string &id);
    esp_err_t read_id(std::string &id);

    esp_err_t save_mqtt(const std::string &broker);
    esp_err_t read_mqtt(std::string &broker);

    esp_err_t save_login(const std::string &login);
    esp_err_t read_login(std::string &login);

    esp_err_t save_pass(const std::string &pass);
    esp_err_t read_pass(std::string &pass);

private:
    Storage();

    esp_err_t set_str(const char *key, const std::string &value);
    esp_err_t get_str(const char *key, std::string &value);

    nvs_handle_t handle_ = 0;
};

#endif
