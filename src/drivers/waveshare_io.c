#include "waveshare_io.h"
#include <string.h>

static esp_err_t write_byte(solar_os_ws_io_chip_t *d, uint8_t reg, uint8_t value)
{
    return d->io.write(d->io.ctx, reg, &value, 1);
}

esp_err_t solar_os_ws_io_chip_init(solar_os_ws_io_chip_t *d,
    const solar_os_ws_io_transport_t *io, uint8_t output, uint8_t direction)
{
    if (!d || !io || !io->read || !io->write) return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    d->io = *io;
    /* Preload the latch before changing the power-on input directions. */
    esp_err_t err = write_byte(d, 0x03, output);
    if (err == ESP_OK) err = write_byte(d, 0x02, direction);
    if (err == ESP_OK) {
        d->output = output;
        d->direction = direction;
        d->ready = true;
    }
    return err;
}

esp_err_t solar_os_ws_io_chip_configure(solar_os_ws_io_chip_t *d, uint8_t line, bool output)
{
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    if (line >= 8) return ESP_ERR_INVALID_ARG;
    const uint8_t direction = output ? d->direction | (1U << line) : d->direction & ~(1U << line);
    if (direction == d->direction) return ESP_OK;
    const esp_err_t err = write_byte(d, 0x02, direction);
    if (err == ESP_OK) d->direction = direction;
    return err;
}

esp_err_t solar_os_ws_io_chip_write(solar_os_ws_io_chip_t *d, uint8_t line, bool level)
{
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    if (line >= 8) return ESP_ERR_INVALID_ARG;
    const uint8_t output = level ? d->output | (1U << line) : d->output & ~(1U << line);
    if (output != d->output) {
        const esp_err_t err = write_byte(d, 0x03, output);
        if (err != ESP_OK) return err;
        d->output = output;
    }
    return solar_os_ws_io_chip_configure(d, line, true);
}

esp_err_t solar_os_ws_io_chip_read(solar_os_ws_io_chip_t *d, uint8_t line, bool *level)
{
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    if (line >= 8 || !level) return ESP_ERR_INVALID_ARG;
    uint8_t value;
    const esp_err_t err = d->io.read(d->io.ctx, 0x04, &value, 1);
    if (err == ESP_OK) *level = (value & (1U << line)) != 0;
    return err;
}

esp_err_t solar_os_ws_io_chip_pwm(solar_os_ws_io_chip_t *d, uint8_t duty)
{
    return d && d->ready ? write_byte(d, 0x05, duty) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_ws_io_chip_adc(solar_os_ws_io_chip_t *d, uint16_t *raw)
{
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    if (!raw) return ESP_ERR_INVALID_ARG;
    uint8_t data[2];
    const esp_err_t err = d->io.read(d->io.ctx, 0x06, data, sizeof(data));
    if (err != ESP_OK) return err;
    const uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (value > 1023U) return ESP_ERR_INVALID_RESPONSE;
    *raw = value;
    return ESP_OK;
}
