#ifndef LM35_H
#define LM35_H

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"

/*
 * Czujnik temperatury LM35 (10 mV / st. C) na wejsciu analogowym.
 *
 * Podlaczenie do XIAO ESP32-S3:
 *     LM35 +Vs -> zasilanie (patrz uwaga nizej)
 *     LM35 GND -> GND
 *     LM35 Vout -> D1 (GPIO2, ADC1_CH1)
 *
 * Uwaga: LM35 wedlug noty wymaga zasilania min. 4 V; z 3V3 zwykle dziala dla temperatur dodatnich, ale zaleca
 * sie zasilanie z 5 V (pin 5V/VBUS XIAO). Wyjscie przy 5 V zasilania i tak nie przekracza ~1 V dla temp. do 100 C.
 * Przy dlugim kablu dodaj ~100 nF miedzy Vout a GND przy wejsciu ADC.
 */
class Lm35
{
public:
    static constexpr adc_channel_t ADC_CHANNEL = ADC_CHANNEL_1; // XIAO ESP32-S3: D1 = GPIO2

    Lm35() = default;
    ~Lm35() { end(); }
    Lm35(const Lm35 &) = delete;
    Lm35 &operator=(const Lm35 &) = delete;

    bool begin();
    void end();
    bool ready() const { return adc_ != nullptr; }

    // Srednia z wielu probek [st. C].
    bool read(float &temperature_c);

private:
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t cali_ = nullptr;
};

#endif
