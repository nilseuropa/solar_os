#pragma once
/* Adapter for the geometry fixtures' deliberately simple monospace fonts.
 * Pixel/font-pack behavior is exercised against U8g2 in native_gfx_test.c. */
#include "solar_os_native_client_ui_utf8.h"
#include "solar_os_gfx_widgets.h"
#include "solar_os_native_client.h"
#include "solar_os_native_client_ui.h"
#include "solar_os_media_widgets.h"

/* These geometry fixtures link the production common chrome. Only the
 * cassette drawing probe is optional; pixel/model behavior uses U8g2 tests. */
static const solar_os_native_media_widgets_api_v1_t fixture_media_api = {
    .abi_version = SOLAR_OS_NATIVE_MEDIA_WIDGETS_ABI, .struct_size = sizeof(fixture_media_api),
    .transport_button_draw = solar_os_media_transport_button_draw,
    .list_layout = solar_os_media_list_layout,
#ifndef SOLAR_TEST_MEDIA_NO_PLAYLIST
    .playlist_draw = solar_os_media_playlist_draw,
#endif
    .player_layout = solar_os_media_player_layout, .player_header_draw = solar_os_media_player_header_draw,
    .player_status_draw = solar_os_media_player_status_draw, .player_controls_draw = solar_os_media_player_controls_draw,
    .controls_draw = solar_os_media_controls_draw, .player_button_at = solar_os_media_player_button_at,
#ifdef SOLAR_TEST_MEDIA_CASSETTE_DRAW
    .cassette_draw = SOLAR_TEST_MEDIA_CASSETTE_DRAW,
#endif
};
static unsigned fixture_ui_queries, fixture_media_queries;
static bool fixture_media_unavailable;

static const solar_os_native_ui_api_v1_t fixture_ui_api = {
    .abi_version = SOLAR_OS_NATIVE_UI_ABI, .struct_size = sizeof(fixture_ui_api),
    .label = solar_os_gfx_label, .centered_label = solar_os_gfx_centered_label,
    .input = solar_os_gfx_input, .dialog = solar_os_gfx_dialog,
    .input_key = solar_os_ui_input_key, .input_text = solar_os_ui_input_text,
    .viewport_reconcile = solar_os_ui_viewport_reconcile, .viewport_key = solar_os_ui_viewport_key,
};
const void *solar_os_native_ui_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    if (!name || strcmp(name, SOLAR_OS_NATIVE_UI_SERVICE) || abi != SOLAR_OS_NATIVE_UI_ABI)
        return NULL;
    ++fixture_ui_queries;
    return minimum <= sizeof(fixture_ui_api) ? &fixture_ui_api : NULL;
}

const void *solar_os_native_media_widgets_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    if (!name || strcmp(name, SOLAR_OS_NATIVE_MEDIA_WIDGETS_SERVICE) || abi != SOLAR_OS_NATIVE_MEDIA_WIDGETS_ABI)
        return NULL;
    ++fixture_media_queries;
    return !fixture_media_unavailable && minimum <= sizeof(fixture_media_api) ? &fixture_media_api : NULL;
}

size_t solar_os_gfx_text_advance(solar_os_gfx_t *gfx, const char *text, size_t length)
{
    char copy[2048]; assert(length < sizeof(copy));
    memcpy(copy, text, length); copy[length] = 0;
    return solar_os_gfx_text_width(gfx, copy);
}
size_t solar_os_gfx_text_fit(solar_os_gfx_t *gfx, const char *text, size_t length,
    size_t width, size_t *advance)
{
    size_t offset = 0, used = 0;
    while (offset < length) {
        uint32_t codepoint;
        const size_t bytes = solar_os_ui_utf8_decode_n(text + offset, length - offset, &codepoint);
        if (!bytes) break;
        const size_t next = solar_os_gfx_text_advance(gfx, text, offset + bytes);
        if (next > width) break;
        used = next; offset += bytes;
    }
    if (advance) *advance = used;
    return offset;
}
size_t solar_os_gfx_text_tail(solar_os_gfx_t *gfx, const char *text, size_t length,
    size_t width, size_t *advance)
{
    size_t offset = 0, used = solar_os_gfx_text_advance(gfx, text, length);
    while (offset < length && used > width) {
        uint32_t codepoint;
        offset += solar_os_ui_utf8_decode_n(text + offset, length - offset, &codepoint);
        used = solar_os_gfx_text_advance(gfx, text + offset, length - offset);
    }
    if (advance) *advance = used;
    return offset;
}
void solar_os_gfx_text_span(solar_os_gfx_t *gfx, int x, int baseline,
    const char *text, size_t length)
{
    char copy[2048]; assert(length < sizeof(copy));
    memcpy(copy, text, length); copy[length] = 0;
    solar_os_gfx_text(gfx, x, baseline, copy);
}
