#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Laczy z siecia WiFi (tryb STA). Blokuje do uzyskania IP albo do uplywu timeout_ms.
// Po udanym polaczeniu rozlaczenia sa automatycznie ponawiane.
bool wifi_manager_connect_sta(const char *ssid, const char *pass, int timeout_ms);

// Wystawia siec (tryb AP). Pusty/krotszy niz 8 znakow pass => siec otwarta.
bool wifi_manager_start_ap(const char *ssid, const char *pass);

// Liczba klientow polaczonych z AP.
int wifi_manager_ap_clients(void);

// Zatrzymuje WiFi (STA/AP).
void wifi_manager_stop(void);

#ifdef __cplusplus
}

#include <string>
#include <vector>

// Zwraca liste unikalnych SSID widocznych w poblizu (WiFi 2.4GHz), posortowanych malejaco
// wedlug sily sygnalu. Dziala rowniez w trakcie pracy w trybie AP (portal konfiguracyjny) -
// na czas skanu urzadzenie chwilowo przelacza sie na tryb APSTA, po czym wraca do AP.
std::vector<std::string> wifi_manager_scan_networks();
#endif

#endif
