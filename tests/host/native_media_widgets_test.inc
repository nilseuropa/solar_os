#include "solar_os_native_media_widgets.h"
#include "solar_os_native_media_widgets_abi.h"
#include "solar_os_media_widgets.h"

static size_t media_rows, media_first, media_last;
static bool media_row(size_t index, solar_os_m3u_entry_t *entry, void *user)
{
    assert(user == &media_rows);
    if (!media_rows++) media_first = index;
    media_last = index;
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->title, sizeof(entry->title), "Row %zu", index);
    return true;
}

static void media_widgets(solar_os_gfx_t *gfx, u8g2_t *u)
{
    const solar_os_native_media_widgets_api_v1_t *api = solar_os_native_media_widgets_get_service("ui.media", 1, 8);
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_media_widgets_get_service("ui.media", 2, 8));
    assert(!solar_os_native_media_widgets_get_service("ui.media", 1, sizeof(*api) + 1));
    assert(!solar_os_native_media_widgets_get_service("other", 1, 8));
    assert(api->player_controls_draw == solar_os_media_player_controls_draw);
    assert(api->playlist_draw == solar_os_media_playlist_draw && api->cassette_draw == solar_os_cassette_widget_draw);
    const unsigned before = allocations;
    solar_os_cassette_widget_t animation;
    memset(&animation, 0xff, sizeof(animation)); api->cassette_reset(&animation);
    assert(!animation.phase && !animation.playing && !animation.elapsed_ms && !animation.total_ms && !animation.last_tick_ms);
    api->cassette_update(&animation, true, 500, 1000, 100);
    assert(animation.phase == 1 && animation.playing && animation.elapsed_ms == 500 && animation.total_ms == 1000);
    const solar_os_cassette_widget_t copy = animation;
    uint8_t frame[52 * 52 * 8];
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->cassette_draw(&animation, gfx, 10, 30, 240, 180);
    memcpy(frame, u->tile_buf_ptr, sizeof(frame));
    assert(!memcmp(&copy, &animation, sizeof(animation)));
    api->cassette_update(&animation, true, 500, 1000, 189); assert(animation.phase == 1);
    api->cassette_update(&animation, true, 500, 1000, 190); assert(animation.phase == 2);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    api->cassette_draw(&animation, gfx, 10, 30, 240, 180);
    assert(memcmp(frame, u->tile_buf_ptr, sizeof(frame)));
    animation.last_tick_ms = UINT32_MAX - 40;
    api->cassette_update(&animation, true, 500, 1000, 50); assert(animation.phase == 3);
    api->cassette_update(&animation, false, 0, 0, 500); assert(animation.phase == 3 && !animation.playing);

    solar_os_media_list_layout_t layout;
    api->list_layout(291, 291, &layout); assert(layout.visible > 0);
    size_t top = SIZE_MAX;
    api->playlist_draw(gfx, 291, 291, SIZE_MAX, SIZE_MAX, SIZE_MAX, &top, media_row, &media_rows, "Help");
    assert(top == SIZE_MAX - (size_t)layout.visible && media_first == top);
    assert(media_rows == (size_t)layout.visible && media_last == SIZE_MAX - 1);
    media_rows = 0;
    api->playlist_draw(gfx, 291, 291, 0, 0, SIZE_MAX, &top, NULL, NULL, "Help");
    assert(!top && !media_rows);
    api->playlist_draw(gfx, 291, 291, 10, 1, 2, NULL, media_row, &media_rows, "Help");
    api->playlist_draw(gfx, 291, 291, 10, 1, 2, &top, NULL, NULL, "Help");
    assert(!media_rows && allocations == before);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
}
