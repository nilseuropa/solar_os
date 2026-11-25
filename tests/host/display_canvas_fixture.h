#pragma once
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/services/solar_os_gfx.c"
#include "solar_os_display_canvas_draw.h"

static unsigned allocations;
static solar_os_display_raster_t presented;
static const uint8_t *fixture_fallback_font;
const uint8_t *solar_os_font_fallback(const uint8_t *font, uint32_t codepoint)
{ (void)font; (void)codepoint; return fixture_fallback_font; }
size_t solar_os_font_pack_count(void) { return fixture_fallback_font ? 1 : 0; }
esp_err_t solar_os_display_present_frame(u8g2_t *u, const solar_os_display_raster_t *frame)
{ (void)u; presented = *frame; return ESP_OK; }
#ifndef SOLAR_TEST_CUSTOM_MEMORY
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; allocations++; return calloc(count, size); }
void solar_os_memory_free(void *p) { free(p); }
#endif
esp_err_t solar_os_display_get_colors(uint32_t *foreground, uint32_t *background)
{ *foreground=0; *background=0xffffff; return ESP_OK; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return ESP_OK; }

static u8x8_display_info_t info = {
    .tile_width=52, .tile_height=52, .pixel_width=412, .pixel_height=412,
};
static uint8_t display(u8x8_t *u, uint8_t msg, uint8_t arg, void *data)
{
    (void)arg; (void)data;
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) u8x8_d_helper_display_setup_memory(u, &info);
    return 1;
}

static bool mono_pixel(u8g2_t *u, unsigned x, unsigned y)
{ return (u->tile_buf_ptr[(y/8)*416+x] & (1U << (y&7))) != 0; }
