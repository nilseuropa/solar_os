#pragma once

#include "solar_os_display_surface.h"
#include "u8g2.h"

/* An opaque monochrome layer over a clean logical canvas. Color rasters retain
 * their colors in a separate backing plane; allocation is only while open. */
typedef struct solar_os_display_layer solar_os_display_layer_t;

solar_os_display_layer_t *solar_os_display_layer_create(u8g2_t *canvas,
    bool black_is_one, bool color);
void solar_os_display_layer_destroy(solar_os_display_layer_t *layer);
uint8_t *solar_os_display_layer_base(solar_os_display_layer_t *layer);
void solar_os_display_layer_uncover(solar_os_display_layer_t *layer);
void solar_os_display_layer_update(solar_os_display_layer_t *layer,
    const uint8_t *bits, uint16_t x, uint16_t y, uint16_t width, uint16_t height);
uint8_t *solar_os_display_layer_canvas(solar_os_display_layer_t *layer, bool base_changed);
void solar_os_display_layer_frame(solar_os_display_layer_t *layer,
    const solar_os_display_raster_t *frame, solar_os_display_raster_t *output);
void solar_os_display_layer_surface(solar_os_display_layer_t *layer,
    const solar_os_display_surface_t *surface, solar_os_display_raster_t *output);
void solar_os_display_layer_raster(solar_os_display_layer_t *layer,
    solar_os_display_raster_t *output);
void solar_os_display_layer_area(solar_os_display_layer_t *layer,
    uint8_t *x, uint8_t *y, uint8_t *width, uint8_t *height);
void solar_os_display_canvas_pack(u8g2_t *canvas, uint8_t *bits,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height);
