/*
 * Program rdzenia LP (ESP32-C6): zlicza blyski diody licznika energii, gdy glowny rdzen (HP) spi.
 *
 * Ta sama logika filtracji co EnergyMeter::task() (energy_meter.cpp):
 *  - blysk to ciag swiecen rozdzielonych przerwami krotszymi niz GAP (drgania na zboczu),
 *  - liczy sie, jesli najdluzszy ciagly odcinek swiecenia ma MIN_WIDTH..MAX_WIDTH,
 *  - od poprzedniego impulsu musi minac co najmniej MIN_INTERVAL.
 *
 * Czas: licznik LP timera (tyki zegara RTC SLOW, ok. 150 kHz) - dziala takze podczas deep sleep HP.
 * Zmienne globalne (ulp_<nazwa> po stronie HP) leza w pamieci RTC i przezywaja deep sleep.
 *
 * Pin: LP_IO_NUM_1 = GPIO1 (musi sie zgadzac z EnergyMeter::PULSE_GPIO dla ESP32-C6).
 */
#include <stdbool.h>
#include <stdint.h>

#include "riscv/csr.h"
#include "riscv/encoding.h"
#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_interrupts.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "ulp_lp_core_utils.h"

#define PULSE_PIN LP_IO_NUM_1
#define ACTIVE_LEVEL 0 // poziom na pinie, gdy dioda licznika swieci

#define MIN_WIDTH_US 10000
#define MAX_WIDTH_US 2000000
#define MIN_INTERVAL_US 40000
#define GAP_US 5000

#define RING_SIZE 32 // potega dwojki nie jest wymagana, ale tanszy modulo

// --- Eksportowane do HP (ulp_pulse_count itd.) ---
uint32_t pulse_count = 0;     // zaakceptowane blyski od startu programu LP
uint32_t rejected_count = 0;  // odrzucone blyski (za krotkie / za dlugie / zbyt szybko)
uint32_t last_width_ms = 0;   // dlugosc ostatniego zaakceptowanego blysku [ms]
uint32_t overflow_count = 0;  // zdarzenia zgubione przez przepelnienie kolejki zboczy

typedef struct
{
    uint64_t t;
    uint32_t level;
} edge_t;

static volatile edge_t s_ring[RING_SIZE];
static volatile uint32_t s_head = 0; // zapisuje ISR
static volatile uint32_t s_tail = 0; // zapisuje petla glowna

void LP_CORE_ISR_ATTR ulp_lp_core_lp_io_intr_handler(void)
{
    ulp_lp_core_gpio_clear_intr_status();
    const uint64_t t = ulp_lp_core_lp_timer_get_cycle_count();
    const uint32_t level = ulp_lp_core_gpio_get_level(PULSE_PIN) ? 1 : 0;

    const uint32_t head = s_head;
    if (head - s_tail >= RING_SIZE)
    {
        overflow_count++;
        return;
    }
    s_ring[head % RING_SIZE].t = t;
    s_ring[head % RING_SIZE].level = level;
    s_head = head + 1;
}

// Progi w tykach LP timera (wyliczone raz na starcie z kalibracji zegara RTC SLOW).
static uint64_t s_min_width, s_max_width, s_min_interval, s_gap;
static uint32_t s_ticks_per_ms;

static bool s_lit = false;            // dioda licznika aktualnie swieci
static bool s_cand = false;           // trwa zbieranie blysku
static uint64_t s_cur_on = 0;         // poczatek biezacego odcinka swiecenia
static uint64_t s_cand_last_off = 0;  // koniec ostatniego odcinka swiecenia
static uint64_t s_best_run = 0;       // najdluzszy ciagly odcinek swiecenia w blysku
static uint64_t s_best_run_start = 0;
static uint64_t s_last_accepted_on = 0;
static bool s_have_accepted = false;

static void finalize(void)
{
    const bool too_short = s_best_run < s_min_width;
    const bool too_long = s_best_run > s_max_width;
    const bool too_soon = s_have_accepted && (s_best_run_start - s_last_accepted_on) < s_min_interval;

    s_cand = false;
    const uint64_t run = s_best_run;
    s_best_run = 0;

    if (too_short || too_long || too_soon)
    {
        rejected_count++;
        return;
    }

    s_last_accepted_on = s_best_run_start;
    s_have_accepted = true;
    last_width_ms = (uint32_t)run / s_ticks_per_ms;
    pulse_count++;
}

static void handle_edge(uint64_t t, bool active)
{
    if (active == s_lit)
        return;
    s_lit = active;

    if (active)
    {
        if (s_cand && (t - s_cand_last_off) >= s_gap)
            finalize();
        if (!s_cand)
        {
            s_cand = true;
            s_best_run = 0;
        }
        s_cur_on = t;
    }
    else if (s_cand)
    {
        const uint64_t run = t - s_cur_on;
        if (run > s_best_run)
        {
            s_best_run = run;
            s_best_run_start = s_cur_on;
        }
        s_cand_last_off = t;
    }
}

int main(void)
{
    s_min_width = ulp_lp_core_lp_timer_calculate_sleep_ticks(MIN_WIDTH_US);
    s_max_width = ulp_lp_core_lp_timer_calculate_sleep_ticks(MAX_WIDTH_US);
    s_min_interval = ulp_lp_core_lp_timer_calculate_sleep_ticks(MIN_INTERVAL_US);
    s_gap = ulp_lp_core_lp_timer_calculate_sleep_ticks(GAP_US);
    s_ticks_per_ms = (uint32_t)ulp_lp_core_lp_timer_calculate_sleep_ticks(1000);
    if (s_ticks_per_ms == 0)
        s_ticks_per_ms = 1;

    s_lit = (ulp_lp_core_gpio_get_level(PULSE_PIN) ? 1 : 0) == ACTIVE_LEVEL;

    ulp_lp_core_intr_enable();
    ulp_lp_core_gpio_intr_enable(PULSE_PIN, LP_IO_INTR_ANYEDGE);

    for (;;)
    {
        // Przetwarz zbocza zebrane przez ISR.
        while (s_tail != s_head)
        {
            const uint32_t i = s_tail % RING_SIZE;
            const uint64_t t = s_ring[i].t;
            const bool active = s_ring[i].level == ACTIVE_LEVEL;
            s_tail = s_tail + 1;
            handle_edge(t, active);
        }

        // Swiatlo zgaslo: czekamy GAP bez zbocza, zeby uznac blysk za zakonczony (kilka ms aktywnego rdzenia).
        if (s_cand && !s_lit)
        {
            if (ulp_lp_core_lp_timer_get_cycle_count() - s_cand_last_off >= s_gap)
                finalize();
            else
                ulp_lp_core_delay_us(500);
            continue;
        }

        // Bezczynnosc: spimy do nastepnego zbocza. Przerwania wylaczamy na czas sprawdzenia kolejki -
        // WFI i tak sie obudzi, gdy przerwanie jest oczekujace, a zbocze nie zgubi sie miedzy sprawdzeniem a WFI.
        RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
        if (s_tail == s_head)
            ulp_lp_core_wait_for_intr();
        RV_SET_CSR(mstatus, MSTATUS_MIE);
    }

    return 0;
}
