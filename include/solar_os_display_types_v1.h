#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int x, y, width, height;
} solar_os_display_rect_t;

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
    /* Two bytes per pixel, most-significant byte first (LCD wire order).
     * No palette, inversion or quantization. */
    SOLAR_OS_DISPLAY_FORMAT_RGB565 = 3,
} solar_os_display_format_t;

#define SOLAR_OS_DISPLAY_FORMAT_BIT(format) (1UL << (unsigned)(format))
#define SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_INDEX8)
#define SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_MONO1)
#define SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_INDEX2)
#define SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT \
    SOLAR_OS_DISPLAY_FORMAT_BIT(SOLAR_OS_DISPLAY_FORMAT_RGB565)

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
    uint16_t background_rgb565; /* Used instead of background_index for RGB565. */
} solar_os_display_raster_t;

/* Indexed working canvas presented synchronously. A driver may update the
 * cache of presented tile hashes during the call. All data is borrowed. */
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

/* BEGIN saves the image and suspends ordinary presentation. BLACK/WHITE
 * perform synchronous full refreshes. END restores the saved image and
 * refresh policy, including after a failed recovery frame. */
typedef enum {
    SOLAR_OS_DISPLAY_RELAX_BEGIN,
    SOLAR_OS_DISPLAY_RELAX_BLACK,
    SOLAR_OS_DISPLAY_RELAX_WHITE,
    SOLAR_OS_DISPLAY_RELAX_END,
} solar_os_display_relax_phase_t;

typedef enum {
    SOLAR_OS_DISPLAY_THEME_FOREGROUND,
    SOLAR_OS_DISPLAY_THEME_BACKGROUND,
} solar_os_display_theme_role_t;

typedef struct {
    uint32_t foreground_rgb888;
    uint32_t background_rgb888;
} solar_os_display_theme_v1_t;
