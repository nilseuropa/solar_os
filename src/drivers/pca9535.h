#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PCA9535_LINE_COUNT 16U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} solar_os_pca9535_io_t;

typedef struct {
    solar_os_pca9535_io_t io;
    uint16_t output, direction;
    bool initialized;
} solar_os_pca9535_t;

/* Direction bits are 1 for input. Output latches are loaded before directions;
 * input polarity is restored to non-inverted. No internal pulls are available. */
esp_err_t solar_os_pca9535_init(solar_os_pca9535_t *device,
    const solar_os_pca9535_io_t *io, uint16_t output, uint16_t direction);
esp_err_t solar_os_pca9535_configure(solar_os_pca9535_t *device, uint8_t line, bool output);
esp_err_t solar_os_pca9535_read(solar_os_pca9535_t *device, uint8_t line, bool *level);
esp_err_t solar_os_pca9535_write(solar_os_pca9535_t *device, uint8_t line, bool level);
