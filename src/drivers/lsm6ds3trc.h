#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define LSM6DS3TRC_I2C_ADDRESS_LOW 0x6AU
#define LSM6DS3TRC_I2C_ADDRESS_HIGH 0x6BU
#define LSM6DS3TRC_WHO_AM_I 0x6AU
#define LSM6DS3TRC_EVENT_TAP (1U << 0)
#define LSM6DS3TRC_EVENT_DOUBLE_TAP (1U << 1)
#define LSM6DS3TRC_EVENT_TILT (1U << 2)
#define LSM6DS3TRC_EVENT_WAKE (1U << 3)
#define LSM6DS3TRC_EVENT_FREE_FALL (1U << 4)
#define LSM6DS3TRC_EVENT_ORIENTATION (1U << 5)

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
    bool gestures_enabled;
} lsm6ds3trc_t;

typedef struct {
    uint8_t flags;
    uint8_t wake;
    uint8_t tap;
    uint8_t function;
    uint8_t orientation;
} lsm6ds3trc_events_t;

esp_err_t lsm6ds3trc_init(lsm6ds3trc_t *device, const lsm6ds3trc_io_t *io);
esp_err_t lsm6ds3trc_deinit(lsm6ds3trc_t *device);
esp_err_t lsm6ds3trc_data_ready(lsm6ds3trc_t *device, bool *ready);
esp_err_t lsm6ds3trc_read_sample(lsm6ds3trc_t *device, lsm6ds3trc_sample_t *sample);
esp_err_t lsm6ds3trc_enable_gestures(lsm6ds3trc_t *device);
esp_err_t lsm6ds3trc_read_events(lsm6ds3trc_t *device, lsm6ds3trc_events_t *events);
