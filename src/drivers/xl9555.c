#include "xl9555.h"

#include <string.h>

#define XL9555_REG_INPUT_PORT0 0x00U
#define XL9555_REG_OUTPUT_PORT0 0x02U
#define XL9555_REG_CONFIG_PORT0 0x06U

static bool valid_device(const xl9555_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t write_u16(xl9555_t *device, uint8_t reg, uint16_t value)
{
    /* Keep the two ports as distinct register transactions. This preserves
     * the known-good XL9555 rail sequencing used during board bring-up. */
    const uint8_t low = (uint8_t)(value & 0xFFU);
    esp_err_t ret = device->io.write(device->io.ctx, reg, &low, sizeof(low));
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t high = (uint8_t)(value >> 8U);
    return device->io.write(device->io.ctx,
                            (uint8_t)(reg + 1U),
                            &high,
                            sizeof(high));
}

esp_err_t xl9555_init(xl9555_t *device,
                      const xl9555_io_t *io,
                      uint16_t initial_output,
                      uint16_t initial_direction)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;

    /* Preload both output latches before enabling any output. */
    esp_err_t ret = write_u16(device, XL9555_REG_OUTPUT_PORT0, initial_output);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ret = write_u16(device, XL9555_REG_CONFIG_PORT0, initial_direction);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->output = initial_output;
    device->direction = initial_direction;
    device->initialized = true;
    return ESP_OK;
}

esp_err_t xl9555_deinit(xl9555_t *device)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t xl9555_configure(xl9555_t *device, uint8_t line, bool output)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (line >= XL9555_LINE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t mask = (uint16_t)(1U << line);
    const uint16_t direction = output
        ? (uint16_t)(device->direction & (uint16_t)~mask)
        : (uint16_t)(device->direction | mask);
    if (direction == device->direction) {
        return ESP_OK;
    }
    const esp_err_t ret = write_u16(device, XL9555_REG_CONFIG_PORT0, direction);
    if (ret == ESP_OK) {
        device->direction = direction;
    }
    return ret;
}

esp_err_t xl9555_read(xl9555_t *device, uint8_t line, bool *level)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (line >= XL9555_LINE_COUNT || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[2];
    const esp_err_t ret = device->io.read(device->io.ctx,
                                          XL9555_REG_INPUT_PORT0,
                                          data,
                                          sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }
    const uint16_t input = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
    *level = (input & (uint16_t)(1U << line)) != 0U;
    return ESP_OK;
}

esp_err_t xl9555_write(xl9555_t *device, uint8_t line, bool level)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (line >= XL9555_LINE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t mask = (uint16_t)(1U << line);
    const uint16_t output = level
        ? (uint16_t)(device->output | mask)
        : (uint16_t)(device->output & (uint16_t)~mask);
    if (output != device->output) {
        const esp_err_t ret = write_u16(device, XL9555_REG_OUTPUT_PORT0, output);
        if (ret != ESP_OK) {
            return ret;
        }
        device->output = output;
    }
    return xl9555_configure(device, line, true);
}
