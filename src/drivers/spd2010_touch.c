/* Protocol reference: Espressif esp_lcd_touch_spd2010 (Apache-2.0),
 * esp-iot-solution, commit da53cfaa90b63085ef21f721f7593b5e2747d448.
 * SolarOS implementation uses callback I/O and bounded packet draining. */
#include "spd2010_touch.h"

#define PACKET_MAX 64U
#define DRAIN_MAX 8U

static bool valid(const solar_os_spd2010_touch_io_t *io)
{
    return io && io->write && io->read && io->delay_us;
}

static esp_err_t read_register(const solar_os_spd2010_touch_io_t *io,
    uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t address[] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
    esp_err_t error = io->write(io->ctx, address, sizeof(address));
    if (error) return error;
    io->delay_us(io->ctx, 200);
    error = io->read(io->ctx, data, len);
    io->delay_us(io->ctx, 200);
    return error;
}

static esp_err_t command(const solar_os_spd2010_touch_io_t *io, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, 0, value, 0};
    esp_err_t error = io->write(io->ctx, data, sizeof(data));
    io->delay_us(io->ctx, 200);
    return error;
}

esp_err_t solar_os_spd2010_touch_probe(const solar_os_spd2010_touch_io_t *io)
{
    if (!valid(io)) return ESP_ERR_INVALID_ARG;
    uint8_t version[18];
    return read_register(io, 0x0026, version, sizeof(version));
}

static esp_err_t drain(const solar_os_spd2010_touch_io_t *io)
{
    for (unsigned attempt = 0; attempt < DRAIN_MAX; ++attempt) {
        uint8_t status[8];
        esp_err_t error = read_register(io, 0x02fc, status, sizeof(status));
        if (error) return error;
        if (status[5] == 0x82) return command(io, 2, 1);
        if (status[5] != 0) return ESP_ERR_INVALID_RESPONSE;
        size_t len = (size_t)status[2] | ((size_t)status[3] << 8);
        if (!len || len > PACKET_MAX) return ESP_ERR_INVALID_RESPONSE;
        uint8_t ignored[PACKET_MAX];
        error = read_register(io, 0x0300, ignored, len);
        if (error) return error;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t solar_os_spd2010_touch_read(const solar_os_spd2010_touch_io_t *io,
    solar_os_spd2010_touch_sample_t *sample)
{
    if (!valid(io) || !sample) return ESP_ERR_INVALID_ARG;
    *sample = (solar_os_spd2010_touch_sample_t){0};
    uint8_t status[4];
    esp_err_t error = read_register(io, 0x0020, status, sizeof(status));
    if (error) return error;
    uint16_t len = (uint16_t)status[2] | ((uint16_t)status[3] << 8);
    if (status[1] & 0x80) return ESP_OK; /* Busy: preserve the current pointer. */
    if (status[1] & 0x40) {
        error = command(io, 2, 1);
        return error ? error : command(io, 4, 1); /* BIOS -> CPU. */
    }
    if (status[1] & 0x20) {
        error = command(io, 0x50, 0);
        if (!error) error = command(io, 0x46, 0);
        return error ? error : command(io, 2, 1);
    }
    if ((status[1] & 8) && len == 0) {
        error = command(io, 2, 1);
        if (!error) sample->valid = true; /* Explicit empty report releases. */
        return error;
    }
    if (status[0] & 3) {
        if (len < 4 || len > PACKET_MAX) return ESP_ERR_INVALID_RESPONSE;
        uint8_t data[PACKET_MAX];
        error = read_register(io, 0x0300, data, len);
        if (error) return error;
        solar_os_spd2010_touch_sample_t result = {.valid = true};
        if ((status[0] & 1) && (len - 4U) % 6U == 0) {
            for (size_t i = 4; i < len; i += 6) {
                if (data[i] > 10U) return ESP_ERR_INVALID_RESPONSE;
                if (!result.touched && data[i + 4]) {
                    result.touched = true; result.id = data[i];
                    result.x = ((uint16_t)(data[i + 3] & 0xf0) << 4) | data[i + 1];
                    result.y = ((uint16_t)(data[i + 3] & 0x0f) << 8) | data[i + 2];
                }
            }
        } else if (!((status[0] & 2) && len >= 7 && data[4] == 0xf6)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        error = drain(io);
        if (!error) *sample = result;
        return error;
    }
    return (status[1] & 8) && (status[0] & 8) ? command(io, 2, 1) : ESP_OK;
}
