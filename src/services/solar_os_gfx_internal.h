#pragma once

#include "solar_os_gfx.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

typedef struct solar_os_gfx_index8_surface solar_os_gfx_index8_surface_t;
typedef struct solar_os_gfx_snapshot solar_os_gfx_snapshot_t;

struct solar_os_gfx {
    u8g2_t *u8g2;
    solar_os_gfx_color_t color;
    solar_os_gfx_font_t font;
    solar_os_gfx_line_style_t line_style;
    bool dirty;
    bool black_is_one;
    bool palette_inverted;
    solar_os_gfx_index8_surface_t *index8;
};

void solar_os_gfx_init(solar_os_gfx_t *gfx, u8g2_t *u8g2);
void solar_os_gfx_set_black_is_one(solar_os_gfx_t *gfx, bool black_is_one);
void solar_os_gfx_set_palette_inverted(solar_os_gfx_t *gfx, bool inverted);
void solar_os_gfx_prepare_surface(solar_os_gfx_t *gfx);
void solar_os_gfx_release_surface(solar_os_gfx_t *gfx);
esp_err_t solar_os_gfx_enable_index8(solar_os_gfx_t *gfx);
void *solar_os_gfx_detach_surface_storage(solar_os_gfx_t *gfx);
const solar_os_display_surface_t *solar_os_gfx_surface(
    const solar_os_gfx_t *gfx);
esp_err_t solar_os_gfx_snapshot_capture(
    const solar_os_gfx_t *gfx,
    solar_os_gfx_snapshot_t **snapshot);
esp_err_t solar_os_gfx_snapshot_restore(
    solar_os_gfx_t *gfx,
    const solar_os_gfx_snapshot_t *snapshot);
void solar_os_gfx_snapshot_destroy(solar_os_gfx_snapshot_t *snapshot);
