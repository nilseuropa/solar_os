#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_DISPLAY_ROTATION_0,
    SOLAR_OS_DISPLAY_ROTATION_90,
    SOLAR_OS_DISPLAY_ROTATION_180,
    SOLAR_OS_DISPLAY_ROTATION_270,
} solar_os_display_rotation_t;

typedef enum {
    SOLAR_OS_DISPLAY_FORMAT_MONO1 = 0,
    SOLAR_OS_DISPLAY_FORMAT_INDEX8 = 1,
    SOLAR_OS_DISPLAY_FORMAT_INDEX2 = 2,
} solar_os_display_format_t;

#define SOLAR_OS_DISPLAY_FORMAT_BIT(format) (1UL << (unsigned)(format))
#define SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_INDEX8)
#define SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_MONO1)
#define SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_INDEX2)

/* Immutable raster submitted at a frame boundary. MONO1 and INDEX2 pixels are
 * packed least-significant pixel first within each byte. Destination scaling
 * uses nearest-neighbour sampling. If clear_background is true, the presenter
 * composes the destination rectangle over a full-screen background in the
 * same frame transaction. */
typedef struct {
    const uint8_t *data;
    size_t data_size;
    const uint16_t *palette_rgb565;
    size_t palette_size;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t source_stride;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    solar_os_display_format_t format;
    bool palette_inverted;
    bool clear_background;
    uint8_t background_index;
} solar_os_display_raster_t;

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
