#ifndef CONSUMPTION_HISTORY_H
#define CONSUMPTION_HISTORY_H

#include <cstdint>

/*
 * Historia impulsow z ostatnich 24 godzin - do liczenia zuzycia w oknach 1 h / 3 h / 24 h.
 *
 * Impulsy sa zliczane w 5-minutowych kubelkach (288 kubelkow = 24 h) w pamieci RTC, ktora
 * przetrwa restart programowy. Czas pochodzi z gettimeofday (bez NTP liczony od startu), wiec po
 * restarcie zegar zaczyna od zera i historia jest czyszczona.
 *
 * Ograniczenia:
 *  - rozdzielczosc okna to 5 min: "ostatnia godzina" to 12 ostatnich kubelkow (od 55 do 60 min),
 *  - po utracie zasilania historia startuje od zera (RTC jest czyszczone) - o tym, ile historii jest
 *    dostepne, mowi covered_s(); wartosci okien sa wtedy zanizone do czasu, az historia sie "zapelni".
 */
class ConsumptionHistory
{
public:
    static constexpr int SLOT_S = 300;
    static constexpr int SLOTS = 288; // 24 h

    // Zapisuje impulsy w biezacym kubelku.
    static void record(uint32_t pulses = 1);

    // Suma impulsow z ostatnich `hours` godzin (1..24).
    static uint32_t pulses_in_last(int hours);

    // Ile sekund historii jest dostepne (maksymalnie 24 h).
    static int64_t covered_s();
};

#endif
