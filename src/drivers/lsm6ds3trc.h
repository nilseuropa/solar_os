#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define LSM6DS3TRC_I2C_ADDRESS_LOW 0x6AU
#define LSM6DS3TRC_I2C_ADDRESS_HIGH 0x6BU
#define LSM6DS3TRC_WHO_AM_I 0x6AU

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} lsm6ds3trc_io_t;

typedef struct {
    float acceleration_m_s2[3];
    float angular_velocity_rad_s[3];
} lsm6ds3trc_sample_t;

typedef struct {
    lsm6ds3trc_io_t io;
    uint8_t identity;
    bool initialized;
} lsm6ds3trc_t;

esp_err_t lsm6ds3trc_init(lsm6ds3trc_t *device, const lsm6ds3trc_io_t *io);
esp_err_t lsm6ds3trc_deinit(lsm6ds3trc_t *device);
esp_err_t lsm6ds3trc_data_ready(lsm6ds3trc_t *device, bool *ready);
esp_err_t lsm6ds3trc_read_sample(lsm6ds3trc_t *device, lsm6ds3trc_sample_t *sample);
