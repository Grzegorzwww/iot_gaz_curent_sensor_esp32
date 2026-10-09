#ifndef MQTT_H
#define MQTT_H

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

    bool start();
    void stop();
    bool is_connected() const { return connected_; }

    // Publikuje na main_topic + "/" + subtopic (pusty subtopic = main_topic).
    bool publish(const std::string &subtopic, const std::string &message, int qos = 0, bool retain = false);

private:
    static void event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

    // Lancuchy musza zyc tak dlugo jak klient.
    std::string uri_;
    std::string username_;
    std::string password_;
    std::string main_topic_;

    esp_mqtt_client_handle_t client_ = nullptr;
    volatile bool connected_ = false;
};

#endif
