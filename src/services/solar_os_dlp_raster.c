#include "solar_os_dlp_raster.h"

#include <stddef.h>
#include <string.h>

bool solar_os_dlp_mono_region(const uint8_t *image, const uint8_t *previous,
    uint16_t width, uint16_t height, solar_os_dlp_region_t *region)
{
    *region = (solar_os_dlp_region_t){0, 0, width, height};
    if (!previous) return width && height;
    size_t stride = (width + 7U) / 8U;
    unsigned left = width, right = 0, top = height, bottom = 0;
    for (unsigned y = 0; y < height; y++) {
        const uint8_t *a = image + y * stride, *b = previous + y * stride;
        if (!memcmp(a, b, stride)) continue;
        for (unsigned x = 0; x < stride; x++) {
            uint8_t changed = a[x] ^ b[x];
            if (x + 1 == stride && width % 8U)
                changed &= (1U << (width % 8U)) - 1U;
            if (!changed) continue;
            if (x * 8U < left) left = x * 8U;
            if ((x + 1U) * 8U > right) right = (x + 1U) * 8U;
            if (y < top) top = y;
            bottom = y + 1U;
        }
    }
    if (left == width) return false;
    if (right > width) right = width;
    *region = (solar_os_dlp_region_t){left, top, right - left, bottom - top};
    return true;
}

size_t solar_os_dlp_mono_regions(const uint8_t *image, const uint8_t *previous,
    uint16_t width, uint16_t height, solar_os_dlp_region_t *regions, size_t capacity)
{
    if (!previous) return solar_os_dlp_mono_region(image, NULL, width, height, regions) ? 1 : 0;
    size_t stride = (width + 7U) / 8U, count = 0;
    bool adjacent = false;
    for (unsigned y = 0; y < height; y += 8U) {
        unsigned rows = height - y < 8U ? height - y : 8U;
        solar_os_dlp_region_t region;
        if (!solar_os_dlp_mono_region(image + y * stride, previous + y * stride,
            width, rows, &region)) {
            adjacent = false;
            continue;
        }
        region.y += y;
        if (count && (adjacent || count == capacity)) {
            solar_os_dlp_region_t *last = &regions[count - 1];
            unsigned right = last->x + last->width;
            if (region.x + region.width > right) right = region.x + region.width;
            if (region.x < last->x) last->x = region.x;
            last->width = right - last->x;
            last->height = region.y + region.height - last->y;
        } else {
            regions[count++] = region;
        }
        adjacent = true;
    }
    return count;
}

void solar_os_dlp_mono_index2(const uint8_t *image, uint16_t image_width,
    const solar_os_dlp_region_t *region, uint8_t *output)
{
    static const uint8_t expand[] = {
        0x00, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f,
        0xc0, 0xc3, 0xcc, 0xcf, 0xf0, 0xf3, 0xfc, 0xff,
    };
    size_t input_stride = (image_width + 7U) / 8U;
    size_t output_stride = (region->width + 3U) / 4U;
    for (unsigned y = 0; y < region->height; y++) {
        const uint8_t *src = image + (region->y + y) * input_stride + region->x / 8U;
        uint8_t *dst = output + y * output_stride;
        size_t pairs = region->width / 8U;
        for (size_t x = 0; x < pairs; x++) {
            uint8_t pixels = src[x];
            dst[x * 2U] = expand[pixels & 15U];
            dst[x * 2U + 1U] = expand[pixels >> 4];
        }
        unsigned tail = region->width % 8U;
        if (tail) {
            uint8_t pixels = src[pairs] & ((1U << tail) - 1U);
            dst[pairs * 2U] = expand[pixels & 15U];
            if (tail > 4) dst[pairs * 2U + 1U] = expand[pixels >> 4];
        }
    }
}
