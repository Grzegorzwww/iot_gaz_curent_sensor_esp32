#include "mqtt/mqtt.hpp"
#include <cstring>
#include "esp_log.h"

static const char *TAG = "MQTT";

// Osadzone przez EMBED_TXTFILES (main/CMakeLists.txt); TXT => konczy sie '\0'.
extern const uint8_t server_cert_pem_start[] asm("_binary_isrgrootx1_pem_start");

Mqtt::Mqtt(const DeviceConfig &cfg)
    : uri_(cfg.mqtt_broker), username_(cfg.mqtt_login), password_(cfg.mqtt_pass), main_topic_(cfg.main_topic)
{
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri_.c_str();
    mqtt_cfg.broker.verification.certificate = reinterpret_cast<const char *>(server_cert_pem_start);
    mqtt_cfg.credentials.username = username_.c_str();
    mqtt_cfg.credentials.authentication.password = password_.c_str();

    client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (client_)
        esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, event_handler, this);
    else
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
}

Mqtt::~Mqtt()
{
    if (client_)
    {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
    }
}

bool Mqtt::start()
{
    return client_ && esp_mqtt_client_start(client_) == ESP_OK;
}

void Mqtt::stop()
{
    if (client_)
        esp_mqtt_client_stop(client_);
    connected_ = false;
}

bool Mqtt::publish(const std::string &subtopic, const std::string &message, int qos, bool retain)
{
    if (!client_ || !connected_)
        return false;
    const std::string topic = subtopic.empty() ? main_topic_ : main_topic_ + "/" + subtopic;
    return esp_mqtt_client_publish(client_, topic.c_str(), message.c_str(), message.size(), qos, retain) >= 0;
}

void Mqtt::event_handler(void *handler_args, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto *self = static_cast<Mqtt *>(handler_args);
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (static_cast<esp_mqtt_event_id_t>(event_id))
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        self->connected_ = true;
        esp_mqtt_client_subscribe(self->client_, (self->main_topic_ + "/cmd").c_str(), 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        self->connected_ = false;
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "TOPIC=%.*s DATA=%.*s", event->topic_len, event->topic, event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG, "esp-tls err: 0x%x, tls stack err: 0x%x, errno: %d (%s)",
                     event->error_handle->esp_tls_last_esp_err, event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGE(TAG, "Connection refused, code: 0x%x", event->error_handle->connect_return_code);
        }
        break;

    default:
        break;
    }
}
