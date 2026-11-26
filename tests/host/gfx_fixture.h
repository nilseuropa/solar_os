#pragma once
#include "solar_os_native_gfx.h"
#include "solar_os_native_gfx_abi.h"
#include "display_canvas_fixture.h"

static unsigned surfaces, mono_frames, ordinary_frames;
static solar_os_display_rect_t last_damage;
static size_t last_damage_count;
bool solar_os_display_target_name_for_u8g2(const u8g2_t *u, char *name, size_t capacity)
{ if (!u || capacity < sizeof("display0")) return false; strcpy(name, "display0"); return true; }
static esp_err_t target_frame(void *context, const solar_os_display_raster_t *frame)
{ return solar_os_display_present_frame(context, frame); }
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{
    if (!name || strcmp(name, "display0")) return false;
    *target = (solar_os_display_target_t){.present_frame = target_frame,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT | SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT};
    return true;
}
void solar_os_display_present_regions(u8g2_t *u, solar_os_display_present_mode_t mode,
    const solar_os_display_rect_t *areas, size_t count)
{
    assert(u && mode == SOLAR_OS_DISPLAY_PRESENT_GRAPHICS);
    ++ordinary_frames;
    last_damage_count = areas ? count : 0;
    if (last_damage_count) last_damage = areas[0];
}
esp_err_t solar_os_display_present_surface(u8g2_t *u, const solar_os_display_surface_t *surface)
{ assert(u && surface && surface->data); ++surfaces; return ESP_OK; }
esp_err_t solar_os_display_present_mono_xbm(u8g2_t *u, const uint8_t *bitmap, size_t size,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t stride, bool inverted)
{ (void)x; (void)y; (void)inverted; assert(u && bitmap && size && width && height && stride); ++mono_frames; return ESP_OK; }
