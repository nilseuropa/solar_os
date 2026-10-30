#pragma once

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

#define SOLAR_OS_DISPLAY_SCALE_MAX 4U

typedef struct {
    u8g2_t *canvas;
    const u8x8_display_info_t *physical_info;
    u8x8_display_info_t logical_info;
    u8x8_msg_cb physical_callback;
    uint8_t *physical_buffer;
    uint8_t physical_rows;
    uint8_t factor;
    uint8_t native_depth;
    bool dirty;
    uint8_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    uint8_t *logical_buffer;
    size_t logical_capacity;
    uint8_t *surface_buffer;
    size_t surface_capacity;
    uint8_t *raster_buffer;
    size_t raster_capacity;
} solar_os_display_scale_t;

void solar_os_display_scale_init(solar_os_display_scale_t *state, u8g2_t *canvas);
void solar_os_display_scale_destroy(solar_os_display_scale_t *state);
esp_err_t solar_os_display_scale_prepare(solar_os_display_scale_t *state, uint8_t factor);
void solar_os_display_scale_apply(solar_os_display_scale_t *state, uint8_t factor);
void solar_os_display_scale_sync_native(solar_os_display_scale_t *state);
void solar_os_display_scale_begin_native(solar_os_display_scale_t *state);
void solar_os_display_scale_end_native(solar_os_display_scale_t *state);
esp_err_t solar_os_display_scale_raster(solar_os_display_scale_t *state,
    const solar_os_display_raster_t *source, solar_os_display_raster_t *output);
esp_err_t solar_os_display_scale_bitmap(solar_os_display_scale_t *state,
    const solar_os_display_raster_t *source, solar_os_display_raster_t *output);
esp_err_t solar_os_display_scale_surface(solar_os_display_scale_t *state,
    const solar_os_display_surface_t *source, solar_os_display_surface_t *output);
