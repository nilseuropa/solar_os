#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MAX17048_I2C_ADDRESS 0x36U

typedef esp_err_t (*max17048_read_fn_t)(void *user,
                                       uint8_t reg,
                                       uint8_t *data,
                                       size_t len);

typedef struct {
    max17048_read_fn_t read;
    void *user;
} max17048_io_t;

typedef struct {
    uint16_t voltage_mv;
    uint8_t percent;
    uint16_t soc_raw;
} max17048_sample_t;

typedef struct {
    max17048_io_t io;
    uint16_t version;
    bool initialized;
} max17048_t;

esp_err_t max17048_init(max17048_t *device, const max17048_io_t *io);
esp_err_t max17048_read_sample(max17048_t *device,
                               max17048_sample_t *sample);
