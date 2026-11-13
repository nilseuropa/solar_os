#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BMI270_I2C_ADDR_DEFAULT 0x68U
#define BMI270_CHIP_ID 0x24U

typedef struct {
    float accel_m_s2[3];
    float gyro_rad_s[3];
    bool accel_valid;
    bool gyro_valid;
} bmi270_sample_t;

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} bmi270_io_t;

typedef struct {
    bmi270_io_t io;
    bool initialized;
} bmi270_t;

esp_err_t bmi270_init(bmi270_t *device, const bmi270_io_t *io);
esp_err_t bmi270_deinit(bmi270_t *device);
esp_err_t bmi270_read_sample(bmi270_t *device, bmi270_sample_t *sample);
