#include "m5ioe1.h"

#include <string.h>

#define M5IOE1_REG_GPIO_MODE_L 0x03U
#define M5IOE1_REG_GPIO_OUT_L  0x05U
#define M5IOE1_REG_GPIO_IN_L   0x07U
#define M5IOE1_REG_GPIO_DRV_L  0x13U
#define M5IOE1_REG_I2C_CFG     0x23U

#define M5IOE1_LINE_MASK ((1U << M5IOE1_LINE_COUNT) - 1U)

static bool valid_device(const m5ioe1_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t write_u8(m5ioe1_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, sizeof(value));
}

static esp_err_t write_u16(m5ioe1_t *device, uint8_t reg, uint16_t value)
{
    const uint8_t low = (uint8_t)(value & 0xFFU);
    esp_err_t ret = device->io.write(device->io.ctx, reg, &low, sizeof(low));
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t high = (uint8_t)(value >> 8U);
    return device->io.write(device->io.ctx, (uint8_t)(reg + 1U), &high, sizeof(high));
}

static esp_err_t read_u16(m5ioe1_t *device, uint8_t reg, uint16_t *value)
{
    uint8_t low = 0U;
    uint8_t high = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, reg, &low, sizeof(low));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = device->io.read(device->io.ctx, (uint8_t)(reg + 1U), &high, sizeof(high));
    if (ret == ESP_OK) {
        *value = (uint16_t)low | ((uint16_t)high << 8U);
    }
    return ret;
}

esp_err_t m5ioe1_init(m5ioe1_t *device,
                      const m5ioe1_io_t *io,
                      uint16_t initial_output,
                      uint16_t initial_direction)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    /* Disable I2C idle sleep so the expander stays reachable. */
    esp_err_t ret = write_u8(device, M5IOE1_REG_I2C_CFG, 0x00U);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint16_t output = (uint16_t)(initial_output & M5IOE1_LINE_MASK);
    const uint16_t direction = (uint16_t)(initial_direction & M5IOE1_LINE_MASK);
    /* Apply output latches, then push-pull drive, before enabling output
     * directions. GPIO_DRV resets to open-drain (0xFF/0x3F) whenever the
     * expander loses power, so a released "high" would float. */
    uint16_t drive = 0U;
    ret = write_u16(device, M5IOE1_REG_GPIO_OUT_L, output);
    if (ret == ESP_OK) {
        ret = read_u16(device, M5IOE1_REG_GPIO_DRV_L, &drive);
    }
    if (ret == ESP_OK) {
        ret = write_u16(device, M5IOE1_REG_GPIO_DRV_L,
                        (uint16_t)(drive & (uint16_t)~direction & M5IOE1_LINE_MASK));
    }
    if (ret == ESP_OK) {
        ret = write_u16(device, M5IOE1_REG_GPIO_MODE_L, direction);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    device->output = output;
    device->direction = direction;
    device->initialized = true;
    return ESP_OK;
}

esp_err_t m5ioe1_deinit(m5ioe1_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t m5ioe1_configure(m5ioe1_t *device, uint8_t line, bool output)
{
    if (!valid_device(device) || line >= M5IOE1_LINE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t mask = (uint16_t)(1U << line);
    uint16_t direction = device->direction;
    if (output) {
        direction = (uint16_t)(direction | mask);
        uint16_t drive = 0U;
        esp_err_t drive_ret = read_u16(device, M5IOE1_REG_GPIO_DRV_L, &drive);
        if (drive_ret == ESP_OK && (drive & mask) != 0U) {
            drive_ret = write_u16(device, M5IOE1_REG_GPIO_DRV_L,
                                  (uint16_t)(drive & (uint16_t)~mask));
        }
        if (drive_ret != ESP_OK) {
            return drive_ret;
        }
    } else {
        direction = (uint16_t)(direction & (uint16_t)~mask);
    }
    const esp_err_t ret = write_u16(device, M5IOE1_REG_GPIO_MODE_L, direction);
    if (ret == ESP_OK) {
        device->direction = direction;
    }
    return ret;
}

esp_err_t m5ioe1_read(m5ioe1_t *device, uint8_t line, bool *level)
{
    if (!valid_device(device) || line >= M5IOE1_LINE_COUNT || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t value = 0U;
    const uint8_t reg = ((device->direction & (1U << line)) != 0U)
        ? M5IOE1_REG_GPIO_OUT_L
        : M5IOE1_REG_GPIO_IN_L;
    const esp_err_t ret = read_u16(device, reg, &value);
    if (ret == ESP_OK) {
        *level = (value & (uint16_t)(1U << line)) != 0U;
    }
    return ret;
}

esp_err_t m5ioe1_write(m5ioe1_t *device, uint8_t line, bool level)
{
    if (!valid_device(device) || line >= M5IOE1_LINE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t mask = (uint16_t)(1U << line);
    uint16_t output = device->output;
    if (level) {
        output = (uint16_t)(output | mask);
    } else {
        output = (uint16_t)(output & (uint16_t)~mask);
    }
    const esp_err_t ret = write_u16(device, M5IOE1_REG_GPIO_OUT_L, output);
    if (ret == ESP_OK) {
        device->output = output;
    }
    return ret;
}
