#include "energy_meter/lp_counter.hpp"

#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "energy_meter/energy_meter.hpp"
#include "ulp_lp_core.h"
#include "ulp_main.h" // wygenerowany z energy_meter/lp_core/main.c: ulp_pulse_count itd.

static const char *TAG = "LP_COUNTER";

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

// Pin w lp_core/main.c (LP_IO_NUM_1) musi byc tym samym pinem, z ktorego czyta EnergyMeter.
static_assert(EnergyMeter::PULSE_GPIO == GPIO_NUM_1, "lp_core/main.c zlicza impulsy z GPIO1 (LP_IO_NUM_1)");

static constexpr uint32_t MAGIC = 0x4C504331; // "LPC1"

// Pamiec RTC przezywa deep sleep; po zasilaniu/resecie jest bez znaczenia, bo wtedy przeladowujemy program LP.
RTC_DATA_ATTR static uint32_t s_lp_started = 0;

static uint32_t read_u32(const uint32_t &var)
{
    return *reinterpret_cast<const volatile uint32_t *>(&var);
}

bool LpCounter::start()
{
    const gpio_num_t pin = EnergyMeter::PULSE_GPIO;
    ESP_ERROR_CHECK(rtc_gpio_init(pin));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(pin));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(pin));

    // Wybudzenie z deep sleep: program LP caly czas dziala i ma aktualne liczniki - nie ruszamy go.
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && s_lp_started == MAGIC)
        return false;

    ulp_lp_core_stop(); // gdyby jeszcze dzialal po resecie HP
    ESP_ERROR_CHECK(ulp_lp_core_load_binary(ulp_main_bin_start, ulp_main_bin_end - ulp_main_bin_start));

    // Zmienne LP zerujemy sami (nie zakladamy, ze .bss jest czyszczone przez kod startowy LP).
    ulp_pulse_count = 0;
    ulp_rejected_count = 0;
    ulp_last_width_ms = 0;
    ulp_overflow_count = 0;

    ulp_lp_core_cfg_t cfg = {};
    cfg.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU; // start od razu
    ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));

    s_lp_started = MAGIC;
    ESP_LOGI(TAG, "Program LP uruchomiony (GPIO%d)", (int)pin);
    return true;
}

void LpCounter::stop()
{
    ulp_lp_core_stop();
    rtc_gpio_deinit(EnergyMeter::PULSE_GPIO);
}

uint32_t LpCounter::count()
{
    return read_u32(ulp_pulse_count);
}

uint32_t LpCounter::rejected()
{
    return read_u32(ulp_rejected_count);
}

uint32_t LpCounter::last_width_ms()
{
    return read_u32(ulp_last_width_ms);
}
