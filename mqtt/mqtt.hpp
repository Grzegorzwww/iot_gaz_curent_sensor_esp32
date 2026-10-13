#ifndef MQTT_H
#define MQTT_H

#include <functional>
#include <string>
#include "mqtt_client.h"
#include "configuration/Configuration.hpp"

// Klient MQTT (TLS / WSS) z uwierzytelnianiem login+haslo.
// Certyfikat serwera: certyficate/isrgrootx1.pem (osadzony w firmware).
class Mqtt
{
public:
    explicit Mqtt(const DeviceConfig &cfg);
    ~Mqtt();

    // Wywolywane (w watku MQTT) dla wiadomosci z main_topic + "/" + subtopic.
    // Wiadomosci retained TEZ sa przekazywane (urzadzenie wiekszosc czasu spi, wiec komenda
    // musi poczekac na broker); po obsluzeniu nalezy ja wyczyscic pustym retained.
    using MessageHandler = std::function<void(const std::string &subtopic, const std::string &payload)>;
    void on_message(MessageHandler handler) { handler_ = std::move(handler); }

    bool start();
    void stop();
    bool is_connected() const { return connected_; }

    // Blokuje do polaczenia z brokerem lub uplywu timeout_ms.
    bool wait_connected(int timeout_ms);

    // Blokuje do wyslania i potwierdzenia wszystkich wiadomosci (pusta kolejka) lub uplywu timeout_ms.
    bool wait_flushed(int timeout_ms);

    // Publikuje na main_topic + "/" + subtopic (pusty subtopic = main_topic).
    bool publish(const std::string &subtopic, const std::string &message, int qos = 0, bool retain = false);

private:
    static void event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

    // Lancuchy musza zyc tak dlugo jak klient.
    std::string uri_;
    std::string username_;
    std::string password_;
    std::string main_topic_;

    MessageHandler handler_;
    esp_mqtt_client_handle_t client_ = nullptr;
    volatile bool connected_ = false;
};

#endif
