#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define XL9555_LINE_COUNT 16U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} xl9555_io_t;

typedef struct {
    xl9555_io_t io;
    uint16_t output;
    uint16_t direction;
    bool initialized;
} xl9555_t;

esp_err_t xl9555_init(xl9555_t *device,
                      const xl9555_io_t *io,
                      uint16_t initial_output,
                      uint16_t initial_direction);
esp_err_t xl9555_deinit(xl9555_t *device);
esp_err_t xl9555_configure(xl9555_t *device, uint8_t line, bool output);
esp_err_t xl9555_read(xl9555_t *device, uint8_t line, bool *level);
esp_err_t xl9555_write(xl9555_t *device, uint8_t line, bool level);
