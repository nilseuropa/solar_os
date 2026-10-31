#pragma once

#include "esp_err.h"

typedef enum {
    CL32_DISPLAY_UNKNOWN,
    CL32_DISPLAY_ST7305,
    CL32_DISPLAY_SSD1685,
} cl32_display_controller_t;

/* Identify only CL-32's known panel variants; never cache a panel choice. */
esp_err_t cl32_display_identify(const char *spi_bus, int cs, int dc, int reset,
                               int mosi, cl32_display_controller_t *controller);
