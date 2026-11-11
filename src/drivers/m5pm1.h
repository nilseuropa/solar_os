#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define M5PM1_LINE_COUNT 5U
#define M5PM1_I2C_ADDR_DEFAULT 0x6EU
#define M5PM1_SYS_CMD_KEY 0xA0U
#define M5PM1_SYS_CMD_SHUTDOWN 0x01U
#define M5PM1_SYS_CMD_REBOOT 0x02U
#define M5PM1_SYS_CMD_DOWNLOAD 0x03U
#define M5PM1_PWR_SRC_VIN 0x01U
#define M5PM1_PWR_SRC_VINOUT 0x02U
#define M5PM1_PWR_SRC_BAT 0x04U
#define M5PM1_GPIO_FUNC_GPIO 0U
#define M5PM1_GPIO_FUNC_PWM0 3U /* GPIO3 special function */

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} m5pm1_io_t;

typedef struct {
    m5pm1_io_t io;
    uint8_t output;
    uint8_t direction;
    bool initialized;
} m5pm1_t;

esp_err_t m5pm1_init(m5pm1_t *device,
                     const m5pm1_io_t *io,
                     uint8_t initial_output,
                     uint8_t initial_direction);
esp_err_t m5pm1_deinit(m5pm1_t *device);
esp_err_t m5pm1_configure(m5pm1_t *device, uint8_t line, bool output);
esp_err_t m5pm1_read(m5pm1_t *device, uint8_t line, bool *level);
esp_err_t m5pm1_write(m5pm1_t *device, uint8_t line, bool level);
esp_err_t m5pm1_read_vbat_mv(m5pm1_t *device, uint16_t *millivolts);
esp_err_t m5pm1_read_vin_mv(m5pm1_t *device, uint16_t *millivolts);
esp_err_t m5pm1_read_power_source(m5pm1_t *device, uint8_t *source);
esp_err_t m5pm1_shutdown(m5pm1_t *device);
esp_err_t m5pm1_configure_pwm0(m5pm1_t *device, uint16_t frequency_hz);
esp_err_t m5pm1_set_pwm0_duty(m5pm1_t *device, uint16_t duty_12bit, bool enable);
