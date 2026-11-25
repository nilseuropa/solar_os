#include "solar_os_native_gfx.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_gfx.h"
#include "solar_os_native_gfx_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
static int gfx_canvas_bounds(const solar_os_gfx_t *gfx, solar_os_display_rect_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_display_rect_t){0};
    if (!gfx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    *out = solar_os_gfx_canvas_bounds(gfx);
    return SOLAR_OS_NATIVE_OK;
}
static int gfx_panel_bounds(const solar_os_gfx_t *gfx, solar_os_display_rect_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_display_rect_t){0};
    if (!gfx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    *out = solar_os_gfx_panel_bounds(gfx);
    return SOLAR_OS_NATIVE_OK;
}
static int gfx_icon_from_name(const char *name, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_gfx_icon_t icon;
    const int result = solar_os_native_result(solar_os_gfx_icon_from_name(name, &icon));
    if (!result) *out = icon;
    return result;
}
static int gfx_icon(solar_os_gfx_t *gfx, int x, int y, uint32_t icon, uint32_t size)
{
    if (!gfx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (icon >= SOLAR_OS_GFX_ICON_COUNT || (size != 8 && size != 16 && size != 32 && size != 48 && size != 64))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_gfx_icon(gfx, x, y, (solar_os_gfx_icon_t)icon, (solar_os_gfx_icon_size_t)size);
    return SOLAR_OS_NATIVE_OK;
}
static int gfx_blit_raster(solar_os_gfx_t *gfx, const solar_os_gfx_raster_t *raster,
    int x, int y, int width, int height, const solar_os_gfx_clip_t *clip)
{ return solar_os_native_result(solar_os_gfx_blit_raster(gfx, raster, x, y, width, height, clip)); }
static int gfx_present_mono_xbm(solar_os_gfx_t *gfx, const uint8_t *bitmap, size_t size,
    int x, int y, int width, int height, size_t stride)
{ return solar_os_native_result(solar_os_gfx_present_mono_xbm(gfx, bitmap, size, x, y, width, height, stride)); }
static int gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame)
{ return solar_os_native_result(solar_os_gfx_present_frame(gfx, frame)); }
static int gfx_present_frame_at(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame, int x, int y)
{ return solar_os_native_result(solar_os_gfx_present_frame_at(gfx, frame, x, y)); }

static const solar_os_native_gfx_api_v1_t gfx_api = {
    .abi_version = SOLAR_OS_NATIVE_GFX_ABI, .struct_size = sizeof(gfx_api),
    .width = solar_os_gfx_width, .height = solar_os_gfx_height, .dimensions = solar_os_gfx_dimensions,
    .canvas_bounds = gfx_canvas_bounds, .panel_bounds = gfx_panel_bounds,
    .format = solar_os_gfx_format, .palette_inverted = solar_os_gfx_palette_inverted,
    .display_target_name = solar_os_gfx_display_target_name,
    .gray = solar_os_gfx_gray, .rgb = solar_os_gfx_rgb, .color_is_valid = solar_os_gfx_color_is_valid,
    .set_color = solar_os_gfx_set_color, .color = solar_os_gfx_color,
    .set_font = solar_os_gfx_set_font, .font = solar_os_gfx_font, .text_width = solar_os_gfx_text_width,
    .set_line_style = solar_os_gfx_set_line_style, .line_style = solar_os_gfx_line_style,
    .clear = solar_os_gfx_clear, .pixel = solar_os_gfx_pixel, .line = solar_os_gfx_line,
    .rect = solar_os_gfx_rect, .fill_rect = solar_os_gfx_fill_rect, .fill_polygon = solar_os_gfx_fill_polygon,
    .circle = solar_os_gfx_circle, .fill_circle = solar_os_gfx_fill_circle, .text = solar_os_gfx_text,
    .icon_from_name = gfx_icon_from_name, .icon = gfx_icon,
    .bitmap = solar_os_gfx_bitmap, .bitmap_2bpp = solar_os_gfx_bitmap_2bpp,
    .bitmap_2bpp_scaled = solar_os_gfx_bitmap_2bpp_scaled,
    .blit_raster = gfx_blit_raster, .present_mono_xbm = gfx_present_mono_xbm,
    .present_frame = gfx_present_frame, .present_frame_at = gfx_present_frame_at,
    .supports_frame_format = solar_os_gfx_supports_frame_format,
    .needs_present = solar_os_gfx_needs_present, .present = solar_os_gfx_present,
    .text_advance = solar_os_gfx_text_advance, .text_fit = solar_os_gfx_text_fit,
    .text_span = solar_os_gfx_text_span,
    .text_tail = solar_os_gfx_text_tail,
};
#endif

const void *solar_os_native_gfx_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_gfx_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
