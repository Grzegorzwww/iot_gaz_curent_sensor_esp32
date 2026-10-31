#include "config_portal/config_portal.hpp"
#include "bme680/bme680.hpp"
#include "configuration/Configuration.hpp"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static const char *TAG = "CONFIG_PORTAL";

// Osadzone przez EMBED_TXTFILES (main/CMakeLists.txt); TXT => konczy sie '\0'.
extern const uint8_t jsqr_js_start[] asm("_binary_jsQR_min_js_start");

static const size_t MAX_BODY = 1024;
static const size_t MAX_SSID = 32;
static const size_t MAX_WIFI_PASS = 63;
static const size_t MAX_FIELD = 127;
static const size_t MAX_TOKEN = 64;
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

// Prosty escaping do osadzenia tekstu w literale JSON (cudzyslow i backslash).
static std::string json_escape(const std::string &in)
{
    std::string out;
    for (char c : in)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

static std::string build_page(const std::string &message)
{
    const DeviceConfig def = Configuration::defaults();
    const DeviceConfig cur = Configuration::stored();

    std::string page =
        "<!DOCTYPE html><html lang=\"pl\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Konfiguracja</title><style>"
        ":root{--accent:#2563eb;--accent-dark:#1d4ed8;--bg:#eef2f7;--card:#fff;--text:#1e293b;--muted:#64748b;--border:#dbe3ec}"
        "*{box-sizing:border-box}"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;"
        "background:var(--bg);color:var(--text);margin:0;padding:2em 1em}"
        ".card{max-width:440px;margin:0 auto;background:var(--card);border-radius:16px;"
        "box-shadow:0 4px 20px rgba(15,23,42,.08);padding:1.6em 1.6em 1.8em}"
        "h2{margin:0 0 .15em;font-size:1.35em}"
        "p.sub{margin:0 0 1.2em;color:var(--muted);font-size:.9em}"
        "label{display:block;margin-top:1.1em;font-weight:600;font-size:.78em;color:var(--muted);"
        "text-transform:uppercase;letter-spacing:.04em}"
        "input,select{width:100%;padding:.6em .7em;margin-top:.35em;border:1px solid var(--border);"
        "border-radius:9px;font-size:1em;background:#fff;color:var(--text);font-family:inherit}"
        "input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(37,99,235,.15)}"
        "button{margin-top:1.2em;padding:.7em 1.2em;border:none;border-radius:9px;font-size:.95em;"
        "font-weight:600;cursor:pointer;background:var(--accent);color:#fff;transition:background .15s}"
        "button:hover{background:var(--accent-dark)}"
        "button.secondary{background:#e7edf5;color:var(--text)}"
        "button.secondary:hover{background:#d7e0ec}"
        "small{display:block;margin-top:.5em;color:var(--muted);font-size:.82em}"
        "#qr_status,#wifi_status{font-weight:500}"
        ".sensor{background:#f1f5f9;border-radius:9px;padding:.7em .9em;font-size:.92em;margin:0 0 .4em}"
        "hr{border:none;border-top:1px solid var(--border);margin:1.6em 0}"
        "</style></head><body><div class=\"card\">"
        "<h2>Konfiguracja urz\xc4\x85" "dzenia</h2>"
        "<p class=\"sub\">Ustawienia WiFi, MQTT i czujnika (BME680 / BME280 / BMP280)</p>";

    if (!message.empty())
        page += "<p><b>" + html_escape(message) + "</b></p>";

    // Aktualny odczyt z czujnika - pozwala od razu sprawdzic, czy BME680 jest dobrze podlaczony.
    {
        Bme680 sensor;
        Bme680Reading r = {};
        char line[200];
        if (sensor.begin() && sensor.read(r))
        {
            int n = snprintf(line, sizeof(line), "%s: <b>%.1f &deg;C</b>", sensor.chip_name(), r.temperature_c);
            if (r.has_humidity)
                n += snprintf(line + n, sizeof(line) - n, ", <b>%.1f %%</b>", r.humidity_pct);
            snprintf(line + n, sizeof(line) - n, ", <b>%.1f hPa</b>", r.pressure_hpa);
        }
        else
            snprintf(line, sizeof(line),
                     "Brak odczytu z czujnika. Sprawd&#378; po&#322;&#261;czenie: SDA = D4, SCL = D5, zasilanie 3V3.");
        page += std::string("<div class=\"sensor\">") + line + "</div>";
    }

    page += "<form method=\"post\" action=\"/save\">";
    page += "<label>WiFi SSID<input list=\"ssid_list\" id=\"ssid\" name=\"ssid\" type=\"text\" maxlength=\"" +
            std::to_string(MAX_SSID) + "\" value=\"" + html_escape(cur.wifi_ssid) + "\" placeholder=\"" +
            html_escape(def.wifi_ssid) + "\" autocomplete=\"off\"></label>"
            "<datalist id=\"ssid_list\"></datalist>"
            "<button type=\"button\" class=\"secondary\" onclick=\"scanWifi()\">Skanuj sieci</button>"
            "<small id=\"wifi_status\"></small>";
    page += input_field("WiFi has\xc5\x82o", "wpass", "", "(bez zmian)", MAX_WIFI_PASS, true);
    page += input_field("MQTT broker (URI)", "broker", cur.mqtt_broker, def.mqtt_broker, MAX_FIELD, false);

    // Token urzadzenia z api.warcgr.pl (konto uzytkownika -> token -> to ESP). Mozna wkleic recznie
    // albo zeskanowac zdjecie kodu QR z panelu (dekodowanie dziala w przegladarce, bez dostepu do internetu).
    page += "<label>Token urz\xc4\x85" "dzenia (z panelu api.warcgr.pl)"
            "<input id=\"token\" name=\"token\" type=\"text\" maxlength=\"" + std::to_string(MAX_TOKEN) +
            "\" value=\"" + html_escape(cur.token) + "\" placeholder=\"wklej token lub zeskanuj QR\" "
            "autocomplete=\"off\"></label>";
    page += "<input type=\"file\" accept=\"image/*\" capture=\"environment\" id=\"qr_file\" style=\"display:none\">"
            "<button type=\"button\" onclick=\"document.getElementById('qr_file').click()\">"
            "\xf0\x9f\x93\xb7 Zeskanuj token (zdj\xc4\x99" "cie kodu QR)</button>"
            "<small id=\"qr_status\"></small>";

    // Strategia wysylania danych: zasilanie sieciowe (polaczenie stale) albo bateria (deep sleep + raport co okres).
    // Wartosc pola to okres w sekundach; szacunek czasu pracy liczy skrypt w przegladarce (funkcja est()).
    const std::string period_sel = cur.report_period.empty() ? def.report_period : cur.report_period;
    struct PeriodOption
    {
        const char *value;
        const char *label;
    };
    static const PeriodOption period_options[] = {
        {"0", "Zasilanie sieciowe: po&#322;&#261;czenie sta&#322;e"},
        {"60", "Bateria: raport co 1 minut&#281;"},
        {"180", "Bateria: raport co 3 minuty"},
        {"300", "Bateria: raport co 5 minut (domy&#347;lnie)"},
        {"600", "Bateria: raport co 10 minut"},
        {"1800", "Bateria: raport co 30 minut"},
        {"3600", "Bateria: raport co 1 godzin&#281;"},
    };
    page += "<label>Strategia wysy&#322;ania danych<select id=\"period\" name=\"period\" onchange=\"est()\">";
    for (const PeriodOption &o : period_options)
    {
        page += std::string("<option value=\"") + o.value + "\"" + (period_sel == o.value ? " selected" : "") + ">" +
                o.label + "</option>";
    }
    page += "</select></label>"
            "<label>Pojemno&#347;&#263; ogniwa [mAh] (tylko do szacunku)"
            "<input id=\"cap\" type=\"number\" min=\"10\" max=\"20000\" step=\"10\" value=\"250\" oninput=\"est()\"></label>"
            "<small id=\"est\" style=\"font-weight:600;color:var(--text)\"></small>"
            "<small>Szacunek zak&#322;ada ok. 0,2 mAh na jeden raport (WiFi + MQTT) i 0,1 mA w u&#347;pieniu, "
            "z 80% pojemno&#347;ci ogniwa. Rzeczywisty czas zale&#380;y od zasi&#281;gu WiFi i uk&#322;adu zasilania. "
            "W trybie bateryjnym wej&#347;cie w t&#281; konfiguracj&#281; wymaga zwarcia D3 z GND i resetu p&#322;ytki.</small>";
    page += "<p><small>Puste pole = warto\xc5\x9b\xc4\x87 domy\xc5\x9blna (widoczna jako podpowied\xc5\xba).</small></p>"
            "<button type=\"submit\" name=\"action\" value=\"save\">Ustaw</button> "
            "<button type=\"submit\" name=\"action\" value=\"reset\" class=\"secondary\">Przywr\xc3\xb3" "\xc4\x87 domy\xc5\x9blne</button>"
            "</form></div>"
            "<script src=\"/jsqr.js\"></script>"
            "<script>"
            // Skanowanie QR z pliku/zdjecia dziala w pelni lokalnie (biblioteka jsQR osadzona w
            // urzadzeniu), wiec dziala takze przez zwykle HTTP bez dostepu do internetu.
            "document.getElementById('qr_file').addEventListener('change',function(e){"
            "var st=document.getElementById('qr_status');var f=e.target.files[0];if(!f)return;"
            "st.textContent='Skanowanie...';"
            "var reader=new FileReader();"
            "reader.onload=function(ev){"
            "var img=new Image();"
            "img.onload=function(){"
            "var maxDim=1000;"
            "var scale=Math.min(1,maxDim/Math.max(img.width,img.height));"
            "var w=Math.max(1,Math.round(img.width*scale));"
            "var h=Math.max(1,Math.round(img.height*scale));"
            "var canvas=document.createElement('canvas');"
            "canvas.width=w;canvas.height=h;"
            "var ctx=canvas.getContext('2d');"
            "ctx.drawImage(img,0,0,w,h);"
            "var data=ctx.getImageData(0,0,w,h);"
            "var code=null;"
            "try{code=jsQR(data.data,w,h);}catch(err){st.textContent='Blad dekodowania: '+err.message;return;}"
            "if(code&&code.data){document.getElementById('token').value=code.data;st.textContent='Zeskanowano.';}"
            "else{st.textContent='Nie znaleziono kodu QR. Sprobuj z blizszej odleglosci, przy lepszym swietle.';}"
            "};"
            "img.src=ev.target.result;"
            "};"
            "reader.readAsDataURL(f);"
            "});"
            "function scanWifi(){"
            "var st=document.getElementById('wifi_status');"
            "st.textContent='Skanowanie sieci...';"
            "fetch('/wifi_scan').then(function(r){return r.json();}).then(function(list){"
            "var dl=document.getElementById('ssid_list');"
            "dl.innerHTML='';"
            "list.forEach(function(ssid){var o=document.createElement('option');o.value=ssid;dl.appendChild(o);});"
            "st.textContent=list.length?('Znaleziono '+list.length+' sieci.'):'Brak wynikow w poblizu.';"
            "}).catch(function(){st.textContent='Blad skanowania sieci.';});"
            "}"
            "window.addEventListener('load',scanWifi);"
            // Szacowany czas pracy na jednym naladowaniu: Iavg = Qraport*3600/T + Isleep [mA], t = 0.8*C/Iavg.
            // Zasilanie sieciowe (T=0): urzadzenie trzyma WiFi caly czas, ok. 100 mA.
            "function est(){"
            "var T=+document.getElementById('period').value;"
            "var C=+document.getElementById('cap').value;"
            "var el=document.getElementById('est');"
            "if(!(C>0)){el.textContent='';return;}"
            "var I=T===0?100:0.2*3600/T+0.1;"
            "var h=0.8*C/I;"
            "var s=h<48?(Math.round(h*10)/10+' h'):(Math.round(h/24*10)/10+' dni');"
            "el.textContent=T===0?"
            "('To tryb dla zasilania sieciowego. Z ogniwa po\\u0142\\u0105czenie sta\\u0142e wystarczy\\u0142oby na ok. '+s+'.'):"
            "('Szacowany czas pracy na jednym na\\u0142adowaniu: ok. '+s+'.');"
            "}"
            "window.addEventListener('load',est);"
            "</script>"
            "</body></html>";
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

static esp_err_t jsqr_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, reinterpret_cast<const char *>(jsqr_js_start), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    const std::vector<std::string> networks = wifi_manager_scan_networks();
    std::string json = "[";
    for (size_t i = 0; i < networks.size(); ++i)
    {
        if (i)
            json += ',';
        json += '"';
        json += json_escape(networks[i]);
        json += '"';
    }
    json += ']';
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
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
        cfg.token = form["token"];
        // Usun biale znaki na brzegach (wklejanie ze schowka / skan QR czasem dokleja spacje/nowa linie).
        while (!cfg.token.empty() && std::isspace((unsigned char)cfg.token.front()))
            cfg.token.erase(cfg.token.begin());
        while (!cfg.token.empty() && std::isspace((unsigned char)cfg.token.back()))
            cfg.token.pop_back();
        cfg.report_period = form["period"];

        // Puste haslo = bez zmian (ale tylko gdy SSID sie nie zmienilo).
        if (!form["wpass"].empty() || cfg.wifi_ssid != old_ssid)
            cfg.wifi_pass = form["wpass"];
    }

    if (cfg.wifi_ssid.size() > MAX_SSID || cfg.wifi_pass.size() > MAX_WIFI_PASS ||
        cfg.mqtt_broker.size() > MAX_FIELD || cfg.mqtt_login.size() > MAX_FIELD ||
        cfg.mqtt_pass.size() > MAX_FIELD || cfg.token.size() > MAX_TOKEN)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Zbyt dlugie wartosci");
        return ESP_FAIL;
    }

    if (!cfg.token.empty() && !Configuration::valid_token(cfg.token))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "Nieprawidlowy token (6-64 znaki: litery, cyfry, '_' lub '-')");
        return ESP_FAIL;
    }
    cfg.main_topic = Configuration::build_topic(cfg.token);

    uint32_t period_check = 0;
    if (!cfg.report_period.empty() && !Configuration::parse_report_period(cfg.report_period, period_check))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nieprawidlowa strategia wysylania danych");
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

    httpd_uri_t jsqr_uri = {};
    jsqr_uri.uri = "/jsqr.js";
    jsqr_uri.method = HTTP_GET;
    jsqr_uri.handler = jsqr_get_handler;
    httpd_register_uri_handler(server, &jsqr_uri);

    httpd_uri_t wifi_scan_uri = {};
    wifi_scan_uri.uri = "/wifi_scan";
    wifi_scan_uri.method = HTTP_GET;
    wifi_scan_uri.handler = wifi_scan_get_handler;
    httpd_register_uri_handler(server, &wifi_scan_uri);

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
