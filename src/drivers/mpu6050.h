#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MPU6050_I2C_ADDRESS_LOW 0x68U
#define MPU6050_I2C_ADDRESS_HIGH 0x69U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} mpu6050_io_t;

typedef struct {
    float acceleration_m_s2[3], angular_velocity_rad_s[3];
} mpu6050_sample_t;

typedef struct { mpu6050_io_t io; bool initialized; } mpu6050_t;

/* 100 Hz, DLPF 3 (accel 44 Hz / gyro 42 Hz), +/-2 g, +/-250 deg/s.
 * IRQ, when used, is active high and latched until INT_STATUS is read. */
esp_err_t mpu6050_init(mpu6050_t *device, const mpu6050_io_t *io);
esp_err_t mpu6050_deinit(mpu6050_t *device);
esp_err_t mpu6050_data_ready(mpu6050_t *device, bool *ready);
esp_err_t mpu6050_read_sample(mpu6050_t *device, mpu6050_sample_t *sample);
