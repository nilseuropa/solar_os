#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_display_surface.h"

/* Portable raster primitives; buffers are validated by their callers. RGB565
 * uses high byte first, including on little-endian CPUs. Compact RGB888 in
 * place before scaling: source reads always precede overlapping writes. */
static inline void solar_os_rgb565_from_rgb888(uint8_t *pixels, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const uint8_t *rgb = pixels + i * 3U;
        const uint16_t color = ((uint16_t)(rgb[0] & 0xf8U) << 8U) |
            ((uint16_t)(rgb[1] & 0xfcU) << 3U) | (rgb[2] >> 3U);
        pixels[i * 2U] = color >> 8U;
        pixels[i * 2U + 1U] = color;
    }
}

static inline void solar_os_rgb565_scale_row(const uint8_t *source,
    uint16_t source_width, uint8_t *output, uint16_t output_width)
{
    if (source_width == output_width) {
        memcpy(output, source, (size_t)output_width * 2U);
        return;
    }
    uint32_t source_x = 0, remainder = 0;
    for (uint16_t x = 0; x < output_width; x++) {
        output[x * 2U] = source[source_x * 2U];
        output[x * 2U + 1U] = source[source_x * 2U + 1U];
        remainder += source_width;
        while (remainder >= output_width) {
            remainder -= output_width; source_x++;
        }
    }
}

static inline void solar_os_rgb565_rotate(const uint8_t *source,
    uint16_t width, uint16_t height, size_t source_stride,
    uint8_t *output, size_t output_stride, solar_os_display_rotation_t rotation)
{
    if (rotation == SOLAR_OS_DISPLAY_ROTATION_0) {
        for (uint16_t y = 0; y < height; y++)
            memcpy(output + (size_t)y * output_stride,
                source + (size_t)y * source_stride, (size_t)width * 2U);
        return;
    }
    if (rotation == SOLAR_OS_DISPLAY_ROTATION_180) {
        for (uint16_t y = 0; y < height; y++) {
            const uint8_t *row = source + (size_t)y * source_stride;
            uint8_t *out = output + (size_t)(height - 1U - y) * output_stride;
            for (uint16_t x = 0; x < width; x++) {
                out[(width - 1U - x) * 2U] = row[x * 2U];
                out[(width - 1U - x) * 2U + 1U] = row[x * 2U + 1U];
            }
        }
        return;
    }
    /* Stage a small tile in internal stack memory. Both PSRAM reads and writes
     * become contiguous row copies rather than repeated column stores. memcpy
     * preserves arbitrary alignment and RGB565 wire byte order. */
    uint16_t tile[16][16], row[16];
    for (unsigned y0 = 0; y0 < height; y0 += 16U)
        for (unsigned x0 = 0; x0 < width; x0 += 16U) {
            const unsigned rows = height - y0 < 16U ? height - y0 : 16U;
            const unsigned cols = width - x0 < 16U ? width - x0 : 16U;
            for (unsigned y = 0; y < rows; y++)
                memcpy(tile[y], source + (size_t)(y0 + y) * source_stride + x0 * 2U, cols * 2U);
            for (unsigned x = 0; x < cols; x++) {
                uint8_t *dst;
                if (rotation == SOLAR_OS_DISPLAY_ROTATION_90) {
                    for (unsigned y = 0; y < rows; y++) row[y] = tile[rows - 1U - y][x];
                    dst = output + (size_t)(x0 + x) * output_stride + (height - y0 - rows) * 2U;
                } else {
                    for (unsigned y = 0; y < rows; y++) row[y] = tile[y][x];
                    dst = output + (size_t)(width - 1U - x0 - x) * output_stride + y0 * 2U;
                }
                memcpy(dst, row, rows * 2U);
            }
        }
}
