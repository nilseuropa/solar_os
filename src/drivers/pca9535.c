#include "pca9535.h"
#include <string.h>

enum { INPUT = 0, OUTPUT = 2, POLARITY = 4, CONFIGURATION = 6 };

static bool valid(const solar_os_pca9535_t *device)
{
    return device && device->initialized;
}

static esp_err_t write_port(solar_os_pca9535_t *device, uint8_t reg,
    uint8_t port, uint8_t value, uint16_t *cache)
{
    esp_err_t error = device->io.write(device->io.ctx, reg + port, &value, 1);
    if (error == ESP_OK && cache) {
        const unsigned shift = port * 8U;
        *cache = (uint16_t)((*cache & ~(UINT16_C(0xff) << shift)) |
                            ((uint16_t)value << shift));
    }
    return error;
}

esp_err_t solar_os_pca9535_init(solar_os_pca9535_t *device,
    const solar_os_pca9535_io_t *io, uint16_t output, uint16_t direction)
{
    if (!device || !io || !io->read || !io->write) return ESP_ERR_INVALID_ARG;
    memset(device, 0, sizeof(*device));
    device->io = *io;
    for (uint8_t port = 0; port < 2; ++port) {
        esp_err_t error = write_port(device, OUTPUT, port,
            (uint8_t)(output >> (port * 8)), &device->output);
        if (error) return error;
    }
    for (uint8_t port = 0; port < 2; ++port) {
        esp_err_t error = write_port(device, POLARITY, port, 0, NULL);
        if (error) return error;
    }
    for (uint8_t port = 0; port < 2; ++port) {
        esp_err_t error = write_port(device, CONFIGURATION, port,
            (uint8_t)(direction >> (port * 8)), &device->direction);
        if (error) return error;
    }
    device->initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_pca9535_configure(solar_os_pca9535_t *device, uint8_t line, bool output)
{
    if (!valid(device)) return ESP_ERR_INVALID_STATE;
    if (line >= PCA9535_LINE_COUNT) return ESP_ERR_INVALID_ARG;
    uint16_t mask = (uint16_t)(1U << line);
    uint16_t value = output ? device->direction & ~mask : device->direction | mask;
    if (value == device->direction) return ESP_OK;
    return write_port(device, CONFIGURATION, line / 8,
        (uint8_t)(value >> ((line / 8) * 8)), &device->direction);
}

esp_err_t solar_os_pca9535_read(solar_os_pca9535_t *device, uint8_t line, bool *level)
{
    if (!valid(device)) return ESP_ERR_INVALID_STATE;
    if (line >= PCA9535_LINE_COUNT || !level) return ESP_ERR_INVALID_ARG;
    uint8_t value;
    esp_err_t error = device->io.read(device->io.ctx, INPUT + line / 8, &value, 1);
    if (!error) *level = (value & (1U << (line % 8))) != 0;
    return error;
}

esp_err_t solar_os_pca9535_write(solar_os_pca9535_t *device, uint8_t line, bool level)
{
    if (!valid(device)) return ESP_ERR_INVALID_STATE;
    if (line >= PCA9535_LINE_COUNT) return ESP_ERR_INVALID_ARG;
    uint16_t mask = (uint16_t)(1U << line);
    uint16_t value = level ? device->output | mask : device->output & ~mask;
    if (value != device->output) {
        esp_err_t error = write_port(device, OUTPUT, line / 8,
            (uint8_t)(value >> ((line / 8) * 8)), &device->output);
        if (error) return error;
    }
    return solar_os_pca9535_configure(device, line, true);
}
