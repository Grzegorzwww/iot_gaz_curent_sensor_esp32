#include "lm35/lm35.hpp"

#include "esp_log.h"

static const char *TAG = "LM35";

static constexpr adc_atten_t ATTEN = ADC_ATTEN_DB_0; // zakres ok. 0-0,95 V => do ok. 95 st. C przy 10 mV/st. C
static constexpr int SAMPLES = 64;
static constexpr float MV_PER_C = 10.0f;

bool Lm35::begin()
{
    if (adc_)
        return true;

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unit_cfg, &adc_) != ESP_OK)
    {
        ESP_LOGE(TAG, "Nie mozna uruchomic ADC1");
        adc_ = nullptr;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ATTEN;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(adc_, ADC_CHANNEL, &chan_cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Konfiguracja kanalu ADC nieudana");
        end();
        return false;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.chan = ADC_CHANNEL;
    cali_cfg.atten = ATTEN;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_) != ESP_OK)
    {
        ESP_LOGW(TAG, "Brak kalibracji ADC - wynik moze byc mniej dokladny");
        cali_ = nullptr;
    }
    return true;
}

void Lm35::end()
{
    if (cali_)
    {
        adc_cali_delete_scheme_curve_fitting(cali_);
        cali_ = nullptr;
    }
    if (adc_)
    {
        adc_oneshot_del_unit(adc_);
        adc_ = nullptr;
    }
}

bool Lm35::read(float &temperature_c)
{
    if (!adc_)
        return false;

    int64_t sum = 0;
    int count = 0;
    for (int i = 0; i < SAMPLES; ++i)
    {
        int raw = 0;
        if (adc_oneshot_read(adc_, ADC_CHANNEL, &raw) == ESP_OK)
        {
            sum += raw;
            ++count;
        }
    }
    if (count == 0)
        return false;

    const int raw_avg = (int)(sum / count);
    int mv = 0;
    if (cali_)
    {
        if (adc_cali_raw_to_voltage(cali_, raw_avg, &mv) != ESP_OK)
            return false;
    }
    else
    {
        mv = raw_avg * 950 / 4095; // przyblizenie dla 0 dB
    }

    temperature_c = mv / MV_PER_C;
    ESP_LOGI(TAG, "ADC: %d probek, raw=%d, %d mV (%s) => %.1f C", count, raw_avg, mv,
             cali_ ? "kalibracja" : "bez kalibracji", temperature_c);
    return true;
}
