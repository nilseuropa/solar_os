#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "solar_os_gfx_icons.h"

#include "solar_os_gfx_types_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx);
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx);
bool solar_os_gfx_dimensions(const solar_os_gfx_t *gfx, uint16_t *width, uint16_t *height);
solar_os_display_rect_t solar_os_gfx_canvas_bounds(const solar_os_gfx_t *gfx);
solar_os_display_rect_t solar_os_gfx_panel_bounds(const solar_os_gfx_t *gfx);
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
size_t solar_os_gfx_text_advance(solar_os_gfx_t *gfx, const char *text, size_t length);
size_t solar_os_gfx_text_fit(solar_os_gfx_t *gfx, const char *text, size_t length,
                             size_t max_pixels, size_t *advance);
size_t solar_os_gfx_text_tail(solar_os_gfx_t *gfx, const char *text, size_t length,
                              size_t max_pixels, size_t *advance);
void solar_os_gfx_text_span(solar_os_gfx_t *gfx, int x, int baseline_y,
                            const char *text, size_t length);
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
/* Resolve a canonical Open Iconic name such as "tablet" or "musical-note". */
esp_err_t solar_os_gfx_icon_from_name(const char *name,
                                      solar_os_gfx_icon_t *icon);
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
void solar_os_gfx_bitmap_2bpp_scaled(solar_os_gfx_t *gfx,
    int x, int y, int width, int height,
    const uint8_t *bitmap, size_t bitmap_size,
    int source_width, int source_height,
    const solar_os_gfx_color_t palette[4]);
/* Draw an opaque grayscale or RGB raster with nearest-neighbour scaling.
 * A NULL clip uses the full display bounds. */
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx,
                                   const solar_os_gfx_raster_t *raster,
                                   int x,
                                   int y,
                                   int width,
                                   int height,
                                   const solar_os_gfx_clip_t *clip);
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
/* Canvas-relative placement, including negative coordinates inside the panel. */
esp_err_t solar_os_gfx_present_frame_at(
    solar_os_gfx_t *gfx,
    const solar_os_display_raster_t *frame,
    int x, int y);
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx,
                                      solar_os_display_format_t format);
bool solar_os_gfx_needs_present(const solar_os_gfx_t *gfx);
void solar_os_gfx_present(solar_os_gfx_t *gfx);

#ifdef __cplusplus
}
#endif
