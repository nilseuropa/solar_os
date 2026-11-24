#include "xpt2046.h"

#include <string.h>

/* XPT2046 datasheet, differential 12-bit mode, 24 clocks per conversion:
 * https://files.waveshare.com/wiki/common/XPT2046_Datasheet.pdf
 * PD=01 keeps the ADC powered during a burst; PD=00 restores PENIRQ. */
#define XPT_Y 0x90U
#define XPT_X 0xd0U
#define XPT_Z1 0xb0U
#define XPT_Z2 0xc0U
#define XPT_SAMPLES 5U

static esp_err_t xpt_adc(const solar_os_xpt2046_io_t *io, uint8_t command, uint16_t *value)
{
    const uint8_t tx[3] = {command, 0, 0};
    uint8_t rx[3] = {0};
    esp_err_t err = io->transfer(io->context, tx, rx);
    if (err == ESP_OK) *value = (((uint16_t)rx[1] << 8U) | rx[2]) >> 3U & 4095U;
    return err;
}

esp_err_t solar_os_xpt2046_idle(const solar_os_xpt2046_io_t *io)
{
    if (!io || !io->transfer) return ESP_ERR_INVALID_ARG;
    uint16_t ignored;
    return xpt_adc(io, XPT_Y, &ignored);
}

static esp_err_t xpt_settled(const solar_os_xpt2046_io_t *io, uint8_t channel, uint16_t *value)
{
    uint16_t ignored;
    esp_err_t err = xpt_adc(io, channel | 1U, &ignored);
    return err == ESP_OK ? xpt_adc(io, channel | 1U, value) : err;
}

static esp_err_t xpt_pressure(const solar_os_xpt2046_io_t *io, uint16_t *pressure)
{
    uint16_t z1, z2;
    esp_err_t err = xpt_settled(io, XPT_Z1, &z1);
    if (err == ESP_OK) err = xpt_settled(io, XPT_Z2, &z2);
    if (err == ESP_OK) {
        /* Reject open/floating or stuck-high/low data, and invalid Z ordering. */
        *pressure = z1 > 0U && z1 < z2 ? (uint16_t)(z1 + 4095U - z2) : 0U;
    }
    return err;
}

static esp_err_t xpt_axis(const solar_os_xpt2046_io_t *io, uint8_t channel, uint16_t *value)
{
    uint16_t values[XPT_SAMPLES], ignored;
    esp_err_t err = xpt_adc(io, channel | 1U, &ignored);
    for (size_t i = 0; err == ESP_OK && i < XPT_SAMPLES; ++i) {
        err = xpt_adc(io, channel | 1U, &values[i]);
        if (err != ESP_OK) break;
        for (size_t j = i; j > 0 && values[j] < values[j - 1U]; --j) {
            uint16_t tmp = values[j]; values[j] = values[j - 1U]; values[j - 1U] = tmp;
        }
    }
    if (err == ESP_OK) *value = values[XPT_SAMPLES / 2U];
    return err;
}

esp_err_t solar_os_xpt2046_read(const solar_os_xpt2046_io_t *io,
    uint16_t minimum_pressure, solar_os_xpt2046_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    if (!io || !io->transfer || !minimum_pressure || minimum_pressure > 4095U) return ESP_ERR_INVALID_ARG;
    uint16_t before = 0, after = 0, x = 0, y = 0;
    esp_err_t err = xpt_pressure(io, &before);
    if (err == ESP_OK && before >= minimum_pressure) {
        err = xpt_axis(io, XPT_X, &x);
        if (err == ESP_OK) err = xpt_axis(io, XPT_Y, &y);
        if (err == ESP_OK) err = xpt_pressure(io, &after);
    }
    esp_err_t idle = solar_os_xpt2046_idle(io);
    if (err != ESP_OK) return err;
    if (idle != ESP_OK) return idle;
    sample->valid = true;
    sample->pressure = before < after ? before : after;
    sample->touched = sample->pressure >= minimum_pressure;
    if (sample->touched) { sample->x = x; sample->y = y; }
    return ESP_OK;
}
