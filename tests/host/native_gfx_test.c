#include "gfx_fixture.h"
#include "native_ui_test.inc"
#include "native_media_widgets_test.inc"
extern const uint8_t test_pack_face[];

static void text_spans(const solar_os_native_gfx_api_v1_t *api, solar_os_gfx_t *gfx, u8g2_t *u)
{
    const unsigned before = allocations;
    const char bounded[] = {'A', 'B', 'I'}; /* Deliberately no terminating NUL. */
    const size_t step = api->text_advance(gfx, bounded, 1);
    assert(step && api->text_advance(gfx, bounded, 3) == 3 * step);
    size_t advance = SIZE_MAX;
    assert(api->text_fit(gfx, bounded, 3, 2 * step, &advance) == 2 && advance == 2 * step);
    assert(api->text_fit(gfx, bounded, 3, 0, &advance) == 0 && !advance);
    assert(api->text_tail(gfx, bounded, 3, 2 * step, &advance) == 1 && advance == 2 * step);
    assert(api->text_tail(gfx, bounded, 3, step - 1, &advance) == 3 && !advance);
    assert(api->text_tail(gfx, bounded, 3, SIZE_MAX, NULL) == 0);
    assert(!api->text_tail(NULL, bounded, 3, 2 * step, &advance) && !advance);
    assert(api->text_fit(NULL, bounded, 3, 100, &advance) == 0 && !advance);
    assert(!api->text_advance(gfx, NULL, 3));
    assert(!api->text_fit(gfx, bounded, 0, SIZE_MAX, NULL));
    assert(api->text_advance(gfx, "A\0B", 3) == step);
    /* Truncated and invalid input must not inspect beyond its byte span. */
    const char truncated[] = {'\xe2', '\x82'};
    assert(api->text_advance(gfx, truncated, 2) == api->text_advance(gfx, "??", 2));
    assert(api->text_advance(gfx, truncated, 1) == api->text_advance(gfx, "?", 1));
    const char utf8[] = {'A', '\xc3', '\xb6', 'B'};
    const size_t two = api->text_advance(gfx, utf8, 3);
    assert(api->text_fit(gfx, utf8, 4, two, &advance) == 3 && advance == two);
    assert(api->text_tail(gfx, utf8, 3, step, &advance) == 1 && advance == step);
    uint8_t expected[52 * 52 * 8];
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text(gfx, 10, 30, "ABI");
    memcpy(expected, u->tile_buf_ptr, sizeof(expected));
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text_span(gfx, 10, 30, bounded, 3);
    assert(!memcmp(expected, u->tile_buf_ptr, sizeof(expected)));
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text_span(gfx, 10, 30, bounded, 2);
    memcpy(expected, u->tile_buf_ptr, sizeof(expected));
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text(gfx, 10, 30, "AB");
    assert(!memcmp(expected, u->tile_buf_ptr, sizeof(expected)));
    /* More than a 16-bit display's worth of text cannot wrap onto the panel. */
    char long_text[12000]; memset(long_text, 'A', sizeof(long_text));
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text_span(gfx, 10, 30, long_text, sizeof(long_text));
    memcpy(expected, u->tile_buf_ptr, sizeof(expected));
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text_span(gfx, 10, 30, long_text, 1000);
    assert(!memcmp(expected, u->tile_buf_ptr, sizeof(expected)));
    /* Exercise a real loaded U8g2 face, not a made-up glyph metric. */
    fixture_fallback_font = test_pack_face;
    u8g2_SetFont(u, test_pack_face);
    assert(u8g2_IsGlyph(u, 0x416)); /* Cyrillic Zhe. */
    const size_t zhe = (size_t)u8g2_GetGlyphWidth(u, 0x416);
    const char mixed[] = {'A', '\xd0', '\x96', 'B'};
    assert(api->text_advance(gfx, mixed, 4) == 2 * step + zhe);
    assert(api->text_fit(gfx, mixed, 4, step + zhe, &advance) == 3 && advance == step + zhe);
    assert(api->text_tail(gfx, mixed, 4, step + zhe, &advance) == 1 && advance == step + zhe);
    const uint8_t *base = u->font;
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text(gfx, 10, 30, "A\xd0\x96" "B");
    memcpy(expected, u->tile_buf_ptr, sizeof(expected));
    api->clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text_span(gfx, 10, 30, mixed, 4);
    assert(!memcmp(expected, u->tile_buf_ptr, sizeof(expected)) && u->font == base);
    fixture_fallback_font = NULL;
    assert(allocations == before);
}

int main(void)
{
    const solar_os_native_gfx_api_v1_t *api = solar_os_native_gfx_get_service("gfx", 1, 8);
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_gfx_get_service("gfx", 2, 8));
    assert(!solar_os_native_gfx_get_service("gfx", 1, sizeof(*api) + 1));
    assert(!solar_os_native_gfx_get_service("missing", 1, 8));
    /* Keep pixel loops in the existing core kernels. */
    assert(api->line == solar_os_gfx_line && api->fill_rect == solar_os_gfx_fill_rect);
    assert(api->bitmap_2bpp_scaled == solar_os_gfx_bitmap_2bpp_scaled);
    uint8_t buffer[52 * 52 * 8];
    u8g2_t u; memset(&u, 0, sizeof(u));
    u8g2_SetupDisplay(&u, display, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u, buffer, 52, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    solar_os_display_geometry_t geometry;
    solar_os_display_geometry_init(&geometry, 412, 412, true);
    u8g2_SetUserPtr(&u, &geometry);
    solar_os_gfx_t gfx; solar_os_gfx_init(&gfx, &u);
    solar_os_gfx_set_black_is_one(&gfx, true);
    solar_os_display_rect_t bounds;
    assert(!api->canvas_bounds(&gfx, &bounds) && !bounds.x && !bounds.y && bounds.width == 291 && bounds.height == 291);
    assert(!api->panel_bounds(&gfx, &bounds) && bounds.x == -60 && bounds.y == -60 && bounds.width == 412);
    assert(api->panel_bounds(NULL, &bounds) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !bounds.width);
    assert(api->canvas_bounds(&gfx, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint16_t width, height; assert(api->dimensions(&gfx, &width, &height) && width == 291 && height == 291);
    assert(api->width(&gfx) == 291 && api->height(&gfx) == 291);
    char target[16]; assert(api->display_target_name(&gfx, target, sizeof(target)) && !strcmp(target, "display0"));
    assert(api->supports_frame_format(&gfx, SOLAR_OS_DISPLAY_FORMAT_RGB565));
    assert(!api->supports_frame_format(&gfx, SOLAR_OS_DISPLAY_FORMAT_INDEX8));
    const unsigned before = allocations;
    api->clear(&gfx, SOLAR_OS_GFX_COLOR_WHITE); api->set_color(&gfx, SOLAR_OS_GFX_COLOR_BLACK);
    api->pixel(&gfx, 0, 0); api->fill_rect(&gfx, -60, 146, 2, 2);
    assert(mono_pixel(&u, 60, 60) && mono_pixel(&u, 0, 206) && mono_pixel(&u, 1, 207));
    assert(!mono_pixel(&u, 2, 206));
    api->set_line_style(&gfx, SOLAR_OS_GFX_LINE_DASHED);
    assert(api->line_style(&gfx) == SOLAR_OS_GFX_LINE_DASHED);
    api->set_line_style(&gfx, SOLAR_OS_GFX_LINE_SOLID);
    api->set_font(&gfx, SOLAR_OS_GFX_FONT_MONO_14);
    assert(api->font(&gfx) == SOLAR_OS_GFX_FONT_MONO_14 && api->text_width(&gfx, "ABI") > 0);
    api->text(&gfx, 10, 30, "ABI");
    text_spans(api, &gfx, &u);
    ui_widgets(&gfx, &u);
    media_widgets(&gfx, &u);
    uint32_t icon = UINT32_MAX;
    assert(!api->icon_from_name("tablet", &icon) && !api->icon(&gfx, 20, 40, icon, 16));
    assert(api->icon_from_name("invalid-icon", &icon) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !icon);
    assert(api->icon(&gfx, 0, 0, UINT32_MAX, 16) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->icon(&gfx, 0, 0, 0, 7) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->color_is_valid(api->rgb(12, 34, 56)) && api->color_is_valid(api->gray(4)));
    assert(api->color(&gfx) == SOLAR_OS_GFX_COLOR_BLACK);
    assert(allocations == before && api->needs_present(&gfx));
    api->present(&gfx); assert(ordinary_frames == 1 && !api->needs_present(&gfx));
    const uint8_t bitmap[] = {1};
    assert(!api->present_mono_xbm(&gfx, bitmap, 1, 0, 0, 1, 1, 1) && mono_frames == 1);
    const uint8_t pixels[] = {0x12, 0x34};
    const solar_os_display_raster_t frame = {.data = pixels, .data_size = 2, .source_width = 1,
        .source_height = 1, .source_stride = 2, .width = 1, .height = 1, .format = SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(!api->present_frame_at(&gfx, &frame, -60, -60));
    assert(presented.data == pixels && !presented.x && !presented.y);
    assert(api->present_frame_at(&gfx, &frame, -61, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    const uint8_t gray[] = {0};
    const solar_os_gfx_raster_t raster = {.pixels = gray, .pixels_size = 1, .width = 1, .height = 1,
        .stride = 1, .format = SOLAR_OS_GFX_RASTER_GRAY8};
    assert(!api->blit_raster(&gfx, &raster, -60, 148, 1, 1, NULL) && mono_pixel(&u, 0, 208));
    assert(api->blit_raster(&gfx, NULL, 0, 0, 1, 1, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!solar_os_gfx_enable_index8(&gfx));
    const unsigned indexed_before = allocations;
    api->clear(&gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->set_color(&gfx, api->rgb(255, 0, 0));
    api->text(&gfx, 10, 30, "ABI");
    uint8_t indexed_expected[412 * 412];
    memcpy(indexed_expected, gfx.index8->pixels, sizeof(indexed_expected));
    api->clear(&gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->text_span(&gfx, 10, 30, "ABI ignored", 3);
    assert(!memcmp(indexed_expected, gfx.index8->pixels, sizeof(indexed_expected)));
    assert(allocations == indexed_before);
    api->clear(&gfx, api->rgb(255, 255, 255)); api->pixel(&gfx, 1, 1);
    assert(api->format(&gfx) == SOLAR_OS_DISPLAY_FORMAT_INDEX8);
    api->present(&gfx); assert(surfaces == 1 && !api->needs_present(&gfx));
    solar_os_gfx_release_surface(&gfx);
    return 0;
}
