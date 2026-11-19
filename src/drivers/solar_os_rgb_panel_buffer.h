#pragma once

#include "esp_err.h"
#include "solar_os_display_surface.h"

/* Native, row-major RGB565 in LCD wire byte order. Storage belongs to caller. */
typedef struct {
    uint8_t *pixels;
    uint16_t width, height;
    /* Optional caller-owned bitset, ceil(height/8) bytes, marking changed rows. */
    uint8_t *dirty_rows;
} solar_os_rgb_panel_buffer_t;

static inline void solar_os_rgb_panel_put(solar_os_rgb_panel_buffer_t *b,
    uint16_t x, uint16_t y, uint16_t color)
{
    uint8_t *p = b->pixels + ((size_t)y * b->width + x) * 2U;
    const uint8_t hi = color >> 8, lo = color;
    if (p[0] == hi && p[1] == lo) return;
    p[0] = hi; p[1] = lo;
    if (b->dirty_rows) b->dirty_rows[y / 8U] |= 1U << (y & 7U);
}

void solar_os_rgb_panel_clear(solar_os_rgb_panel_buffer_t *buffer, uint16_t color);
esp_err_t solar_os_rgb_panel_frame(solar_os_rgb_panel_buffer_t *buffer,
    const solar_os_display_raster_t *frame, solar_os_display_rotation_t rotation);
esp_err_t solar_os_rgb_panel_surface(solar_os_rgb_panel_buffer_t *buffer,
    const solar_os_display_surface_t *surface);
/* changed_only requires the backing image to contain the previous surface. */
esp_err_t solar_os_rgb_panel_surface_update(solar_os_rgb_panel_buffer_t *buffer,
    const solar_os_display_surface_t *surface, bool changed_only);
