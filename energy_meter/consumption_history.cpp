#include "energy_meter/consumption_history.hpp"

#include <sys/time.h>

#include <cstring>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

static constexpr uint32_t MAGIC = 0x454E5231; // "ENR1"

// Pamiec RTC - przetrwa deep sleep. Przy utracie zasilania magic != MAGIC => historia jest czyszczona.
RTC_DATA_ATTR static uint32_t s_magic = 0;
RTC_DATA_ATTR static int64_t s_head_slot = 0; // bezwzgledny numer najnowszego kubelka (czas / SLOT_S)
RTC_DATA_ATTR static int64_t s_start_s = 0;   // kiedy zaczela sie historia
RTC_DATA_ATTR static uint16_t s_slots[ConsumptionHistory::SLOTS];

// Wywolywane z zadania licznika i z glownego zadania.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_s()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec;
}

// Przesuwa "glowe" do biezacego kubelka (zerujac pominiete). Wolac pod s_lock.
static void advance_locked(int64_t t)
{
    const int64_t slot = t / ConsumptionHistory::SLOT_S;

    // Pierwsze uruchomienie albo zegar poszedl wstecz (reset zegara) => zaczynamy od nowa.
    if (s_magic != MAGIC || slot < s_head_slot)
    {
        memset(s_slots, 0, sizeof(s_slots));
        s_magic = MAGIC;
        s_head_slot = slot;
        s_start_s = t;
        return;
    }

    int64_t steps = slot - s_head_slot;
    if (steps > ConsumptionHistory::SLOTS)
        steps = ConsumptionHistory::SLOTS;
    for (int64_t i = 1; i <= steps; ++i)
        s_slots[(s_head_slot + i) % ConsumptionHistory::SLOTS] = 0;
    s_head_slot = slot;
}

void ConsumptionHistory::record(uint32_t pulses)
{
    const int64_t t = now_s();
    portENTER_CRITICAL(&s_lock);
    advance_locked(t);
    uint16_t &cell = s_slots[s_head_slot % SLOTS];
    const uint32_t sum = (uint32_t)cell + pulses;
    cell = sum > 0xFFFF ? 0xFFFF : (uint16_t)sum;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t ConsumptionHistory::pulses_in_last(int hours)
{
    if (hours < 1)
        hours = 1;
    if (hours > 24)
        hours = 24;
    const int n = hours * (3600 / SLOT_S);

    const int64_t t = now_s();
    uint32_t sum = 0;
    portENTER_CRITICAL(&s_lock);
    advance_locked(t);
    for (int k = 0; k < n; ++k)
        sum += s_slots[((s_head_slot - k) % SLOTS + SLOTS) % SLOTS];
    portEXIT_CRITICAL(&s_lock);
    return sum;
}

int64_t ConsumptionHistory::covered_s()
{
    const int64_t t = now_s();
    portENTER_CRITICAL(&s_lock);
    advance_locked(t);
    int64_t covered = t - s_start_s;
    portEXIT_CRITICAL(&s_lock);

    if (covered < 0)
        covered = 0;
    const int64_t max_s = (int64_t)SLOTS * SLOT_S;
    return covered > max_s ? max_s : covered;
}
