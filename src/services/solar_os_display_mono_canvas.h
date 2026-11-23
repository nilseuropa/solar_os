#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_display_surface.h"

/* Native, unrotated U8g2 coordinates: each byte holds eight vertical pixels,
 * least-significant bit at the top. Bit polarity follows the native driver.
 * The optional fused
 * presenter must also update native_buffer, preserving exports and overlays.
 * Source pixels are borrowed only for the duration of the callback. */
typedef struct {
    const uint8_t *data;
    size_t data_size;
    uint16_t width, height, stride;
    uint8_t scale;
    uint8_t *native_buffer;
    uint16_t native_width, native_height, native_stride;
    solar_os_display_rect_t area;
} solar_os_display_mono_canvas_t;

typedef esp_err_t (*solar_os_display_mono_canvas_presenter_t)(
    void *context, const solar_os_display_mono_canvas_t *canvas);

static inline uint32_t solar_os_display_mono_expand(uint8_t value, unsigned factor)
{
    uint32_t bits = value;
    if (factor == 1U) return bits;
    if (factor == 2U) {
        bits = (bits | (bits << 4U)) & 0x0f0fU;
        bits = (bits | (bits << 2U)) & 0x3333U;
        bits = (bits | (bits << 1U)) & 0x5555U;
        return bits * 3U;
    }
    if (factor == 3U) {
        bits = (bits | (bits << 8U)) & 0x00f00fU;
        bits = (bits | (bits << 4U)) & 0x0c30c3U;
        bits = (bits | (bits << 2U)) & 0x249249U;
        return bits * 7U;
    }
    bits = (bits | (bits << 12U)) & 0x000f000fU;
    bits = (bits | (bits << 6U)) & 0x03030303U;
    bits = (bits | (bits << 3U)) & 0x11111111U;
    return bits * 15U;
}

/* Low result byte is the top row, with its left pixel in bit 7. */
static inline uint64_t solar_os_display_mono_transpose(const uint8_t columns[8])
{
    uint64_t pixels, swap;
    memcpy(&pixels, columns, sizeof(pixels));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    pixels = __builtin_bswap64(pixels);
#endif
    swap = (pixels ^ (pixels >> 7)) & UINT64_C(0x00aa00aa00aa00aa);
    pixels ^= swap ^ (swap << 7);
    swap = (pixels ^ (pixels >> 14)) & UINT64_C(0x0000cccc0000cccc);
    pixels ^= swap ^ (swap << 14);
    swap = (pixels ^ (pixels >> 28)) & UINT64_C(0x00000000f0f0f0f0);
    return pixels ^ swap ^ (swap << 28);
}

typedef struct {
    uint32_t columns[8], rows[8];
} solar_os_display_mono_tile_t;

/* One logical 8x8 tile produces scale*8 native columns and rows. Transpose
 * once before expansion, rather than once per native tile (nine at scale 3).
 * Padding outside the logical canvas matches the regular U8g2 scaler. */
static inline void solar_os_display_mono_canvas_expand_tile(
    const solar_os_display_mono_canvas_t *canvas, unsigned x, unsigned y,
    solar_os_display_mono_tile_t *tile)
{
    const unsigned available = y < canvas->height ? canvas->height-y : 0;
    const uint8_t mask = available >= 8U ? 0xffU : (1U << available) - 1U;
    uint8_t columns[8];
    for (unsigned i = 0; i < 8U; i++) {
        columns[i] = available && x+i < canvas->width ?
            canvas->data[(size_t)(y/8U)*canvas->stride+x+i] & mask : 0;
        tile->columns[i] = solar_os_display_mono_expand(columns[i],canvas->scale);
    }
    const uint64_t packed = solar_os_display_mono_transpose(columns);
    for (unsigned row = 0; row < 8U; row++)
        tile->rows[row] = solar_os_display_mono_expand((uint8_t)(packed >> (row*8U)),canvas->scale);
}
