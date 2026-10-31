#pragma once

#include "esp_err.h"

typedef enum {
    EPD_STICKY_UNKNOWN,
    EPD_STICKY_SSD1677,
    EPD_STICKY_SSD2677,
} epd_sticky_controller_t;

/* Restricted to Sticky's known 800x480 panel variants. No NVS cache: a panel
 * replacement is identified at the next attachment/boot. */
esp_err_t epd_sticky_identify(const char *spi_bus, int cs, int dc, int reset,
    int busy, int power, epd_sticky_controller_t *controller);
