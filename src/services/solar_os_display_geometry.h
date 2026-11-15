#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "u8g2.h"
#include "solar_os_display_surface.h"

/* Panel coordinates describe the existing framebuffer. Canvas coordinates used
 * by apps start at (0,0); the canvas is a layout area, not a clipping window. */
typedef struct {
    solar_os_display_rect_t panel, canvas, native_canvas;
    uint16_t native_width, native_height;
    uint16_t rotation;
    uint8_t scale;
    bool round;
} solar_os_display_geometry_t;

#ifdef __cplusplus
extern "C" {
#endif
void solar_os_display_geometry_init(solar_os_display_geometry_t *geometry,
    uint16_t width, uint16_t height, bool round);
void solar_os_display_geometry_update(solar_os_display_geometry_t *geometry,
    uint16_t rotation, uint8_t scale);
bool solar_os_display_geometry_set_canvas(solar_os_display_geometry_t *geometry,
    const solar_os_display_rect_t *canvas);
#ifdef __cplusplus
}
#endif

/* SolarOS reserves the otherwise unused u8g2 user pointer for display geometry.
 * Drivers retain their full panel dimensions and framebuffer allocation. */
static inline solar_os_display_geometry_t *solar_os_display_geometry_for(const u8g2_t *display)
{
    return display ? (solar_os_display_geometry_t *)u8g2_GetUserPtr(display) : NULL;
}

static inline solar_os_display_rect_t solar_os_display_canvas_rect(const u8g2_t *display)
{
    solar_os_display_geometry_t *g = solar_os_display_geometry_for(display);
    return g ? g->canvas : (solar_os_display_rect_t){0, 0,
        display ? u8g2_GetDisplayWidth(display) : 0,
        display ? u8g2_GetDisplayHeight(display) : 0};
}

static inline void solar_os_display_geometry_sync_rotation(u8g2_t *display)
{
    solar_os_display_geometry_t *g = solar_os_display_geometry_for(display);
    if (g) solar_os_display_geometry_update(g,
        display->cb == U8G2_R1 ? 90 : display->cb == U8G2_R2 ? 180 :
        display->cb == U8G2_R3 ? 270 : 0, g->scale);
}
