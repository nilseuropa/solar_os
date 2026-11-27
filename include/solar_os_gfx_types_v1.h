#pragma once
#include "solar_os_application_types_v1.h"
#include "solar_os_display_types_v1.h"

typedef uint32_t solar_os_gfx_color_t;

typedef enum {
    SOLAR_OS_GFX_COLOR_WHITE,
    SOLAR_OS_GFX_COLOR_LIGHT,
    SOLAR_OS_GFX_COLOR_DARK,
    SOLAR_OS_GFX_COLOR_BLACK,
} solar_os_gfx_color_index_t;

#define SOLAR_OS_GFX_GRAY_MAX 16U
#define SOLAR_OS_GFX_COLOR_GRAY_BASE 16U
#define SOLAR_OS_GFX_COLOR_GRAY_LAST (SOLAR_OS_GFX_COLOR_GRAY_BASE + SOLAR_OS_GFX_GRAY_MAX)
#define SOLAR_OS_GFX_COLOR_RGB_FLAG 0x01000000UL

typedef enum {
    SOLAR_OS_GFX_FONT_SMALL,
    SOLAR_OS_GFX_FONT_MONO,
    SOLAR_OS_GFX_FONT_BOLD,
    SOLAR_OS_GFX_FONT_MONO_12,
    SOLAR_OS_GFX_FONT_MONO_14,
    SOLAR_OS_GFX_FONT_MONO_16,
    SOLAR_OS_GFX_FONT_MONO_18,
    SOLAR_OS_GFX_FONT_MONO_20,
    SOLAR_OS_GFX_FONT_BOLD_12,
    SOLAR_OS_GFX_FONT_BOLD_14,
    SOLAR_OS_GFX_FONT_BOLD_16,
    SOLAR_OS_GFX_FONT_BOLD_18,
    SOLAR_OS_GFX_FONT_BOLD_20,
    SOLAR_OS_GFX_FONT_ITALIC_12,
    SOLAR_OS_GFX_FONT_ITALIC_14,
    SOLAR_OS_GFX_FONT_ITALIC_16,
    SOLAR_OS_GFX_FONT_ITALIC_18,
    SOLAR_OS_GFX_FONT_ITALIC_20,
    SOLAR_OS_GFX_FONT_BOLD_ITALIC_12,
    SOLAR_OS_GFX_FONT_BOLD_ITALIC_14,
    SOLAR_OS_GFX_FONT_BOLD_ITALIC_16,
    SOLAR_OS_GFX_FONT_BOLD_ITALIC_18,
    SOLAR_OS_GFX_FONT_BOLD_ITALIC_20,
    SOLAR_OS_GFX_FONT_BITMAP_8,
    SOLAR_OS_GFX_FONT_COUNT,
} solar_os_gfx_font_t;

typedef enum {
    SOLAR_OS_GFX_LINE_SOLID,
    SOLAR_OS_GFX_LINE_DOTTED,
    SOLAR_OS_GFX_LINE_DASHED,
} solar_os_gfx_line_style_t;



typedef struct {
    int x;
    int y;
} solar_os_gfx_point_t;

typedef enum {
    SOLAR_OS_GFX_RASTER_GRAY8,
    SOLAR_OS_GFX_RASTER_RGB888,
} solar_os_gfx_raster_format_t;

typedef struct {
    const uint8_t *pixels;
    size_t pixels_size;
    uint32_t width;
    uint32_t height;
    size_t stride;
    solar_os_gfx_raster_format_t format;
} solar_os_gfx_raster_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} solar_os_gfx_clip_t;
