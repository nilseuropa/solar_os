#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DRV2605_EFFECT_COUNT 117U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx,
                       uint8_t reg,
                       const uint8_t *data,
                       size_t len);
    void (*delay_ms)(void *ctx, uint32_t delay_ms);
    void *ctx;
} drv2605_io_t;

typedef struct {
    drv2605_io_t io;
    uint8_t chip_id;
    bool initialized;
} drv2605_t;

esp_err_t drv2605_init(drv2605_t *device, const drv2605_io_t *io);
esp_err_t drv2605_deinit(drv2605_t *device);
esp_err_t drv2605_play_effect(drv2605_t *device, uint16_t effect);
esp_err_t drv2605_stop(drv2605_t *device);
