#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

class ConfigPortal
{
public:
    // Wystawia siec WiFi (AP) i prosta strone konfiguracyjna pod adresem 192.168.4.1
    // na timeout_s sekund. Po zapisaniu ustawien urzadzenie sie restartuje.
    // Po uplywie czasu bez zapisu funkcja konczy sie i wylacza siec.
    static void run(int timeout_s);
};

#endif
