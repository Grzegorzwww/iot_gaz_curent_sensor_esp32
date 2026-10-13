#include "energy_meter/energy_meter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "hal/gpio_ll.h"

#include "energy_meter/consumption_history.hpp"
#include "storage/storage.hpp"

static const char *TAG = "ENERGY";

// Filtr blyskow diody licznika.
static constexpr int64_t MIN_WIDTH_US = 10000;          // najdluzszy ciagly odcinek swiecenia krotszy = zaklocenie
static constexpr int64_t MAX_WIDTH_US = 2000000;       // dluzsze = swiatlo stale / zle ustawiony czujnik
static constexpr int64_t MIN_INTERVAL_US = 40000;      // 36 kW przy 1000 imp/kWh - szybciej nie ma sensu
static constexpr int64_t GAP_US = 5000;                // przerwy w swieceniu krotsze niz to = drgania na zboczu, scalane w jeden blysk
static constexpr int64_t POWER_TIMEOUT_US = 10LL * 60 * 1000000;

namespace
{
struct Edge
{
    int64_t t_us;
    uint8_t level;
};

QueueHandle_t s_queue = nullptr;
} // namespace

// Przerwanie w IRAM, zeby zapis do flasha (NVS) nie opoznial obslugi zbocza.
static void IRAM_ATTR edge_isr(void *)
{
    Edge e;
    e.t_us = esp_timer_get_time();
    e.level = (uint8_t)gpio_ll_get_level(&GPIO, EnergyMeter::PULSE_GPIO);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_queue, &e, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

std::string EnergyMeter::format_kwh(uint32_t pulses, uint32_t imp_per_kwh)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", (double)pulses / (double)imp_per_kwh);
    return buf;
}

bool EnergyMeter::parse_kwh(const std::string &text, uint32_t imp_per_kwh, uint32_t &pulses)
{
    std::string t = text;
    for (char &c : t)
        if (c == ',')
            c = '.';

    char *end = nullptr;
    const double kwh = strtod(t.c_str(), &end);
    while (end && (*end == ' ' || *end == '\r' || *end == '\n'))
        ++end;
    if (t.empty() || end == t.c_str() || *end != '\0' || !(kwh >= 0.0) || kwh * imp_per_kwh > 4.0e9)
        return false;

    pulses = (uint32_t)std::llround(kwh * imp_per_kwh);
    return true;
}

EnergyMeter &EnergyMeter::instance()
{
    static EnergyMeter meter;
    return meter;
}

bool EnergyMeter::start(uint32_t imp_per_kwh)
{
    if (task_)
        return true;

    imp_per_kwh_ = imp_per_kwh > 0 ? imp_per_kwh : 1000;

    uint32_t saved = 0;
    if (Storage::GetInstance()->read_pulses(saved) == ESP_OK)
        pulses_ = saved;

    s_queue = xQueueCreate(256, sizeof(Edge));
    if (!s_queue || xTaskCreate(task_entry, "energy_meter", 4096, this, 5, &task_) != pdPASS)
    {
        ESP_LOGE(TAG, "Cannot create task/queue");
        return false;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PULSE_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&io));

    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) // INVALID_STATE = juz zainstalowana
        ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(PULSE_GPIO, edge_isr, nullptr));

    ESP_LOGI(TAG, "Start: GPIO%d, %lu imp/kWh, counter %lu pulses (%s kWh)", (int)PULSE_GPIO,
             (unsigned long)imp_per_kwh_, (unsigned long)pulses_.load(),
             format_kwh(pulses_.load(), imp_per_kwh_).c_str());
    return true;
}

void EnergyMeter::set_pulses(uint32_t pulses)
{
    pulses_ = pulses;
    dirty_ = false;
    Storage::GetInstance()->save_pulses(pulses);
    ESP_LOGI(TAG, "Counter set to %lu pulses", (unsigned long)pulses);
}

void EnergyMeter::flush()
{
    if (dirty_.exchange(false))
        Storage::GetInstance()->save_pulses(pulses_.load());
}

double EnergyMeter::power_w() const
{
    int64_t last, interval;
    portENTER_CRITICAL(&lock_);
    last = last_pulse_us_;
    interval = last_interval_us_;
    portEXIT_CRITICAL(&lock_);

    if (last == 0 || interval == 0)
        return -1.0;

    const int64_t since = esp_timer_get_time() - last;
    if (since > POWER_TIMEOUT_US)
        return 0.0;

    // Gdy kolejny impuls sie spoznia, moc na pewno nie jest wieksza niz wynikajaca z biezacej ciszy.
    const int64_t ref_us = std::max(interval, since);
    return 3.6e12 / ((double)imp_per_kwh_ * (double)ref_us);
}

void EnergyMeter::count_pulse(int64_t on_us, int64_t width_us)
{
    const uint32_t total = pulses_.fetch_add(1) + 1;
    dirty_ = true;
    last_width_ms_ = (uint32_t)std::max<int64_t>(1, width_us / 1000);
    ConsumptionHistory::record(1);

    int64_t interval;
    portENTER_CRITICAL(&lock_);
    interval = last_pulse_us_ != 0 ? on_us - last_pulse_us_ : 0;
    last_interval_us_ = interval;
    last_pulse_us_ = on_us;
    portEXIT_CRITICAL(&lock_);

    if (interval > 0)
    {
        ESP_LOGD(TAG, "Pulse! total=%lu (%s kWh), width %lld ms, interval %.2f s, power %.0f W", (unsigned long)total,
                 format_kwh(total, imp_per_kwh_).c_str(), (long long)(width_us / 1000), interval / 1e6,
                 3.6e12 / ((double)imp_per_kwh_ * (double)interval));
    }
    else
    {
        ESP_LOGD(TAG, "Pulse! total=%lu (%s kWh), width %lld ms", (unsigned long)total,
                 format_kwh(total, imp_per_kwh_).c_str(), (long long)(width_us / 1000));
    }
}

void EnergyMeter::task_entry(void *arg)
{
    static_cast<EnergyMeter *>(arg)->task();
}

// Przy powolnych zboczach (slaby pull-up, czujnik w polcieniu) pin drga wokol progu: dziesiatki zboczy
// w ciagu kilku ms. Dlatego blysk to ciag swiecen rozdzielonych przerwami krotszymi niz GAP_US.
// Blysk jest rozstrzygany po GAP_US ciszy; liczy sie, jesli jego najdluzszy CIAGLY odcinek swiecenia
// ma MIN_WIDTH_US..MAX_WIDTH_US (same drgania maja co najwyzej kilkaset us).
void EnergyMeter::task()
{
    bool lit = false;               // dioda licznika aktualnie swieci (po filtracji zdublowanych zdarzen)
    bool cand = false;              // trwa zbieranie blysku
    int64_t cur_on_us = 0;          // poczatek biezacego odcinka swiecenia
    int64_t cand_last_off_us = 0;   // koniec ostatniego odcinka swiecenia
    int64_t best_run_us = 0;        // najdluzszy ciagly odcinek swiecenia w blysku
    int64_t best_run_start_us = 0;
    int64_t last_accepted_on_us = 0;

    auto finalize = [&]()
    {
        const bool too_short = best_run_us < MIN_WIDTH_US;
        const bool too_long = best_run_us > MAX_WIDTH_US;
        const bool too_soon = last_accepted_on_us != 0 && best_run_start_us - last_accepted_on_us < MIN_INTERVAL_US;
        const bool accepted = !too_short && !too_long && !too_soon;

        cand = false;
        best_run_us = 0;
        if (!accepted)
        {
            rejected_.fetch_add(1);
            return;
        }
        last_accepted_on_us = best_run_start_us;
        count_pulse(best_run_start_us, best_run_us);
    };

    auto handle_edge = [&](int64_t t_us, bool active)
    {
        if (active == lit)
            return;
        lit = active;

        if (active)
        {
            if (cand && t_us - cand_last_off_us >= GAP_US)
                finalize();
            if (!cand)
            {
                cand = true;
                best_run_us = 0;
            }
            cur_on_us = t_us;
        }
        else if (cand)
        {
            const int64_t run = t_us - cur_on_us;
            if (run > best_run_us)
            {
                best_run_us = run;
                best_run_start_us = cur_on_us;
            }
            cand_last_off_us = t_us;
        }
    };

    for (;;)
    {
        Edge e;
        const TickType_t wait = cand ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(5000);
        if (xQueueReceive(s_queue, &e, wait) == pdTRUE)
        {
            handle_edge(e.t_us, e.level == ACTIVE_LEVEL);
        }
        else
        {
            // Cisza na pinie: synchronizujemy stan z rzeczywistym poziomem (gdyby zdarzenie zginelo).
            const bool raw = gpio_get_level(PULSE_GPIO) == ACTIVE_LEVEL;
            handle_edge(esp_timer_get_time(), raw);
        }

        // Blysk jest gotowy, gdy swiatlo zgaslo i przez GAP_US nie bylo nowego zbocza.
        if (cand && !lit && esp_timer_get_time() - cand_last_off_us >= GAP_US)
            finalize();
    }
}
