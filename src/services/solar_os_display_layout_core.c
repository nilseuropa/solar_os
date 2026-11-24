#include "solar_os_display_layout_core.h"

#include <limits.h>
#include <string.h>

static size_t mono_stride(uint16_t width)
{
    return ((size_t)width + 7U) & ~(size_t)7U;
}

bool solar_os_display_layout_split_geometry(
    solar_os_display_layout_axis_t axis,
    uint16_t width,
    uint16_t height,
    solar_os_display_layout_rect_t regions[2])
{
    if (regions == NULL || width == 0U || height == 0U) {
        return false;
    }

    if (axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL) {
        if (width < 2U) {
            return false;
        }
        const uint16_t first = width / 2U;
        regions[0] = (solar_os_display_layout_rect_t){0U, 0U, first, height};
        regions[1] = (solar_os_display_layout_rect_t){
            first, 0U, (uint16_t)(width - first), height};
        return true;
    }
    if (axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL) {
        if (height < 2U) {
            return false;
        }
        const uint16_t first = height / 2U;
        regions[0] = (solar_os_display_layout_rect_t){0U, 0U, width, first};
        regions[1] = (solar_os_display_layout_rect_t){
            0U, first, width, (uint16_t)(height - first)};
        return true;
    }
    return false;
}

bool solar_os_display_layout_join_geometry(
    solar_os_display_layout_axis_t axis,
    const uint16_t *widths,
    const uint16_t *heights,
    size_t count,
    uint16_t *width,
    uint16_t *height,
    solar_os_display_layout_rect_t *regions)
{
    if ((axis != SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL &&
         axis != SOLAR_OS_DISPLAY_LAYOUT_VERTICAL) ||
        widths == NULL || heights == NULL || count == 0U ||
        width == NULL || height == NULL || regions == NULL) {
        return false;
    }

    uint32_t total = 0U;
    uint16_t cross = 0U;
    for (size_t i = 0; i < count; i++) {
        if (widths[i] == 0U || heights[i] == 0U) {
            return false;
        }
        const uint16_t length = axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL ?
            widths[i] : heights[i];
        const uint16_t member_cross = axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL ?
            heights[i] : widths[i];
        total += length;
        if (total > UINT16_MAX) {
            return false;
        }
        if (member_cross > cross) {
            cross = member_cross;
        }
    }

    uint16_t offset = 0U;
    for (size_t i = 0; i < count; i++) {
        regions[i] = (solar_os_display_layout_rect_t){
            .x = axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL ? offset : 0U,
            .y = axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL ? offset : 0U,
            .width = widths[i],
            .height = heights[i],
        };
        offset = (uint16_t)(offset +
            (axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL ? widths[i] : heights[i]));
    }

    *width = axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL ? (uint16_t)total : cross;
    *height = axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL ? (uint16_t)total : cross;
    return true;
}

size_t solar_os_display_layout_mono_size(uint16_t width, uint16_t height)
{
    if (width == 0U || height == 0U) {
        return 0U;
    }
    return mono_stride(width) * (((size_t)height + 7U) / 8U);
}

bool solar_os_display_layout_mono_get(const uint8_t *buffer,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t x,
                                      uint16_t y)
{
    if (buffer == NULL || x >= width || y >= height) {
        return false;
    }
    const size_t offset = (size_t)(y / 8U) * mono_stride(width) + x;
    return (buffer[offset] & (uint8_t)(1U << (y & 7U))) != 0U;
}

void solar_os_display_layout_mono_set(uint8_t *buffer,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t x,
                                      uint16_t y,
                                      bool set)
{
    if (buffer == NULL || x >= width || y >= height) {
        return;
    }
    const size_t offset = (size_t)(y / 8U) * mono_stride(width) + x;
    const uint8_t mask = (uint8_t)(1U << (y & 7U));
    if (set) {
        buffer[offset] |= mask;
    } else {
        buffer[offset] &= (uint8_t)~mask;
    }
}

static bool mono_copy_region(
    uint8_t *destination,
    uint16_t destination_width,
    uint16_t destination_height,
    uint16_t destination_x,
    uint16_t destination_y,
    const uint8_t *source,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t source_x,
    uint16_t source_y,
    uint16_t width,
    uint16_t height)
{
    if (destination == NULL || source == NULL || width == 0U ||
        height == 0U || destination_x > destination_width ||
        destination_y > destination_height ||
        source_x > source_width || source_y > source_height ||
        width > (uint16_t)(destination_width - destination_x) ||
        height > (uint16_t)(destination_height - destination_y) ||
        width > (uint16_t)(source_width - source_x) ||
        height > (uint16_t)(source_height - source_y)) {
        return false;
    }

    const size_t destination_stride = mono_stride(destination_width);
    const size_t source_stride = mono_stride(source_width);
    while (height > 0U) {
        const unsigned destination_shift = destination_y & 7U;
        const unsigned source_shift = source_y & 7U;
        const unsigned rows = height < 8U - destination_shift ?
            height : 8U - destination_shift;
        uint8_t *out = destination +
            (size_t)(destination_y / 8U) * destination_stride + destination_x;
        const uint8_t *in = source +
            (size_t)(source_y / 8U) * source_stride + source_x;
        if (rows == 8U && source_shift == 0U) {
            memcpy(out, in, width);
        } else {
            const uint8_t mask = (uint8_t)(((1U << rows) - 1U) << destination_shift);
            for (uint16_t x = 0U; x < width; x++) {
                unsigned value = in[x] >> source_shift;
                if (source_shift + rows > 8U) {
                    value |= (unsigned)in[source_stride + x] << (8U - source_shift);
                }
                out[x] = (uint8_t)((out[x] & (uint8_t)~mask) |
                    ((value << destination_shift) & mask));
            }
        }
        destination_y = (uint16_t)(destination_y + rows);
        source_y = (uint16_t)(source_y + rows);
        height = (uint16_t)(height - rows);
    }
    return true;
}

bool solar_os_display_layout_mono_blit(
    uint8_t *destination,
    uint16_t destination_width,
    uint16_t destination_height,
    uint16_t destination_x,
    uint16_t destination_y,
    const uint8_t *source,
    uint16_t source_width,
    uint16_t source_height)
{
    return mono_copy_region(destination, destination_width, destination_height,
                             destination_x, destination_y, source,
                             source_width, source_height, 0U, 0U,
                             source_width, source_height);
}

bool solar_os_display_layout_mono_extract(
    uint8_t *destination,
    uint16_t width,
    uint16_t height,
    const uint8_t *source,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t source_x,
    uint16_t source_y)
{
    return mono_copy_region(destination, width, height, 0U, 0U, source,
                             source_width, source_height, source_x, source_y,
                             width, height);
}
