#pragma once
#include "solar_os_display_types_v1.h"

typedef struct {
    const uint8_t *data;
    size_t data_size;
    const uint16_t *palette_rgb565;
    size_t palette_size;
    const uint8_t *dirty_tiles;
    size_t dirty_size;
    uint32_t *presented_hashes;
    size_t presented_hash_count;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint16_t native_width;
    uint16_t native_height;
    uint16_t dirty_stride;
    uint16_t hash_stride;
    uint8_t tile_size;
    solar_os_display_format_t format;
    solar_os_display_rotation_t rotation;
} solar_os_display_surface_t;
