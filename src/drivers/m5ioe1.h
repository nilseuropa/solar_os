#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define M5IOE1_LINE_COUNT 14U
#define M5IOE1_I2C_ADDR_DEFAULT 0x4FU

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} m5ioe1_io_t;

typedef struct {
    m5ioe1_io_t io;
    uint16_t output;
    uint16_t direction; /* bit=1 means output, matching SolarOS gpio controllers */
    bool initialized;
} m5ioe1_t;

esp_err_t m5ioe1_init(m5ioe1_t *device,
                      const m5ioe1_io_t *io,
                      uint16_t initial_output,
                      uint16_t initial_direction);
esp_err_t m5ioe1_deinit(m5ioe1_t *device);
esp_err_t m5ioe1_configure(m5ioe1_t *device, uint8_t line, bool output);
esp_err_t m5ioe1_read(m5ioe1_t *device, uint8_t line, bool *level);
esp_err_t m5ioe1_write(m5ioe1_t *device, uint8_t line, bool level);
