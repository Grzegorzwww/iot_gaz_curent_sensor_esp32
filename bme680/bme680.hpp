#ifndef BME680_H
#define BME680_H

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

/*
 * Sterownik czujnikow BME680 / BME280 / BMP280 (I2C). Typ ukladu jest rozpoznawany automatycznie po rejestrze ID:
 *     BME680 - temperatura, wilgotnosc, cisnienie (pomiar gazu wylaczony, grzalka nie jest wlaczana)
 *     BME280 - temperatura, wilgotnosc, cisnienie
 *     BMP280 - temperatura, cisnienie (has_humidity = false)
 *
 * Podlaczenie do XIAO ESP32-S3 (takie samo dla wszystkich trzech):
 *     VCC (VIN) -> 3V3
 *     GND       -> GND
 *     SDA       -> D4 (GPIO5)
 *     SCL       -> D5 (GPIO6)
 *     SDO       -> GND (adres 0x76) albo VCC (adres 0x77); sterownik sprawdza oba adresy
 * Linie SDA/SCL wymagaja podciagniec do 3V3 (typowe plytki z BME680 maja rezystory na pokladzie; w razie braku
 * dodaj po 4,7 kOhm). Slabe podciagniecia wewnetrzne ESP sa wlaczone jako zabezpieczenie.
 */

struct Bme680Reading
{
    float temperature_c;
    float humidity_pct; // wazne tylko gdy has_humidity (BMP280 nie mierzy wilgotnosci)
    float pressure_hpa;
    bool has_humidity;
};

class Bme680
{
public:
    static constexpr gpio_num_t SDA_GPIO = GPIO_NUM_5; // XIAO ESP32-S3: D4
    static constexpr gpio_num_t SCL_GPIO = GPIO_NUM_6; // XIAO ESP32-S3: D5

    Bme680() = default;
    ~Bme680() { end(); }
    Bme680(const Bme680 &) = delete;
    Bme680 &operator=(const Bme680 &) = delete;

    // Uruchamia magistrale I2C, znajduje czujnik (0x76 / 0x77), resetuje go i wczytuje kalibracje.
    // Zwraca false, gdy czujnika nie ma lub to inny uklad (np. BME280).
    bool begin();

    // Zwalnia magistrale I2C.
    void end();

    bool ready() const { return dev_ != nullptr; }

    // Nazwa wykrytego ukladu ("BME680", "BME280", "BMP280") albo "brak".
    const char *chip_name() const;

    // Jeden pomiar w trybie "forced" (czujnik sam wraca potem do uspienia). Trwa ok. 30 ms.
    bool read(Bme680Reading &out);

private:
    enum class Chip : uint8_t
    {
        None,
        Bme680,
        Bme280,
        Bmp280,
    };

    // Kalibracja rodziny BMx280 (BME280 / BMP280); h1..h6 uzywane tylko przez BME280.
    struct Calib280
    {
        uint16_t t1;
        int16_t t2;
        int16_t t3;
        uint16_t p1;
        int16_t p2;
        int16_t p3;
        int16_t p4;
        int16_t p5;
        int16_t p6;
        int16_t p7;
        int16_t p8;
        int16_t p9;
        uint8_t h1;
        int16_t h2;
        uint8_t h3;
        int16_t h4;
        int16_t h5;
        int8_t h6;
    };

    struct Calib
    {
        uint16_t t1;
        int16_t t2;
        int8_t t3;
        uint16_t p1;
        int16_t p2;
        int8_t p3;
        int16_t p4;
        int16_t p5;
        int8_t p6;
        int8_t p7;
        int16_t p8;
        int16_t p9;
        uint8_t p10;
        uint16_t h1;
        uint16_t h2;
        int8_t h3;
        int8_t h4;
        int8_t h5;
        uint8_t h6;
        int8_t h7;
    };

    bool read_regs(uint8_t reg, uint8_t *buf, size_t len);
    bool write_reg(uint8_t reg, uint8_t value);
    bool load_calibration();
    bool load_calibration_bmx280();
    bool read_bme680(Bme680Reading &out);
    bool read_bmx280(Bme680Reading &out);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    Chip chip_ = Chip::None;
    Calib cal_ = {};
    Calib280 cal2_ = {};
};

#endif
