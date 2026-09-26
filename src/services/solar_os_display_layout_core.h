#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL,
    SOLAR_OS_DISPLAY_LAYOUT_VERTICAL,
} solar_os_display_layout_axis_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} solar_os_display_layout_rect_t;

bool solar_os_display_layout_split_geometry(
    solar_os_display_layout_axis_t axis,
    uint16_t width,
    uint16_t height,
    solar_os_display_layout_rect_t regions[2]);

bool solar_os_display_layout_join_geometry(
    solar_os_display_layout_axis_t axis,
    const uint16_t *widths,
    const uint16_t *heights,
    size_t count,
    uint16_t *width,
    uint16_t *height,
    solar_os_display_layout_rect_t *regions);

size_t solar_os_display_layout_mono_size(uint16_t width, uint16_t height);
bool solar_os_display_layout_mono_get(const uint8_t *buffer,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t x,
                                      uint16_t y);
void solar_os_display_layout_mono_set(uint8_t *buffer,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t x,
                                      uint16_t y,
                                      bool set);
bool solar_os_display_layout_mono_blit(
    uint8_t *destination,
    uint16_t destination_width,
    uint16_t destination_height,
    uint16_t destination_x,
    uint16_t destination_y,
    const uint8_t *source,
    uint16_t source_width,
    uint16_t source_height);
