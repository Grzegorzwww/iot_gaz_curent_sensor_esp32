#ifndef LP_COUNTER_H
#define LP_COUNTER_H

#include <cstdint>

/*
 * Zliczanie impulsow diody licznika przez rdzen LP (tryb bateryjny, tylko ESP32-C6).
 *
 * Program rdzenia LP (energy_meter/lp_core/main.c) dziala takze wtedy, gdy glowny rdzen (HP) spi w deep sleep.
 * HP po obudzeniu tylko odczytuje liczniki z pamieci RTC. Pin impulsow (EnergyMeter::PULSE_GPIO = GPIO1)
 * jest pinem LP, wiec rdzen LP ma do niego dostep.
 *
 * Licznik rdzenia LP liczy od startu programu LP (nie przechowuje stanu licznika energii) - stan calkowity
 * trzyma main.cpp (RTC + NVS).
 */
class LpCounter
{
public:
    // Konfiguruje pin jako RTC IO. Gdy to nie jest wybudzenie z deep sleep (zasilanie, reset, restart),
    // laduje i uruchamia program LP. Zwraca true, gdy program LP wystartowal od nowa (liczniki LP = 0).
    static bool start();

    // Zatrzymuje program LP i oddaje pin zwyklemu GPIO (tryb sieciowy, gdy po restarcie LP jeszcze liczy).
    static void stop();

    // Zaakceptowane blyski od startu programu LP (zawija sie przy 2^32 - roznice licz arytmetyka bez znaku).
    static uint32_t count();
    static uint32_t rejected();
    static uint32_t last_width_ms();
};

#endif
