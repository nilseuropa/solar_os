#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_GFX_COLOR_WHITE,
    SOLAR_OS_GFX_COLOR_LIGHT,
    SOLAR_OS_GFX_COLOR_DARK,
    SOLAR_OS_GFX_COLOR_BLACK,
} solar_os_gfx_color_t;

#define SOLAR_OS_GFX_GRAY_MAX 16U
#define SOLAR_OS_GFX_COLOR_GRAY_BASE 16U
#define SOLAR_OS_GFX_COLOR_GRAY_LAST (SOLAR_OS_GFX_COLOR_GRAY_BASE + SOLAR_OS_GFX_GRAY_MAX)

typedef enum {
    SOLAR_OS_GFX_FONT_SMALL,
    SOLAR_OS_GFX_FONT_MONO,
    SOLAR_OS_GFX_FONT_BOLD,
} solar_os_gfx_font_t;

typedef enum {
    SOLAR_OS_GFX_LINE_SOLID,
    SOLAR_OS_GFX_LINE_DOTTED,
    SOLAR_OS_GFX_LINE_DASHED,
} solar_os_gfx_line_style_t;

typedef struct solar_os_gfx solar_os_gfx_t;

typedef struct {
    int x;
    int y;
} solar_os_gfx_point_t;

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx);
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx);
solar_os_gfx_color_t solar_os_gfx_gray(uint8_t level);
bool solar_os_gfx_color_is_valid(solar_os_gfx_color_t color);
void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color);
solar_os_gfx_color_t solar_os_gfx_color(const solar_os_gfx_t *gfx);
void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font);
solar_os_gfx_font_t solar_os_gfx_font(const solar_os_gfx_t *gfx);
void solar_os_gfx_set_line_style(solar_os_gfx_t *gfx, solar_os_gfx_line_style_t style);
solar_os_gfx_line_style_t solar_os_gfx_line_style(const solar_os_gfx_t *gfx);
void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color);
void solar_os_gfx_pixel(solar_os_gfx_t *gfx, int x, int y);
void solar_os_gfx_line(solar_os_gfx_t *gfx, int x0, int y0, int x1, int y1);
void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height);
void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height);
void solar_os_gfx_fill_polygon(solar_os_gfx_t *gfx,
                               const solar_os_gfx_point_t *points,
                               size_t point_count);
void solar_os_gfx_circle(solar_os_gfx_t *gfx, int x, int y, int radius);
void solar_os_gfx_fill_circle(solar_os_gfx_t *gfx, int x, int y, int radius);
void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int baseline_y, const char *text);
void solar_os_gfx_bitmap(solar_os_gfx_t *gfx,
                         int x,
                         int y,
                         int width,
                         int height,
                         const uint8_t *bitmap);
bool solar_os_gfx_needs_present(const solar_os_gfx_t *gfx);
void solar_os_gfx_present(solar_os_gfx_t *gfx);
