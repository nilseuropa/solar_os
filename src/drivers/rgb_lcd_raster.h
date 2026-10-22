#pragma once
#include "esp_err.h"
#include "solar_os_display_surface.h"

/* Render into native-endian RGB565 scanout memory. Input RGB565 stays MSB-first.
 * Validation completes before the first destination write. */
esp_err_t solar_os_rgb_lcd_render(uint16_t *output, uint16_t width, uint16_t height,
    const solar_os_display_raster_t *frame, uint16_t foreground, uint16_t background);
esp_err_t solar_os_rgb_lcd_render_surface(uint16_t *output, uint16_t width, uint16_t height,
    const solar_os_display_surface_t *surface, uint16_t foreground, uint16_t background);
