#pragma once

#include "esp_err.h"
#include "solar_os_display_surface.h"

/* Native, row-major RGB565 in LCD wire byte order. Storage belongs to caller. */
typedef struct {
    uint8_t *pixels;
    uint16_t width, height;
} solar_os_rgb_panel_buffer_t;

void solar_os_rgb_panel_clear(solar_os_rgb_panel_buffer_t *buffer, uint16_t color);
esp_err_t solar_os_rgb_panel_frame(solar_os_rgb_panel_buffer_t *buffer,
    const solar_os_display_raster_t *frame, solar_os_display_rotation_t rotation);
esp_err_t solar_os_rgb_panel_surface(solar_os_rgb_panel_buffer_t *buffer,
    const solar_os_display_surface_t *surface);
