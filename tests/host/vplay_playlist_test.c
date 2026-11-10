#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "solar_os.h"
#include "solar_os_keys.h"
#include "solar_os_media_playlist.h"
#include "solar_os_media_widgets.h"
#include "solar_os_storage_browser.h"

typedef struct {
    bool stopped, done, suspended, playlist_visible, layout_dirty;
    esp_err_t error;
    char path[SOLAR_OS_STORAGE_PATH_MAX], title[SOLAR_OS_M3U_TITLE_MAX];
} mpeg_player_t;
#include "vplay_playlist_state.inc"
static unsigned starts, destroys, finishes, pauses;
static bool decoder_live, save_failed, directory_selected;
static size_t saved_count;
static solar_os_m3u_entry_t saved_entries[SOLAR_OS_MEDIA_PLAYLIST_CAPACITY];
struct solar_os_storage_browser { size_t cursor; };
static struct solar_os_storage_browser browser;
typedef struct { uint8_t volume; } solar_os_audio_status_t;
static mpeg_player_t decoder;
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
int solar_os_context_argc(const solar_os_context_t *ctx) { return ctx->argc; }
const char *solar_os_context_argv(const solar_os_context_t *ctx, int i) { return ctx->argv[i]; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active) { ctx->graphics_active = active; }
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)ctx; (void)code; (void)message; finishes++; }
size_t strlcpy(char *out, const char *in, size_t size)
{ size_t n = strlen(in); if (size) snprintf(out, size, "%s", in); return n; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
static void vplay_stop_playback(mpeg_player_t *p) { if (p) p->stopped = true; }
static void vplay_reap_stopped(mpeg_player_t *p) { (void)p; }
static void mpeg_player_destroy(mpeg_player_t *p) { if (p) { assert(p->done); destroys++; decoder_live = false; } }
static esp_err_t mpeg_player_start(solar_os_context_t *ctx, const char *path, bool fit,
    bool fullscreen, double time, bool paused, mpeg_player_t **out)
{
    (void)ctx; (void)fit; (void)fullscreen; (void)time; (void)paused;
    assert(!decoder_live); decoder_live = true; starts++;
    memset(&decoder, 0, sizeof(decoder)); snprintf(decoder.path, sizeof(decoder.path), "%s", path);
    *out = &decoder; return ESP_OK;
}
static void mpeg_player_event(solar_os_context_t *ctx, mpeg_player_t *p, const solar_os_event_t *event)
{ (void)ctx; (void)p; if (event->type == SOLAR_OS_EVENT_CHAR && event->data.ch == ' ') pauses++; }
static void mpeg_player_fullscreen(mpeg_player_t *p, bool enabled) { (void)p; (void)enabled; }
static void mpeg_player_fit(mpeg_player_t *p, bool enabled) { (void)p; (void)enabled; }
static bool vplay_pointer(const solar_os_input_pointer_event_t *p) { (void)p; return false; }
static void vplay_seek(int direction) { (void)direction; }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { (void)gfx; return 320; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { (void)gfx; return 240; }
void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color) { (void)gfx; (void)color; }
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; }
void solar_os_audio_get_status(solar_os_audio_status_t *status) { status->volume = 50; }
void solar_os_media_player_controls_draw(solar_os_gfx_t *gfx, int w, int h, const char *title,
    const char *status, uint8_t volume, bool seek, solar_os_media_transport_icon_t icon)
{ (void)gfx; (void)w; (void)h; (void)title; (void)status; (void)volume; (void)seek; (void)icon; }
void solar_os_media_player_header_draw(solar_os_gfx_t *gfx, int width, const char *title, const char *tabs)
{ (void)gfx; (void)width; (void)title; (void)tabs; }
void solar_os_media_playlist_draw(solar_os_gfx_t *gfx, int w, int h, size_t count,
    size_t cursor, size_t active, size_t *top, solar_os_m3u_get_fn get, void *user, const char *help)
{
    (void)gfx; (void)w; (void)h; (void)active; (void)top; (void)help;
    solar_os_m3u_entry_t entry;
    if (count) assert(cursor < count && get(cursor, &entry, user));
}
bool solar_os_m3u_path(const char *path) { return path && strstr(path, ".m3u") != NULL; }
esp_err_t solar_os_media_playlist_open(solar_os_media_playlist_t *list, solar_os_media_playlist_kind_t kind,
    const char *argument, size_t *selected)
{
    assert(!argument && kind == SOLAR_OS_MEDIA_PLAYLIST_VIDEO);
    *list = (solar_os_media_playlist_t){.entries = saved_entries, .count = saved_count, .kind = kind};
    *selected = 0; return ESP_OK;
}
esp_err_t solar_os_media_playlist_add(solar_os_media_playlist_t *list, const char *path, size_t *index)
{
    if (save_failed) return ESP_FAIL;
    assert(list->count < SOLAR_OS_MEDIA_PLAYLIST_CAPACITY);
    *index = list->count++;
    snprintf(list->entries[*index].path, sizeof(list->entries[*index].path), "%s", path);
    saved_count = list->count; return ESP_OK;
}
esp_err_t solar_os_media_playlist_import(solar_os_media_playlist_t *list, const char *path)
{ (void)list; (void)path; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_media_playlist_remove(solar_os_media_playlist_t *list, size_t index)
{
    if (save_failed) return ESP_FAIL;
    assert(index < list->count);
    memmove(list->entries + index, list->entries + index + 1, (--list->count - index) * sizeof(*list->entries));
    saved_count = list->count; return ESP_OK;
}
const char *solar_os_storage_mount_point(void) { return "/"; }
esp_err_t solar_os_storage_browser_create(solar_os_storage_browser_filter_t filter, void *user,
    solar_os_storage_browser_t **out)
{ assert(filter("movie.mpg", user) && !filter("audio.wav", user)); *out = &browser; return ESP_OK; }
esp_err_t solar_os_storage_browser_open(solar_os_storage_browser_t *b, const char *path)
{ (void)b; (void)path; return ESP_OK; }
size_t solar_os_storage_browser_count(const solar_os_storage_browser_t *b) { (void)b; return 1; }
size_t solar_os_storage_browser_cursor(const solar_os_storage_browser_t *b) { return b->cursor; }
void solar_os_storage_browser_move(solar_os_storage_browser_t *b, int direction) { (void)b; (void)direction; }
bool solar_os_storage_browser_media_entry(size_t i, solar_os_m3u_entry_t *entry, void *b)
{ (void)b; memset(entry, 0, sizeof(*entry)); snprintf(entry->path, sizeof(entry->path), "movie.mpg"); return i == 0; }
esp_err_t solar_os_storage_browser_activate(solar_os_storage_browser_t *b, char *path, size_t size, bool *selected)
{ (void)b; *selected = !directory_selected; directory_selected = false; snprintf(path, size, "/movie.mpg"); return ESP_OK; }
#include "vplay_playlist_code.inc"

static void empty_launch_and_add_test(void)
{
    vplay_state_t state = {0}; vplay_state_storage = &state;
    solar_os_context_t ctx = {.argc = 1, .gfx = (solar_os_gfx_t *)1};
    starts = destroys = finishes = 0; decoder_live = false; saved_count = 0;
    assert(vplay_start(&ctx) == ESP_OK);
    assert(state.playlist_visible && !state.player && !starts && !finishes && ctx.graphics_active);
    solar_os_event_t key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = '\r'};
    assert(vplay_event(&ctx, &key) && !state.restart && !finishes);
    key.data.ch = 'a'; assert(vplay_event(&ctx, &key) && state.browsing);
    key.data.ch = SOLAR_OS_KEY_ESCAPE; assert(vplay_event(&ctx, &key) && !state.browsing && !finishes);
    key.data.ch = 'a'; assert(vplay_event(&ctx, &key) && state.browsing);
    directory_selected = true;
    key.data.ch = '\r'; assert(vplay_event(&ctx, &key) && state.browsing && !saved_count);
    save_failed = true; assert(vplay_event(&ctx, &key) && state.browsing && !saved_count && state.message[0]);
    save_failed = false; assert(vplay_event(&ctx, &key) && !state.browsing && saved_count == 1 && !starts);
    assert(vplay_event(&ctx, &key) && state.restart && !starts);
    solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK};
    assert(vplay_event(&ctx, &tick) && starts == 1 && !state.playlist_visible);
    key.data.ch = '\t'; assert(vplay_event(&ctx, &key));
    key.data.ch = SOLAR_OS_KEY_DELETE; save_failed = true;
    assert(vplay_event(&ctx, &key) && saved_count == 1 && state.active == 0);
    save_failed = false; assert(vplay_event(&ctx, &key) && !saved_count && state.active == SIZE_MAX);
    assert(state.playlist_visible && !finishes);
    saved_count = 1; decoder_live = false;
    assert(vplay_start(&ctx) == ESP_OK && state.playlist_visible && !state.player && starts == 1);
}

int main(void)
{
    vplay_state_t state = {0}; vplay_state_storage = &state;
    solar_os_m3u_entry_t entries[3] = {
        {.path = "/one.mpg", .title = "One"}, {.path = "/two.mpg", .title = "Two"},
        {.path = "/three.mpg", .title = "Three"},
    };
    state.player = &decoder; decoder_live = true; state.playlist.entries = entries; state.playlist.count = 3;
    solar_os_context_t ctx = {0};
    solar_os_event_t key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = SOLAR_OS_KEY_RIGHT};
    solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK};
    assert(vplay_event(&ctx, &key) && state.active == 1 && state.restart);
    assert(!starts && !destroys);
    assert(vplay_event(&ctx, &tick) && !starts); /* Old decoder still owns resources. */
    decoder.done = true; assert(vplay_event(&ctx, &tick));
    assert(starts == 1 && destroys == 1 && !strcmp(decoder.path, "/two.mpg"));
    assert(!strcmp(decoder.title, "Two"));
    key.data.ch = '\t'; assert(vplay_event(&ctx, &key) && state.playlist_visible);
    key.data.ch = ' '; assert(vplay_event(&ctx, &key) && pauses == 1);
    key.data.ch = SOLAR_OS_KEY_DOWN; assert(vplay_event(&ctx, &key) && state.cursor == 2);
    key.data.ch = '\r'; assert(vplay_event(&ctx, &key) && state.active == 2 && !state.playlist_visible);
    decoder.done = true; assert(vplay_event(&ctx, &tick));
    assert(starts == 2 && !strcmp(decoder.path, "/three.mpg"));
    decoder.done = true; assert(vplay_event(&ctx, &tick));
    assert(decoder.stopped && !state.restart && !finishes); /* Final EOF stays in player. */
    state.active = state.cursor = 0; decoder.stopped = false;
    assert(vplay_event(&ctx, &tick) && state.active == 1 && starts == 3);
    empty_launch_and_add_test();
    puts("VPlay playlist selection, pause and EOF ownership: passed");
}
