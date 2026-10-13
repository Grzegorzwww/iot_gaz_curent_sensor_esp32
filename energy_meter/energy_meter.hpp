#ifndef ENERGY_METER_H
#define ENERGY_METER_H

#include <atomic>
#include <cstdint>
#include <string>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Licznik impulsow diody licznika energii (np. 1000 imp/kWh), odczytywanych fototranzystorem.
 *
 * Podlaczenie (NPN, np. fototranzystor 3 mm sprzezony optycznie z dioda licznika):
 *     3V3 --[10k]--+-- GPIO3 (D1)         (mozna tez zostawic sam wewnetrzny pull-up ~45k)
 *                  |
 *             kolektor
 *             emiter
 *                  |
 *                 GND
 * Gdy dioda swieci, fototranzystor przewodzi i pin spada do poziomu niskiego (ACTIVE_LEVEL = 0).
 * Fototranzystor trzeba oslonic od swiatla zewnetrznego (np. rurka termokurczliwa + tasma izolacyjna).
 *
 * Zliczanie:
 *  - przerwanie na obu zboczach (IRAM) wrzuca do kolejki poziom i znacznik czasu,
 *  - zadanie mierzy dlugosc blysku (zbocze swiecenia -> zbocze zgasniecia),
 *  - impuls jest liczony po zgasnieciu, jesli blysk trwal MIN_WIDTH..MAX_WIDTH (odrzuca zaklocenia
 *    i swiatlo stale) i od poprzedniego impulsu minelo co najmniej MIN_INTERVAL,
 *  - chwilowa moc wynika z odstepu miedzy impulsami: P[W] = 3.6e6 / (imp_per_kwh * odstep_s).
 *
 * Urzadzenie jest zasilane z sieci, wiec licznik jest zapisywany do NVS co jakis czas (flush()),
 * a nie przy kazdym impulsie (przy kilku impulsach na sekunde szybko zuzylby flash).
 */
class EnergyMeter
{
public:
    // XIAO ESP32-C3: D1 = GPIO3
    static constexpr gpio_num_t PULSE_GPIO = GPIO_NUM_3;
    static constexpr int ACTIVE_LEVEL = 0; // poziom na pinie, gdy dioda licznika swieci

    // TEST: loguje kazde zbocze i co 5 s surowy poziom na pinie (do strojenia fototranzystora).
    static constexpr bool LOG_EDGES = true;

    static EnergyMeter &instance();

    // 1234567 impulsow przy 1000 imp/kWh -> "1234.567"
    static std::string format_kwh(uint32_t pulses, uint32_t imp_per_kwh);

    // "1234.567" lub "1234,567" -> impulsy. Zwraca false dla nieprawidlowej wartosci.
    static bool parse_kwh(const std::string &text, uint32_t imp_per_kwh, uint32_t &pulses);

    // Wczytuje licznik z NVS i uruchamia zliczanie.
    bool start(uint32_t imp_per_kwh);

    uint32_t pulses() const { return pulses_.load(); }
    uint32_t imp_per_kwh() const { return imp_per_kwh_; }

    // Ustawia stan licznika (impulsy) i od razu zapisuje go w NVS.
    void set_pulses(uint32_t pulses);

    // Zapisuje licznik w NVS, jesli od ostatniego zapisu doszly impulsy.
    void flush();

    // Chwilowa moc [W]; wartosc ujemna = jeszcze nieznana (potrzebne dwa impulsy po starcie).
    // Po 10 min bez impulsu zwraca 0.
    double power_w() const;

    // Dlugosc ostatniego zaakceptowanego blysku [ms] (0 = jeszcze nie bylo).
    uint32_t last_pulse_width_ms() const { return last_width_ms_.load(); }

    // Liczba odrzuconych blyskow (za krotkie / za dlugie / zbyt szybko po poprzednim).
    uint32_t rejected() const { return rejected_.load(); }

private:
    EnergyMeter() = default;

    static void task_entry(void *arg);
    void task();
    void count_pulse(int64_t on_us, int64_t width_us);

    uint32_t imp_per_kwh_ = 1000;
    std::atomic<uint32_t> pulses_{0};
    std::atomic<bool> dirty_{false};
    std::atomic<uint32_t> last_width_ms_{0};
    std::atomic<uint32_t> rejected_{0};

    // Czasy (esp_timer, us) - 64 bity, wiec chronione sekcja krytyczna zamiast std::atomic.
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    int64_t last_pulse_us_ = 0;     // 0 = nie bylo impulsu
    int64_t last_interval_us_ = 0;  // 0 = nieznany

    TaskHandle_t task_ = nullptr;
};

#endif
