#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_XPT2046_ADC_MAX 4095U
#define SOLAR_OS_XPT2046_PRESSURE_DEFAULT 400U

typedef struct {
    /* One full-duplex, mode-0, 24-clock command/conversion transaction. */
    esp_err_t (*transfer)(void *context, const uint8_t tx[3], uint8_t rx[3]);
    void *context;
} solar_os_xpt2046_io_t;

typedef struct {
    bool valid, touched;
    uint16_t x, y, pressure;
} solar_os_xpt2046_sample_t;

esp_err_t solar_os_xpt2046_idle(const solar_os_xpt2046_io_t *io);
/* Read filtered 12-bit differential X/Y and pressure. Always attempts to end
 * in PD=00, restoring PENIRQ and low-power idle, including after an I/O error.
 * No chip identity register exists: a successful transfer is not a probe. */
esp_err_t solar_os_xpt2046_read(const solar_os_xpt2046_io_t *io,
    uint16_t minimum_pressure, solar_os_xpt2046_sample_t *sample);
