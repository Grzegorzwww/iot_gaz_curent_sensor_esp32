#ifndef TEMPERATURE_SENSOR_H
#define TEMPERATURE_SENSOR_H

#include "hal/adc_types.h"
#include "configuration/Configuration.hpp" // definiuje ESP32_C3/ESP32_C6 (wybor plytki)

/*
 * Odczyt temperatury wody z czujnika LM35 (analogowy, liniowy: 10 mV / stopien C, 0 V = 0 st. C).
 *
 * Podlaczenie:
 *     LM35 Vs   -> 3V3
 *     LM35 GND  -> GND
 *     LM35 Vout -> ADC_CHANNEL ponizej (dzielnik napiecia niepotrzebny - w zakresie wody uzytkowej
 *                  napiecie wyjsciowe nie przekracza ok. 1 V, co miesci sie w zakresie ADC).
 *
 * Uwaga: pierwotny przyklad uzytkownika wskazywal ADC1_CHANNEL_6 / GPIO34 - to mapowanie pochodzi
 * z "klasycznego" ESP32 i nie istnieje na ESP32-C6 (ADC1 ma tam tylko kanaly 0-6 na GPIO0-6, a XIAO
 * ESP32-C6 wyprowadza na zewnatrz jedynie kanaly 0-2 jako piny A0/A1/A2 = GPIO0/1/2). Uzyto wiec
 * kanalu 0 (GPIO0, podpisany na plytce XIAO jako "A0"/"D0").
 */
class TemperatureSensor
{
public:
#ifdef ESP32_C6
    static constexpr adc_channel_t ADC_CHANNEL = ADC_CHANNEL_0; // XIAO ESP32-C6: GPIO0 (A0/D0)
#endif
    static constexpr adc_unit_t ADC_UNIT = ADC_UNIT_1;

    // Inicjalizuje ADC. Zwraca false przy bledzie (czujnik nie bedzie dostepny).
    static bool init();

    // Zwraca usredniony odczyt temperatury wody [st. C]. Przy bledzie odczytu zwraca NAN.
    static float read_celsius();
};

#endif
