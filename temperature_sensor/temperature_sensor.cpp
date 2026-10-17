#include "temperature_sensor/temperature_sensor.hpp"

#include <cmath>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "TEMP_SENSOR";

// Liczba probek do usrednienia jednego odczytu (ADC szumi, a LM35 i tak reaguje wolno).
static const int SAMPLES = 16;

static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t s_cali = nullptr;
static bool s_cali_ok = false;

bool TemperatureSensor::init()
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT;
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        s_adc = nullptr;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12; // pelny zakres (~0-3.1V); LM35 w praktyce daje <1.5V
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, ADC_CHANNEL, &chan_cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        return false;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT;
    cali_cfg.chan = ADC_CHANNEL;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK;
    if (!s_cali_ok)
        ESP_LOGW(TAG, "Kalibracja ADC niedostepna na tym ukladzie - pomiar bedzie mniej dokladny");

    return true;
}

float TemperatureSensor::read_celsius()
{
    if (!s_adc)
        return NAN;

    int64_t sum_mv = 0;
    int ok_samples = 0;
    for (int i = 0; i < SAMPLES; ++i)
    {
        int raw = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL, &raw) != ESP_OK)
            continue;

        int mv = 0;
        if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK)
        {
            sum_mv += mv;
            ++ok_samples;
        }
    }

    if (ok_samples == 0)
    {
        ESP_LOGW(TAG, "Odczyt ADC nieudany");
        return NAN;
    }

    const double avg_mv = (double)sum_mv / ok_samples;
    return (float)(avg_mv / 10.0); // LM35: 10 mV na stopien C
}
