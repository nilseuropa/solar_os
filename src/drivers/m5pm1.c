#include "m5pm1.h"

#include <string.h>

#define REG_PWR_SRC 0x04U
#define REG_I2C_CFG 0x09U
#define REG_WDT_CNT 0x0AU
#define REG_SYS_CMD 0x0CU
#define REG_GPIO_MODE 0x10U
#define REG_GPIO_OUT 0x11U
#define REG_GPIO_IN 0x12U
#define REG_GPIO_DRV 0x13U
#define REG_GPIO_FUNC0 0x16U
#define REG_GPIO_FUNC1 0x17U
#define REG_VBAT_L 0x22U
#define REG_VIN_L 0x24U
#define REG_PWM0_L 0x30U
#define REG_PWM_FREQ_L 0x34U

#define LINE_MASK ((1U << M5PM1_LINE_COUNT) - 1U)

/* GPIO_DRV resets to 0x1F (all open-drain) and only bits 4:0 belong to
 * GPIO0-4; bit 5 is LED_EN and is left untouched. */
static esp_err_t set_push_pull(m5pm1_t *device, uint8_t mask)
{
    uint8_t drive = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_GPIO_DRV, &drive, 1U);
    if (ret != ESP_OK || (drive & mask) == 0U) {
        return ret;
    }
    drive = (uint8_t)(drive & (uint8_t)~mask);
    return device->io.write(device->io.ctx, REG_GPIO_DRV, &drive, 1U);
}

/* Select the plain GPIO function (00) for the given lines. */
static esp_err_t set_gpio_function(m5pm1_t *device, uint8_t mask)
{
    uint8_t func0 = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_GPIO_FUNC0, &func0, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t next0 = func0;
    for (uint8_t line = 0U; line < 4U; line++) {
        if ((mask & (1U << line)) != 0U) {
            next0 = (uint8_t)(next0 & (uint8_t)~(0x03U << (line * 2U)));
        }
    }
    if (next0 != func0) {
        ret = device->io.write(device->io.ctx, REG_GPIO_FUNC0, &next0, 1U);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if ((mask & (1U << 4U)) != 0U) {
        uint8_t func1 = 0U;
        ret = device->io.read(device->io.ctx, REG_GPIO_FUNC1, &func1, 1U);
        if (ret == ESP_OK && (func1 & 0x03U) != 0U) {
            func1 = (uint8_t)(func1 & (uint8_t)~0x03U);
            ret = device->io.write(device->io.ctx, REG_GPIO_FUNC1, &func1, 1U);
        }
    }
    return ret;
}

static bool valid_device(const m5pm1_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t write_u8(m5pm1_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, sizeof(value));
}

static esp_err_t read_u8(m5pm1_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, sizeof(*value));
}

static esp_err_t read_mv(m5pm1_t *device, uint8_t reg, uint16_t *millivolts)
{
    uint8_t raw[2] = {0};
    const esp_err_t ret = device->io.read(device->io.ctx, reg, raw, sizeof(raw));
    if (ret == ESP_OK) {
        *millivolts = (uint16_t)raw[0] | ((uint16_t)(raw[1] & 0x0FU) << 8U);
    }
    return ret;
}

esp_err_t m5pm1_init(m5pm1_t *device,
                     const m5pm1_io_t *io,
                     uint8_t initial_output,
                     uint8_t initial_direction)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    /* Disable I2C idle sleep (SLP_TO = 0) and keep 100 kHz. */
    esp_err_t ret = write_u8(device, REG_I2C_CFG, 0x00U);
    /* WDT_CNT survives reset and power-off; a watchdog armed by earlier
     * firmware would keep restarting a system that never feeds it. M5GFX
     * clears it the same way at Paper Mono power-on. */
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_WDT_CNT, 0x00U);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t output = (uint8_t)(initial_output & LINE_MASK);
    const uint8_t direction = (uint8_t)(initial_direction & LINE_MASK);
    ret = write_u8(device, REG_GPIO_OUT, output);
    if (ret == ESP_OK && direction != 0U) {
        ret = set_gpio_function(device, direction);
    }
    if (ret == ESP_OK && direction != 0U) {
        ret = set_push_pull(device, direction);
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_GPIO_MODE, direction);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    device->output = output;
    device->direction = direction;
    device->initialized = true;
    return ESP_OK;
}

esp_err_t m5pm1_deinit(m5pm1_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t m5pm1_configure(m5pm1_t *device, uint8_t line, bool output)
{
    if (!valid_device(device) || line >= M5PM1_LINE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = (uint8_t)(1U << line);
    uint8_t direction = device->direction;
    if (output) {
        direction = (uint8_t)(direction | mask);
        const esp_err_t drive_ret = set_push_pull(device, mask);
        if (drive_ret != ESP_OK) {
            return drive_ret;
        }
    } else {
        direction = (uint8_t)(direction & (uint8_t)~mask);
    }
    const esp_err_t ret = write_u8(device, REG_GPIO_MODE, direction);
    if (ret == ESP_OK) {
        device->direction = direction;
    }
    return ret;
}

esp_err_t m5pm1_read(m5pm1_t *device, uint8_t line, bool *level)
{
    if (!valid_device(device) || line >= M5PM1_LINE_COUNT || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value = 0U;
    const uint8_t reg = ((device->direction & (1U << line)) != 0U)
        ? REG_GPIO_OUT
        : REG_GPIO_IN;
    const esp_err_t ret = read_u8(device, reg, &value);
    if (ret == ESP_OK) {
        *level = (value & (uint8_t)(1U << line)) != 0U;
    }
    return ret;
}

esp_err_t m5pm1_write(m5pm1_t *device, uint8_t line, bool level)
{
    if (!valid_device(device) || line >= M5PM1_LINE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = (uint8_t)(1U << line);
    uint8_t output = device->output;
    if (level) {
        output = (uint8_t)(output | mask);
    } else {
        output = (uint8_t)(output & (uint8_t)~mask);
    }
    const esp_err_t ret = write_u8(device, REG_GPIO_OUT, output);
    if (ret == ESP_OK) {
        device->output = output;
    }
    return ret;
}

esp_err_t m5pm1_read_vbat_mv(m5pm1_t *device, uint16_t *millivolts)
{
    if (!valid_device(device) || millivolts == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_mv(device, REG_VBAT_L, millivolts);
}

esp_err_t m5pm1_read_vin_mv(m5pm1_t *device, uint16_t *millivolts)
{
    if (!valid_device(device) || millivolts == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_mv(device, REG_VIN_L, millivolts);
}

esp_err_t m5pm1_read_power_source(m5pm1_t *device, uint8_t *source)
{
    if (!valid_device(device) || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_u8(device, REG_PWR_SRC, source);
}

esp_err_t m5pm1_shutdown(m5pm1_t *device)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_u8(device, REG_SYS_CMD,
                    (uint8_t)(M5PM1_SYS_CMD_KEY | M5PM1_SYS_CMD_SHUTDOWN));
}

esp_err_t m5pm1_configure_pwm0(m5pm1_t *device, uint16_t frequency_hz)
{
    if (!valid_device(device) || frequency_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* GPIO3 special function is PWM0 (2-bit field at bits 7:6 of FUNC0). */
    uint8_t func0 = 0U;
    esp_err_t ret = read_u8(device, REG_GPIO_FUNC0, &func0);
    if (ret != ESP_OK) {
        return ret;
    }
    func0 = (uint8_t)((func0 & 0x3FU) | (uint8_t)(M5PM1_GPIO_FUNC_PWM0 << 6U));
    ret = write_u8(device, REG_GPIO_FUNC0, func0);
    if (ret != ESP_OK) {
        return ret;
    }
    /* Drive type outranks the PWM function: open-drain PWM cannot drive
     * the frontlight enable high (M5GFX clears this bit too). */
    ret = set_push_pull(device, 1U << 3U);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t freq[2] = {
        (uint8_t)(frequency_hz & 0xFFU),
        (uint8_t)(frequency_hz >> 8U),
    };
    return device->io.write(device->io.ctx, REG_PWM_FREQ_L, freq, sizeof(freq));
}

esp_err_t m5pm1_set_pwm0_duty(m5pm1_t *device, uint16_t duty_12bit, bool enable)
{
    if (!valid_device(device) || duty_12bit > 4095U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t payload[2] = {
        (uint8_t)(duty_12bit & 0xFFU),
        (uint8_t)(((duty_12bit >> 8U) & 0x0FU) | (enable ? 0x10U : 0x00U)),
    };
    return device->io.write(device->io.ctx, REG_PWM0_L, payload, sizeof(payload));
}
