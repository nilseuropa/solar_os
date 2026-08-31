#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "solar_os_gfx_icons.h"

typedef uint32_t solar_os_gfx_color_t;

enum {
    SOLAR_OS_GFX_COLOR_WHITE,
    SOLAR_OS_GFX_COLOR_LIGHT,
    SOLAR_OS_GFX_COLOR_DARK,
    SOLAR_OS_GFX_COLOR_BLACK,
};

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
    SOLAR_OS_GFX_FONT_COUNT,
} solar_os_gfx_font_t;

typedef enum {
    SOLAR_OS_GFX_LINE_SOLID,
    SOLAR_OS_GFX_LINE_DOTTED,
    SOLAR_OS_GFX_LINE_DASHED,
} solar_os_gfx_line_style_t;

typedef struct solar_os_gfx solar_os_gfx_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
} solar_os_gfx_point_t;

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx);
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx);
solar_os_display_format_t solar_os_gfx_format(const solar_os_gfx_t *gfx);
bool solar_os_gfx_palette_inverted(const solar_os_gfx_t *gfx);
bool solar_os_gfx_display_target_name(const solar_os_gfx_t *gfx,
                                      char *name,
                                      size_t name_len);
solar_os_gfx_color_t solar_os_gfx_gray(uint8_t level);
solar_os_gfx_color_t solar_os_gfx_rgb(uint8_t red, uint8_t green, uint8_t blue);
bool solar_os_gfx_color_is_valid(solar_os_gfx_color_t color);
void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color);
solar_os_gfx_color_t solar_os_gfx_color(const solar_os_gfx_t *gfx);
void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font);
solar_os_gfx_font_t solar_os_gfx_font(const solar_os_gfx_t *gfx);
size_t solar_os_gfx_text_width(solar_os_gfx_t *gfx, const char *text);
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
/* Draw a transparent icon in the current color. x and y are its top-left. */
void solar_os_gfx_icon(solar_os_gfx_t *gfx,
                       int x,
                       int y,
                       solar_os_gfx_icon_t icon,
                       solar_os_gfx_icon_size_t size);
void solar_os_gfx_bitmap(solar_os_gfx_t *gfx,
                         int x,
                         int y,
                         int width,
                         int height,
                         const uint8_t *bitmap);
void solar_os_gfx_bitmap_2bpp(solar_os_gfx_t *gfx,
                              int x,
                              int y,
                              int width,
                              int height,
                              const uint8_t *bitmap,
                              size_t bitmap_size,
                              const solar_os_gfx_color_t palette[4]);
esp_err_t solar_os_gfx_present_mono_xbm(solar_os_gfx_t *gfx,
                                        const uint8_t *bitmap,
                                        size_t bitmap_size,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        size_t stride);
esp_err_t solar_os_gfx_present_frame(
    solar_os_gfx_t *gfx,
    const solar_os_display_raster_t *frame);
bool solar_os_gfx_needs_present(const solar_os_gfx_t *gfx);
void solar_os_gfx_present(solar_os_gfx_t *gfx);

#ifdef __cplusplus
}
#endif
