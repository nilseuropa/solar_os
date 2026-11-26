#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SHT40_SENSOR_ADDRESS 0x44U
#define SHT40_CMD_MEASURE_HIGH 0xfdU
#define SHT40_CMD_SOFT_RESET 0x94U
#define SHT40_MEASURE_DELAY_MS 10U

static inline uint8_t sht40_word_crc(const uint8_t *data)
{
    uint8_t crc = 0xff;
    for (unsigned i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x31 : 0));
        }
    }
    return crc;
}

static inline bool sht40_decode_sample(const uint8_t data[6], float *temperature_c, float *humidity_percent)
{
    if (sht40_word_crc(data) != data[2] || sht40_word_crc(data + 3) != data[5]) return false;
    const uint16_t raw_temperature = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t raw_humidity = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)raw_temperature / 65535.0f);
    const float humidity = -6.0f + 125.0f * ((float)raw_humidity / 65535.0f);
    *humidity_percent = humidity < 0.0f ? 0.0f : humidity > 100.0f ? 100.0f : humidity;
    return true;
}
