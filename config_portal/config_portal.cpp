#include "config_portal/config_portal.hpp"
#include "configuration/Configuration.hpp"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

static const char *TAG = "CONFIG_PORTAL";

static const size_t MAX_BODY = 1024;
static const size_t MAX_SSID = 32;
static const size_t MAX_WIFI_PASS = 63;
static const size_t MAX_FIELD = 127;
static const int CLIENT_GRACE_S = 10;

static volatile bool settings_saved = false;

static std::string url_decode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        char c = in[i];
        if (c == '+')
        {
            out += ' ';
        }
        else if (c == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2]))
        {
            out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        }
        else
        {
            out += c;
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_form(const std::string &body)
{
    std::map<std::string, std::string> fields;
    size_t pos = 0;
    while (pos <= body.size())
    {
        size_t end = body.find('&', pos);
        if (end == std::string::npos)
            end = body.size();
        std::string pair = body.substr(pos, end - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos)
        {
            fields[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        pos = end + 1;
    }
    return fields;
}

static std::string html_escape(const std::string &in)
{
    std::string out;
    for (char c : in)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

static std::string input_field(const char *label, const char *name, const std::string &value,
                               const std::string &placeholder, size_t maxlen, bool password)
{
    return std::string("<label>") + label + "<input name=\"" + name + "\" type=\"" + (password ? "password" : "text") +
           "\" maxlength=\"" + std::to_string(maxlen) + "\" value=\"" + html_escape(value) +
           "\" placeholder=\"" + html_escape(placeholder) + "\" autocomplete=\"off\"></label>";
}

static std::string build_page(const std::string &message)
{
    const DeviceConfig def = Configuration::defaults();
    const DeviceConfig cur = Configuration::stored();

    std::string page =
        "<!DOCTYPE html><html lang=\"pl\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Konfiguracja</title><style>"
        "body{font-family:sans-serif;max-width:420px;margin:1em auto;padding:0 1em}"
        "label{display:block;margin-top:.8em;font-weight:bold}"
        "input{width:100%;padding:.5em;box-sizing:border-box}"
        "button{margin-top:1.2em;padding:.7em 1.2em}small{color:#555}"
        "</style></head><body><h2>Konfiguracja urz\xc4\x85" "dzenia</h2>";

    if (!message.empty())
        page += "<p><b>" + html_escape(message) + "</b></p>";

    page += "<form method=\"post\" action=\"/save\">";
    page += input_field("WiFi SSID", "ssid", cur.wifi_ssid, def.wifi_ssid, MAX_SSID, false);
    page += input_field("WiFi has\xc5\x82o", "wpass", "", "(bez zmian)", MAX_WIFI_PASS, true);
    page += input_field("MQTT broker (URI)", "broker", cur.mqtt_broker, def.mqtt_broker, MAX_FIELD, false);
    page += input_field("MQTT login", "login", cur.mqtt_login, def.mqtt_login, MAX_FIELD, false);
    page += input_field("MQTT has\xc5\x82o", "mpass", "", "(bez zmian)", MAX_FIELD, true);
    page += input_field("MAIN TOPIC", "topic", cur.main_topic, def.main_topic, MAX_FIELD, false);
    page += "<p><small>Puste pole = warto\xc5\x9b\xc4\x87 domy\xc5\x9blna (widoczna jako podpowied\xc5\xba).</small></p>"
            "<button type=\"submit\" name=\"action\" value=\"save\">Ustaw</button> "
            "<button type=\"submit\" name=\"action\" value=\"reset\">Przywr\xc3\xb3" "\xc4\x87 domy\xc5\x9blne</button>"
            "</form></body></html>";
    return page;
}

static esp_err_t send_html(httpd_req_t *req, const std::string &html)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_html(req, build_page(""));
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nieprawidlowe dane");
        return ESP_FAIL;
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size())
    {
        int r = httpd_req_recv(req, &body[received], body.size() - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0)
            return ESP_FAIL;
        received += r;
    }

    std::map<std::string, std::string> form = parse_form(body);
    const std::string action = form["action"];

    DeviceConfig cfg = Configuration::stored();
    if (action == "reset")
    {
        cfg = DeviceConfig{};
    }
    else
    {
        const std::string old_ssid = cfg.wifi_ssid;
        cfg.wifi_ssid = form["ssid"];
        cfg.mqtt_broker = form["broker"];
        cfg.mqtt_login = form["login"];
        cfg.main_topic = form["topic"];

        // Puste haslo = bez zmian (ale tylko gdy SSID sie nie zmienilo).
        if (!form["wpass"].empty() || cfg.wifi_ssid != old_ssid)
            cfg.wifi_pass = form["wpass"];
        if (!form["mpass"].empty())
            cfg.mqtt_pass = form["mpass"];
    }

    if (cfg.wifi_ssid.size() > MAX_SSID || cfg.wifi_pass.size() > MAX_WIFI_PASS ||
        cfg.mqtt_broker.size() > MAX_FIELD || cfg.mqtt_login.size() > MAX_FIELD ||
        cfg.mqtt_pass.size() > MAX_FIELD || cfg.main_topic.size() > MAX_FIELD)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Zbyt dlugie wartosci");
        return ESP_FAIL;
    }

    if (Configuration::save(cfg) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Blad zapisu");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Settings saved, restarting");
    esp_err_t res = send_html(req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                   "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
                                   "<body style=\"font-family:sans-serif\"><h3>Zapisano. Urz\xc4\x85" "dzenie si\xc4\x99 restartuje...</h3></body></html>");
    settings_saved = true;
    return res;
}

void ConfigPortal::run(int timeout_s)
{
    settings_saved = false;
    wifi_manager_start_ap(config_defaults::AP_SSID, config_defaults::AP_PASS);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        wifi_manager_stop();
        return;
    }

    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = root_get_handler;
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t save_uri = {};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = save_post_handler;
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "Config mode for %d s: WiFi '%s', open http://192.168.4.1", timeout_s, config_defaults::AP_SSID);

    // Po uplywie czasu portal dziala dalej, dopoki jest polaczony jakis klient.
    // Po jego odlaczeniu czekamy jeszcze CLIENT_GRACE_S (np. na ponowne polaczenie telefonu).
    int idle_ticks = 0;
    int elapsed_ticks = 0;
    while (!settings_saved)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        ++elapsed_ticks;

        if (wifi_manager_ap_clients() > 0)
        {
            idle_ticks = 0;
            continue;
        }
        ++idle_ticks;

        if (elapsed_ticks >= timeout_s * 10 && idle_ticks >= CLIENT_GRACE_S * 10)
            break;
    }

    if (settings_saved)
    {
        vTaskDelay(1500 / portTICK_PERIOD_MS); // czas na dostarczenie odpowiedzi do przegladarki
        esp_restart();
    }

    ESP_LOGI(TAG, "Config mode finished");
    httpd_stop(server);
    wifi_manager_stop();
}
