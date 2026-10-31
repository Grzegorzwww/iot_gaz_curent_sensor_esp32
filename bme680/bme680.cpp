#include "bme680/bme680.hpp"

#ifndef BME680_FORCE_CHIP
#define BME680_FORCE_CHIP 0
#endif

#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BMx680";

// Rejestry (datasheet BME680, rozdz. 5.3).
static constexpr uint8_t REG_MEAS_STATUS_0 = 0x1D; // pole danych 0: status, cisnienie, temperatura, wilgotnosc
static constexpr uint8_t REG_CTRL_GAS_0 = 0x70;    // bit 3 = heat_off
static constexpr uint8_t REG_CTRL_GAS_1 = 0x71;    // bit 4 = run_gas
static constexpr uint8_t REG_CTRL_HUM = 0x72;
static constexpr uint8_t REG_CTRL_MEAS = 0x74;
static constexpr uint8_t REG_CONFIG = 0x75;
static constexpr uint8_t REG_CHIP_ID = 0xD0;
static constexpr uint8_t REG_RESET = 0xE0;
static constexpr uint8_t REG_COEFF1 = 0x8A; // 23 bajtow kalibracji (T2..P10)
static constexpr uint8_t REG_COEFF2 = 0xE1; // 14 bajtow kalibracji (H1..H7, T1)

// Rejestry rodziny BMx280 (datasheet BME280 / BMP280); ID i reset sa wspolne z BME680.
static constexpr uint8_t REG280_CALIB_A = 0x88;  // 24 bajty: T1..T3, P1..P9
static constexpr uint8_t REG280_CALIB_H1 = 0xA1; // 1 bajt (tylko BME280)
static constexpr uint8_t REG280_CALIB_H2 = 0xE1; // 7 bajtow: H2..H6 (tylko BME280)
static constexpr uint8_t REG280_CTRL_HUM = 0xF2; // tylko BME280
static constexpr uint8_t REG280_CTRL_MEAS = 0xF4;
static constexpr uint8_t REG280_CONFIG = 0xF5;
static constexpr uint8_t REG280_DATA = 0xF7; // cisnienie (3), temperatura (3), wilgotnosc (2, tylko BME280)

static constexpr uint8_t ID_BMP280_SAMPLE_1 = 0x56; // wczesne egzemplarze BMP280
static constexpr uint8_t ID_BMP280_SAMPLE_2 = 0x57;
static constexpr uint8_t ID_BMP280 = 0x58;
static constexpr uint8_t ID_BME280 = 0x60;
static constexpr uint8_t ID_BME680 = 0x61; // takze BME688
static constexpr uint8_t SOFT_RESET_CMD = 0xB6;
static constexpr uint8_t MODE_FORCED = 0x01;

// Nadprobkowanie: 1 = x1, 2 = x2, 3 = x4. Wiecej probek = mniejszy szum, dluzszy pomiar (kilkadziesiat ms).
static constexpr uint8_t OS_TEMP = 2;
static constexpr uint8_t OS_PRES = 3;
static constexpr uint8_t OS_HUM = 2;

static constexpr int I2C_SPEED_HZ = 100000;
static constexpr int I2C_TIMEOUT_MS = 100;
static constexpr int PROBE_TIMEOUT_MS = 50;
static constexpr int MEASURE_TIMEOUT_MS = 300;

bool Bme680::read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return dev_ && i2c_master_transmit_receive(dev_, &reg, 1, buf, len, I2C_TIMEOUT_MS) == ESP_OK;
}

bool Bme680::write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = {reg, value};
    return dev_ && i2c_master_transmit(dev_, tx, sizeof(tx), I2C_TIMEOUT_MS) == ESP_OK;
}

bool Bme680::begin()
{
    if (dev_)
        return true;

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = SDA_GPIO;
    bus_cfg.scl_io_num = SCL_GPIO;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &bus_) != ESP_OK)
    {
        ESP_LOGE(TAG, "Nie mozna uruchomic magistrali I2C");
        bus_ = nullptr;
        return false;
    }

    uint8_t addr = 0;
    const uint8_t candidates[] = {0x76, 0x77};
    for (uint8_t candidate : candidates)
    {
        if (i2c_master_probe(bus_, candidate, PROBE_TIMEOUT_MS) == ESP_OK)
        {
            addr = candidate;
            break;
        }
    }
    if (addr == 0)
    {
        ESP_LOGE(TAG, "Brak odpowiedzi pod adresem 0x76/0x77 (SDA=GPIO%d, SCL=GPIO%d) - sprawdz zasilanie 3V3, "
                      "przewody SDA/SCL i podciagniecia",
                 (int)SDA_GPIO, (int)SCL_GPIO);
        end();
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = I2C_SPEED_HZ;
    if (i2c_master_bus_add_device(bus_, &dev_cfg, &dev_) != ESP_OK)
    {
        dev_ = nullptr;
        end();
        return false;
    }

    uint8_t id = 0;
    if (!read_regs(REG_CHIP_ID, &id, 1))
    {
        ESP_LOGE(TAG, "Odczyt ID czujnika (0x%02X) nieudany", addr);
        end();
        return false;
    }
#if BME680_FORCE_CHIP == 1
    chip_ = Chip::Bme680;
#elif BME680_FORCE_CHIP == 2
    chip_ = Chip::Bme280;
#elif BME680_FORCE_CHIP == 3
    chip_ = Chip::Bmp280;
#else
    switch (id)
    {
    case ID_BME680:
        chip_ = Chip::Bme680;
        break;
    case ID_BME280:
        chip_ = Chip::Bme280;
        break;
    case ID_BMP280:
    case ID_BMP280_SAMPLE_1:
    case ID_BMP280_SAMPLE_2:
        chip_ = Chip::Bmp280;
        break;
    default:
        ESP_LOGE(TAG, "Pod adresem 0x%02X jest nieobslugiwany uklad o ID 0x%02X (obslugiwane: 0x61 = BME680, "
                      "0x60 = BME280, 0x58 = BMP280)",
                 addr, id);
        end();
        return false;
    }
#endif

    write_reg(REG_RESET, SOFT_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!(chip_ == Chip::Bme680 ? load_calibration() : load_calibration_bmx280()))
    {
        end();
        return false;
    }

    ESP_LOGI(TAG, "%s pod adresem 0x%02X", chip_name(), addr);
    return true;
}

const char *Bme680::chip_name() const
{
    switch (chip_)
    {
    case Chip::Bme680:
        return "BME680";
    case Chip::Bme280:
        return "BME280";
    case Chip::Bmp280:
        return "BMP280";
    default:
        return "brak";
    }
}

void Bme680::end()
{
    if (dev_)
        i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
    chip_ = Chip::None;
    if (bus_)
        i2c_del_master_bus(bus_);
    bus_ = nullptr;
}

bool Bme680::load_calibration()
{
    uint8_t c1[23];
    uint8_t c2[14];
    if (!read_regs(REG_COEFF1, c1, sizeof(c1)) || !read_regs(REG_COEFF2, c2, sizeof(c2)))
    {
        ESP_LOGE(TAG, "Odczyt kalibracji nieudany");
        return false;
    }

    auto u16 = [](uint8_t msb, uint8_t lsb) { return (uint16_t)(((uint16_t)msb << 8) | lsb); };
    auto s16 = [&](uint8_t msb, uint8_t lsb) { return (int16_t)u16(msb, lsb); };

    // Indeks w c1 = adres - 0x8A, w c2 = adres - 0xE1.
    cal_.t1 = u16(c2[9], c2[8]);
    cal_.t2 = s16(c1[1], c1[0]);
    cal_.t3 = (int8_t)c1[2];

    cal_.p1 = u16(c1[5], c1[4]);
    cal_.p2 = s16(c1[7], c1[6]);
    cal_.p3 = (int8_t)c1[8];
    cal_.p4 = s16(c1[11], c1[10]);
    cal_.p5 = s16(c1[13], c1[12]);
    cal_.p6 = (int8_t)c1[15];
    cal_.p7 = (int8_t)c1[14];
    cal_.p8 = s16(c1[19], c1[18]);
    cal_.p9 = s16(c1[21], c1[20]);
    cal_.p10 = c1[22];

    cal_.h1 = (uint16_t)(((uint16_t)c2[2] << 4) | (c2[1] & 0x0F));
    cal_.h2 = (uint16_t)(((uint16_t)c2[0] << 4) | (c2[1] >> 4));
    cal_.h3 = (int8_t)c2[3];
    cal_.h4 = (int8_t)c2[4];
    cal_.h5 = (int8_t)c2[5];
    cal_.h6 = c2[6];
    cal_.h7 = (int8_t)c2[7];

    if (cal_.t1 == 0 || cal_.t1 == 0xFFFF || cal_.p1 == 0 || cal_.p1 == 0xFFFF)
    {
        ESP_LOGE(TAG, "Nieprawidlowa kalibracja (T1=%u, P1=%u) - uszkodzony czujnik albo zle polaczenie",
                 (unsigned)cal_.t1, (unsigned)cal_.p1);
        return false;
    }
    return true;
}

bool Bme680::read(Bme680Reading &out)
{
    if (!dev_)
        return false;
    return chip_ == Chip::Bme680 ? read_bme680(out) : read_bmx280(out);
}

bool Bme680::read_bme680(Bme680Reading &out)
{
    // Grzalka i pomiar gazu wylaczone, bez filtra IIR (pojedynczy pomiar), potem start pomiaru w trybie forced.
    // CTRL_MEAS musi byc zapisany po CTRL_HUM, zeby ustawienie wilgotnosci zadzialalo.
    if (!write_reg(REG_CTRL_GAS_0, 0x08) || !write_reg(REG_CTRL_GAS_1, 0x00) || !write_reg(REG_CONFIG, 0x00) ||
        !write_reg(REG_CTRL_HUM, OS_HUM) ||
        !write_reg(REG_CTRL_MEAS, (uint8_t)((OS_TEMP << 5) | (OS_PRES << 2) | MODE_FORCED)))
    {
        ESP_LOGE(TAG, "Zapis konfiguracji pomiaru nieudany");
        return false;
    }

    // Po zakonczeniu pomiaru czujnik sam wraca do trybu sleep (bity trybu = 0).
    bool done = false;
    for (int waited = 0; waited < MEASURE_TIMEOUT_MS; waited += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t ctrl = 0;
        if (read_regs(REG_CTRL_MEAS, &ctrl, 1) && (ctrl & 0x03) == 0)
        {
            done = true;
            break;
        }
    }
    if (!done)
    {
        ESP_LOGE(TAG, "Pomiar nie zakonczyl sie w %d ms", MEASURE_TIMEOUT_MS);
        return false;
    }

    // d[0] = status (bit 7 = nowe dane), d[2..4] = cisnienie, d[5..7] = temperatura, d[8..9] = wilgotnosc.
    uint8_t d[10];
    if (!read_regs(REG_MEAS_STATUS_0, d, sizeof(d)) || !(d[0] & 0x80))
    {
        ESP_LOGE(TAG, "Brak nowych danych pomiarowych");
        return false;
    }

    const uint32_t adc_p = ((uint32_t)d[2] << 12) | ((uint32_t)d[3] << 4) | (d[4] >> 4);
    const uint32_t adc_t = ((uint32_t)d[5] << 12) | ((uint32_t)d[6] << 4) | (d[7] >> 4);
    const uint32_t adc_h = ((uint32_t)d[8] << 8) | d[9];
    if (adc_t == 0x80000 || adc_p == 0x80000 || adc_h == 0x8000) // wartosc "pomiar pominiety"
    {
        ESP_LOGE(TAG, "Czujnik zwrocil pominiety pomiar");
        return false;
    }

    // Kompensacja wg wzorow zmiennoprzecinkowych Bosch (BME68x Sensor API), liczona w double.
    const double t = (double)adc_t;
    double v1 = (t / 16384.0 - cal_.t1 / 1024.0) * cal_.t2;
    const double dt = t / 131072.0 - cal_.t1 / 8192.0;
    double v2 = dt * dt * (cal_.t3 * 16.0);
    const double t_fine = v1 + v2;
    const double temp_c = t_fine / 5120.0;

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * (cal_.p6 / 131072.0);
    v2 = v2 + v1 * cal_.p5 * 2.0;
    v2 = v2 / 4.0 + cal_.p4 * 65536.0;
    v1 = ((cal_.p3 * v1 * v1) / 16384.0 + cal_.p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * cal_.p1;
    if (v1 == 0.0)
        return false;
    double p = 1048576.0 - (double)adc_p;
    p = ((p - v2 / 4096.0) * 6250.0) / v1;
    v1 = (cal_.p9 * p * p) / 2147483648.0;
    v2 = p * (cal_.p8 / 32768.0);
    const double v3 = (p / 256.0) * (p / 256.0) * (p / 256.0) * (cal_.p10 / 131072.0);
    p = p + (v1 + v2 + v3 + cal_.p7 * 128.0) / 16.0; // [Pa]

    const double h_v1 = (double)adc_h - (cal_.h1 * 16.0 + (cal_.h3 / 2.0) * temp_c);
    const double h_v2 = h_v1 * ((cal_.h2 / 262144.0) *
                                (1.0 + (cal_.h4 / 16384.0) * temp_c + (cal_.h5 / 1048576.0) * temp_c * temp_c));
    const double h_v3 = cal_.h6 / 16384.0;
    const double h_v4 = cal_.h7 / 2097152.0;
    double hum = h_v2 + (h_v3 + h_v4 * temp_c) * h_v2 * h_v2;
    hum = hum < 0.0 ? 0.0 : (hum > 100.0 ? 100.0 : hum);

    const double hpa = p / 100.0;
    if (!std::isfinite(temp_c) || !std::isfinite(hum) || !std::isfinite(hpa) || temp_c < -40.0 || temp_c > 85.0 ||
        hpa < 300.0 || hpa > 1100.0)
    {
        ESP_LOGE(TAG, "Odczyt poza zakresem: %.1f C, %.1f %%, %.1f hPa", temp_c, hum, hpa);
        return false;
    }

    out.temperature_c = (float)temp_c;
    out.humidity_pct = (float)hum;
    out.pressure_hpa = (float)hpa;
    out.has_humidity = true;
    return true;
}

bool Bme680::load_calibration_bmx280()
{
    uint8_t a[24];
    if (!read_regs(REG280_CALIB_A, a, sizeof(a)))
    {
        ESP_LOGE(TAG, "Odczyt kalibracji nieudany");
        return false;
    }

    auto u16 = [](uint8_t lsb, uint8_t msb) { return (uint16_t)(((uint16_t)msb << 8) | lsb); };
    cal2_.t1 = u16(a[0], a[1]);
    cal2_.t2 = (int16_t)u16(a[2], a[3]);
    cal2_.t3 = (int16_t)u16(a[4], a[5]);
    cal2_.p1 = u16(a[6], a[7]);
    cal2_.p2 = (int16_t)u16(a[8], a[9]);
    cal2_.p3 = (int16_t)u16(a[10], a[11]);
    cal2_.p4 = (int16_t)u16(a[12], a[13]);
    cal2_.p5 = (int16_t)u16(a[14], a[15]);
    cal2_.p6 = (int16_t)u16(a[16], a[17]);
    cal2_.p7 = (int16_t)u16(a[18], a[19]);
    cal2_.p8 = (int16_t)u16(a[20], a[21]);
    cal2_.p9 = (int16_t)u16(a[22], a[23]);

    if (chip_ == Chip::Bme280)
    {
        uint8_t h1 = 0;
        uint8_t h[7];
        if (!read_regs(REG280_CALIB_H1, &h1, 1) || !read_regs(REG280_CALIB_H2, h, sizeof(h)))
        {
            ESP_LOGE(TAG, "Odczyt kalibracji wilgotnosci nieudany");
            return false;
        }
        cal2_.h1 = h1;
        cal2_.h2 = (int16_t)u16(h[0], h[1]);
        cal2_.h3 = h[2];
        cal2_.h4 = (int16_t)(((int16_t)(int8_t)h[3] << 4) | (h[4] & 0x0F));
        cal2_.h5 = (int16_t)(((int16_t)(int8_t)h[5] << 4) | (h[4] >> 4));
        cal2_.h6 = (int8_t)h[6];
    }

    if (cal2_.t1 == 0 || cal2_.t1 == 0xFFFF || cal2_.p1 == 0 || cal2_.p1 == 0xFFFF)
    {
        ESP_LOGE(TAG, "Nieprawidlowa kalibracja (T1=%u, P1=%u) - uszkodzony czujnik albo zle polaczenie",
                 (unsigned)cal2_.t1, (unsigned)cal2_.p1);
        return false;
    }
    return true;
}

bool Bme680::read_bmx280(Bme680Reading &out)
{
    const bool has_hum = chip_ == Chip::Bme280;

    // Bez filtra IIR; ctrl_hum (tylko BME280) musi byc zapisany przed ctrl_meas, zeby zadzialal.
    if (!write_reg(REG280_CONFIG, 0x00) || (has_hum && !write_reg(REG280_CTRL_HUM, OS_HUM)) ||
        !write_reg(REG280_CTRL_MEAS, (uint8_t)((OS_TEMP << 5) | (OS_PRES << 2) | MODE_FORCED)))
    {
        ESP_LOGE(TAG, "Zapis konfiguracji pomiaru nieudany");
        return false;
    }

    // Po zakonczeniu pomiaru czujnik sam wraca do trybu sleep (bity trybu = 0).
    bool done = false;
    for (int waited = 0; waited < MEASURE_TIMEOUT_MS; waited += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t ctrl = 0;
        if (read_regs(REG280_CTRL_MEAS, &ctrl, 1) && (ctrl & 0x03) == 0)
        {
            done = true;
            break;
        }
    }
    if (!done)
    {
        ESP_LOGE(TAG, "Pomiar nie zakonczyl sie w %d ms", MEASURE_TIMEOUT_MS);
        return false;
    }

    // d[0..2] = cisnienie, d[3..5] = temperatura, d[6..7] = wilgotnosc (tylko BME280).
    uint8_t d[8] = {};
    if (!read_regs(REG280_DATA, d, has_hum ? 8 : 6))
    {
        ESP_LOGE(TAG, "Odczyt danych pomiarowych nieudany");
        return false;
    }

    const uint32_t adc_p = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4);
    const uint32_t adc_t = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4);
    const uint32_t adc_h = ((uint32_t)d[6] << 8) | d[7];
    if (adc_t == 0x80000 || adc_p == 0x80000 || (has_hum && adc_h == 0x8000)) // wartosc "pomiar pominiety"
    {
        ESP_LOGE(TAG, "Czujnik zwrocil pominiety pomiar");
        return false;
    }

    // Kompensacja wg wzorow zmiennoprzecinkowych z datasheetu BME280 / BMP280.
    const double t = (double)adc_t;
    double v1 = (t / 16384.0 - cal2_.t1 / 1024.0) * cal2_.t2;
    const double dt = t / 131072.0 - cal2_.t1 / 8192.0;
    double v2 = dt * dt * cal2_.t3;
    const double t_fine = v1 + v2;
    const double temp_c = t_fine / 5120.0;

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * cal2_.p6 / 32768.0;
    v2 = v2 + v1 * cal2_.p5 * 2.0;
    v2 = v2 / 4.0 + cal2_.p4 * 65536.0;
    v1 = (cal2_.p3 * v1 * v1 / 524288.0 + cal2_.p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * cal2_.p1;
    if (v1 == 0.0)
        return false;
    double p = 1048576.0 - (double)adc_p;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = cal2_.p9 * p * p / 2147483648.0;
    v2 = p * cal2_.p8 / 32768.0;
    p = p + (v1 + v2 + cal2_.p7) / 16.0; // [Pa]

    double hum = 0.0;
    if (has_hum)
    {
        double h = t_fine - 76800.0;
        h = ((double)adc_h - (cal2_.h4 * 64.0 + cal2_.h5 / 16384.0 * h)) *
            (cal2_.h2 / 65536.0 * (1.0 + cal2_.h6 / 67108864.0 * h * (1.0 + cal2_.h3 / 67108864.0 * h)));
        h = h * (1.0 - cal2_.h1 * h / 524288.0);
        hum = h < 0.0 ? 0.0 : (h > 100.0 ? 100.0 : h);
    }

    const double hpa = p / 100.0;
    if (!std::isfinite(temp_c) || !std::isfinite(hum) || !std::isfinite(hpa) || temp_c < -40.0 || temp_c > 85.0 ||
        hpa < 300.0 || hpa > 1100.0)
    {
        ESP_LOGE(TAG, "Odczyt poza zakresem: %.1f C, %.1f %%, %.1f hPa", temp_c, hum, hpa);
        return false;
    }

    out.temperature_c = (float)temp_c;
    out.humidity_pct = (float)hum;
    out.pressure_hpa = (float)hpa;
    out.has_humidity = has_hum;
    return true;
}
