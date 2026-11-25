/* Runs the production UI and transport helpers with deterministic owners and
 * drawing callbacks. test_rtsp_app.py supplies the extracted source headers. */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_media_widgets.h"
#include "solar_os_native_client.h"
#include "solar_os_media_playlist.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_rtsp_client.h"
#include "solar_os_keys.h"
#include "media_preferences_mock.h"

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
#define portMAX_DELAY 0
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0
#define SOLAR_OS_TASK_ROLE_FOREGROUND 0
#define pdPASS 1
#define SOLAR_OS_LOGI(...) ((void)0)
#include "rtsp_app_test_state.h"

struct solar_os_gfx { int width, height; solar_os_display_rect_t panel; };
struct solar_os_rtsp_client { solar_os_rtsp_client_status_t status; };
static struct solar_os_rtsp_client client;
static unsigned cancellations, deletions, destructions, freed, buttons, scopes, frames;
static int scope_x, scope_y, scope_width, scope_height;
static solar_os_media_transport_icon_t button_icon;
static int button_x, button_y, button_width;
static bool refreshed;
static unsigned creations, finishes;
static bool allocation_failure, task_failure;
static bool client_paused;
static bool save_failed;
static size_t saved_count;
static solar_os_m3u_entry_t saved_entries[SOLAR_OS_MEDIA_PLAYLIST_CAPACITY];
static solar_os_rtsp_client_options_t last_options;
static bool track_port_dialog, port_cursor_visible;
static char port_dialog_output[1024];

solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
int solar_os_context_argc(const solar_os_context_t *ctx) { return ctx->argc; }
const char *solar_os_context_argv(const solar_os_context_t *ctx, int i) { return ctx->argv[i]; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active) { ctx->graphics_active = active; }
void solar_os_context_set_app_class(solar_os_context_t *ctx, solar_os_app_class_t cls) { ctx->app_class = cls; }
size_t strlcpy(char *out, const char *in, size_t size)
{ size_t len = strlen(in); if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(out, in, n); out[n] = 0; } return len; }
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)ctx; (void)code; (void)message; finishes++; }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { return gfx->width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { return gfx->height; }
solar_os_display_rect_t solar_os_gfx_canvas_bounds(const solar_os_gfx_t *gfx)
{ return (solar_os_display_rect_t){0,0,gfx->width,gfx->height}; }
solar_os_display_rect_t solar_os_gfx_panel_bounds(const solar_os_gfx_t *gfx)
{ return gfx->panel.width ? gfx->panel : solar_os_gfx_canvas_bounds(gfx); }
solar_os_display_format_t solar_os_gfx_format(const solar_os_gfx_t *gfx)
{ (void)gfx; return SOLAR_OS_DISPLAY_FORMAT_MONO1; }
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx, solar_os_display_format_t format)
{ (void)gfx; (void)format; return false; }
void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color) { (void)gfx; (void)color; }
void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font) { (void)gfx; (void)font; }
size_t solar_os_gfx_text_width(solar_os_gfx_t *gfx, const char *text) { (void)gfx; return strlen(text) * 7; }
void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int baseline, const char *text)
{ (void)text; solar_os_display_rect_t b=solar_os_gfx_panel_bounds(gfx);
  assert(x>=b.x && x<b.x+b.width && baseline>=b.y && baseline<b.y+b.height); }
void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color) { (void)gfx; (void)color; }
void solar_os_gfx_line(solar_os_gfx_t *gfx, int x, int y, int x1, int y1)
{ assert(x >= 0 && x1 < gfx->width && y >= 0 && y1 < gfx->height); }
void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{ solar_os_display_rect_t b=solar_os_gfx_panel_bounds(gfx);
  assert(x>=b.x && y>=b.y && width>0 && height>0 && x+width<=b.x+b.width && y+height<=b.y+b.height); }
void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{ solar_os_gfx_rect(gfx, x, y, width, height); }
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; }
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx, const solar_os_gfx_raster_t *raster,
    int x, int y, int width, int height, const solar_os_gfx_clip_t *clip)
{ (void)raster; (void)clip; solar_os_gfx_rect(gfx, x, y, width, height); return ESP_OK; }
static int frame_x, frame_y, frame_width, frame_height;
esp_err_t solar_os_gfx_present_frame_at(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame, int x, int y)
{ frames++; frame_x=x; frame_y=y; frame_width=frame->width; frame_height=frame->height;
  solar_os_gfx_rect(gfx, x, y, frame->width, frame->height); return ESP_OK; }
void solar_os_audio_get_status(solar_os_audio_status_t *status) { status->volume = 50; }
esp_err_t solar_os_audio_set_volume(uint8_t volume) { (void)volume; assert(!"player must not change global volume"); return ESP_FAIL; }
void solar_os_media_transport_button_draw(solar_os_gfx_t *gfx, int x, int y, int width,
    int height, solar_os_media_transport_icon_t icon, bool active)
{
    (void)active; solar_os_gfx_rect(gfx, x, y, width, height);
    buttons++; button_x = x; button_y = y; button_width = width;
    if (buttons % 3 == 2) button_icon = icon;
}
void solar_os_oscilloscope_widget_draw(solar_os_oscilloscope_widget_t *widget,
    solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    (void)widget; solar_os_gfx_rect(gfx, x, y, width, height);
    scopes++; scope_x = x; scope_y = y; scope_width = width; scope_height = height;
}
esp_err_t solar_os_oscilloscope_widget_create(size_t capacity, solar_os_oscilloscope_widget_t **out)
{ (void)capacity; *out = (solar_os_oscilloscope_widget_t *)1; return ESP_OK; }
void solar_os_rtsp_client_status(solar_os_rtsp_client_t *c, solar_os_rtsp_client_status_t *status)
{ assert(c); *status = c->status; }
void solar_os_rtsp_client_cancel(solar_os_rtsp_client_t *c) { assert(c); cancellations++; }
esp_err_t solar_os_rtsp_client_destroy(solar_os_rtsp_client_t *c)
{ assert(c && rtsp.network_done && rtsp.decode_done); destructions++; return ESP_OK; }
void solar_os_rtsp_client_set_paused(solar_os_rtsp_client_t *c, bool paused)
{ assert(c); client_paused = paused; }
int64_t solar_os_rtsp_client_video_lateness(solar_os_rtsp_client_t *c, uint32_t timestamp, uint64_t arrived)
{ (void)c; (void)timestamp; (void)arrived; return 0; }
void solar_os_memory_free(void *p) { if (p) { freed++; free(p); } }
void solar_os_task_delete_external(TaskHandle_t task) { assert(task); deletions++; }
static void refresh_override(solar_os_context_t *ctx, bool enabled) { (void)ctx; refreshed = enabled; }
static void xSemaphoreTake(SemaphoreHandle_t mutex, unsigned delay) { (void)mutex; (void)delay; }
static void xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static uint64_t esp_timer_get_time(void) { return 1000000; }
static void network_worker(void *arg) { (void)arg; }
static void decode_worker(void *arg) { (void)arg; }
static void rtsp_samples(const int16_t *samples, size_t count, uint8_t channels, void *user)
{ (void)samples; (void)count; (void)channels; (void)user; }
esp_err_t solar_os_rtsp_client_create(const char *url, const solar_os_rtsp_client_options_t *options,
    solar_os_rtsp_client_t **result)
{
    assert(url[0] && !rtsp.client && !rtsp.network_task && !rtsp.decode_task);
    creations++; last_options = *options;
    if (allocation_failure) return ESP_ERR_NO_MEM;
    memset(&client, 0, sizeof(client)); *result = &client; return ESP_OK;
}
static int solar_os_task_create_pinned_external(void (*worker)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *task, int affinity, int role)
{
    (void)worker; (void)name; (void)stack; (void)arg; (void)priority; (void)affinity; (void)role;
    if (task_failure) return 0;
    *task = (void *)1; return pdPASS;
}
static void resume(solar_os_context_t *ctx) { (void)ctx; }
static void diagnostics_tick(const solar_os_rtsp_client_status_t *status) { (void)status; }
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx) { return ctx->shell_io; }
static uint16_t port_cols = 80;
uint16_t solar_os_shell_io_cols(const solar_os_shell_io_t *io) { (void)io; return port_cols; }
static void solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *format, ...)
{
    (void)io;
    if (track_port_dialog) {
        va_list args; va_start(args, format);
        vsnprintf(port_dialog_output, sizeof(port_dialog_output), format, args);
        va_end(args);
    }
}
esp_err_t solar_os_shell_io_set_cursor_visible(solar_os_shell_io_t *io, bool visible)
{ (void)io; port_cursor_visible = visible; return ESP_OK; }
#define SOLAR_OS_SHELL_IO_KIND_PORT 1
static int solar_os_shell_io_kind(solar_os_shell_io_t *io) { (void)io; return SOLAR_OS_SHELL_IO_KIND_PORT; }
bool solar_os_m3u_path(const char *path) { return path && strstr(path, ".m3u") != NULL; }
esp_err_t solar_os_media_playlist_open(solar_os_media_playlist_t *list, solar_os_media_playlist_kind_t kind,
    const char *argument, size_t *selected)
{
    assert(!argument && kind == SOLAR_OS_MEDIA_PLAYLIST_RTSP);
    *list = (solar_os_media_playlist_t){.entries = saved_entries, .count = saved_count, .kind = kind};
    *selected = 0; return ESP_OK;
}
esp_err_t solar_os_media_playlist_set(solar_os_media_playlist_t *list, size_t index, const solar_os_m3u_entry_t *entry)
{
    if (save_failed) return ESP_FAIL;
    assert(index <= list->count && index < SOLAR_OS_MEDIA_PLAYLIST_CAPACITY);
    list->entries[index] = *entry;
    if (index == list->count) list->count++;
    saved_count = list->count; return ESP_OK;
}
esp_err_t solar_os_media_playlist_remove(solar_os_media_playlist_t *list, size_t index)
{
    if (save_failed) return ESP_FAIL;
    assert(index < list->count);
    memmove(list->entries + index, list->entries + index + 1, (--list->count - index) * sizeof(*list->entries));
    saved_count = list->count; return ESP_OK;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }
#include "gfx_text_metrics_fixture.h"
#include "rtsp_app_test_code.h"

static void reset(void)
{
    static rtsp_app_state_t state;
    memset(&state, 0, sizeof(state)); rtsp_state = &state;
    memset(&client, 0, sizeof(client));
    rtsp.client = &client; rtsp.graphical = true;
    rtsp.ui_api = &fixture_ui_api;
    rtsp.media_ui = &fixture_media_api;
    snprintf(rtsp.url, sizeof(rtsp.url), "rtsp://192.168.1.238/media");
    client.status = (solar_os_rtsp_client_status_t){.audio = true, .playing = true,
        .audio_playing = true, .sample_rate = 16000, .channels = 1};
    buttons = scopes = frames = cancellations = deletions = destructions = freed = 0;
    creations = finishes = 0; allocation_failure = task_failure = false;
}

static void layout_test(int w, int h)
{
    solar_os_gfx_t gfx = {.width = w, .height = h};
    solar_os_context_t ctx = {.gfx = &gfx};
    reset(); render(&ctx, true);
    assert(buttons == 3 && scopes == 1);
    assert(scope_x == 3 && scope_y == RTSP_HEADER_HEIGHT + 3);
    assert(scope_width == w - 6 && scope_height == h - RTSP_HEADER_HEIGHT - RTSP_CONTROLS_HEIGHT - 6);
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(w, h, false, &layout);
    assert(button_y == layout.button_y);
    solar_os_event_t fullscreen = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = 'f'};
    assert(event(&ctx, &fullscreen) && rtsp.fullscreen && rtsp.output_height == (unsigned)h);
    buttons = scopes = 0;
    render(&ctx, true);
    assert(buttons == 0 && scopes == 1);
    assert(scope_x == 0 && scope_y == 0 && scope_width == w && scope_height == h);
    assert(event(&ctx, &fullscreen) && !rtsp.fullscreen);
    assert(rtsp.output_height == (unsigned)(h - RTSP_HEADER_HEIGHT - RTSP_CONTROLS_HEIGHT));
    buttons = scopes = 0;
    render(&ctx, true); assert(buttons == 3 && scopes == 1);

    /* Color video must keep the native frame path inside the new viewport. */
    client.status.video = true; rtsp.direct_rgb565 = true;
    rtsp.image_width = 160; rtsp.image_height = 120; rtsp.pixels = malloc(160 * 120 * 2);
    render(&ctx, true); assert(frames == 1);
    rtsp.fullscreen = true; rtsp.layout_dirty = true; buttons = 0;
    render(&ctx, true); assert(frames == 2 && buttons == 0);
    solar_os_memory_free(rtsp.pixels); rtsp.pixels = NULL;
    assert(!finishes);
}

static void circular_fullscreen_test(void)
{
    solar_os_gfx_t gfx={.width=291,.height=291,.panel={-60,-60,412,412}};
    solar_os_context_t ctx={.gfx=&gfx};
    solar_os_event_t key={.type=SOLAR_OS_EVENT_CHAR,.data.ch='f'};
    reset(); output_dimensions(&gfx);
    assert(rtsp.output_width==291 && rtsp.output_height==175);
    assert(event(&ctx,&key) && rtsp.output_width==412 && rtsp.output_height==412);
    render(&ctx,true);
    assert(scope_x==-60 && scope_y==-60 && scope_width==412 && scope_height==412);
    client.status.video=true; rtsp.direct_rgb565=true;
    rtsp.image_width=rtsp.image_height=4; rtsp.pixels=malloc(32);
    render(&ctx,true);
    assert(frame_x==-60 && frame_y==-60 && frame_width==412 && frame_height==412);
    key.data.ch='d'; assert(event(&ctx,&key));
    assert(rtsp.output_width==412 && rtsp.output_height==396 && !rtsp.pixels);
    key.data.ch='d'; assert(event(&ctx,&key));
    key.data.ch='f'; assert(event(&ctx,&key));
    assert(!rtsp.fullscreen && rtsp.output_width==291 && rtsp.output_height==175);
}

static void transport_test(void)
{
    solar_os_gfx_t gfx = {.width = 480, .height = 320};
    solar_os_context_t ctx = {.gfx = &gfx};
    reset();
    rtsp.network_task = (void *)1; rtsp.decode_task = (void *)2;
    rtsp.pixels = malloc(16); rtsp.queued = 2;
    rtsp.queue[0].pixels = malloc(16); rtsp.queue[1].pixels = malloc(16);
    toggle_playback(&ctx);
    assert(rtsp.stop && rtsp.stopped && !refreshed && cancellations == 1);
    assert(rtsp.layout_generation == 1 && rtsp.queued == 0 && !rtsp.pixels && freed == 3);
    reap_playback(); assert(!deletions && !destructions);
    rtsp.network_done = true;
    reap_playback(); assert(!deletions && !destructions);
    toggle_playback(&ctx); assert(rtsp.restart && rtsp.stop);
    rtsp.decode_done = true;
    reap_playback(); assert(deletions == 2 && destructions == 1 && !rtsp.client);
    assert(!rtsp.network_task && !rtsp.decode_task && rtsp.dirty);
    reap_playback(); assert(deletions == 2 && destructions == 1);
    render(&ctx, true); assert(button_icon == SOLAR_OS_MEDIA_TRANSPORT_PLAY);

    reset();
    solar_os_input_pointer_event_t press = {.mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_PRESS, .buttons = SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY,
        .x = 240, .y = 305};
    rtsp.fullscreen = true;
    assert(!pointer(&ctx, &press) && !rtsp.stopped);
    rtsp.fullscreen = false;
    press.x = 5; assert(pointer(&ctx, &press) && !rtsp.stopped);
    press.x = 240; assert(pointer(&ctx, &press) && rtsp.stopped);
    assert(pointer(&ctx, &press) && rtsp.restart);
    press.action = SOLAR_OS_INPUT_POINTER_MOVE;
    assert(!pointer(&ctx, &press));
    solar_os_rtsp_client_status_t status;
    playback_status(&status); assert(!status.playing && !status.audio_playing);
    assert(!strcmp(playback_label(&status), "STOPPING"));
    rtsp.network_done = rtsp.decode_done = true;
    assert(!strcmp(playback_label(&status), "STOPPED"));
}

static void reconnect_test(void)
{
    solar_os_gfx_t gfx = {.width = 480, .height = 320};
    solar_os_context_t ctx = {.gfx = &gfx};
    reset(); rtsp.network_task = (void *)1; rtsp.decode_task = (void *)2;
    solar_os_event_t enter = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = '\r'};
    solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK, .data.tick_ms = 1000};
    assert(event(&ctx, &enter) && rtsp.stopped);
    assert(event(&ctx, &enter) && rtsp.restart);
    assert(event(&ctx, &tick) && !creations && !finishes);
    rtsp.network_done = true;
    assert(event(&ctx, &tick) && !creations && !finishes);
    rtsp.decode_done = true; rtsp.audio_only = true; rtsp.diagnostics = true;
    assert(event(&ctx, &tick) && creations == 1 && !finishes);
    assert(!rtsp.stopped && !rtsp.stop && !rtsp.restart && rtsp.network_task);
    assert(last_options.audio && !last_options.video && last_options.diagnostics);
    assert(last_options.reconnect_attempts == 6 && refreshed);
    assert(destructions == 1 && deletions == 2);

    /* Stop again and test a failed client allocation and worker admission. */
    assert(event(&ctx, &enter)); rtsp.network_done = true;
    assert(event(&ctx, &tick) && !rtsp.client);
    allocation_failure = true;
    assert(event(&ctx, &enter)); assert(event(&ctx, &tick));
    assert(finishes == 1 && !rtsp.client && rtsp.network_done && rtsp.decode_done);
    reset(); rtsp.client = NULL; rtsp.network_done = rtsp.decode_done = true;
    task_failure = true;
    assert(start_playback(&ctx) == ESP_ERR_NO_MEM && finishes == 1);
    assert(rtsp.network_done && rtsp.decode_done && !rtsp.network_task);
    reap_playback(); assert(destructions == 1 && !rtsp.client);
}

static void playlist_and_pause_test(void)
{
    solar_os_gfx_t gfx = {.width = 480, .height = 320};
    solar_os_context_t ctx = {.gfx = &gfx};
    reset();
    solar_os_m3u_entry_t entries[2] = {
        {.path = "rtsp://camera-one/media", .title = "One", .duration = -1},
        {.path = "rtsp://camera-two/media", .title = "Two", .duration = -1},
    };
    rtsp.playlist.entries = entries; rtsp.playlist.count = 2;
    solar_os_event_t key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = ' '};
    assert(event(&ctx, &key) && rtsp.paused && client_paused);
    assert(!strcmp(playback_label(&client.status), "PAUSED"));
    assert(event(&ctx, &key) && !rtsp.paused && !client_paused);
    key.data.ch = '\t'; assert(event(&ctx, &key) && rtsp.playlist_visible);
    key.data.ch = SOLAR_OS_KEY_DOWN; assert(event(&ctx, &key) && rtsp.cursor == 1);
    rtsp.network_task = (void *)1; rtsp.decode_task = (void *)2;
    key.data.ch = '\r'; assert(event(&ctx, &key));
    assert(!rtsp.playlist_visible && rtsp.active == 1 && rtsp.stopped && rtsp.restart);
    assert(!strcmp(rtsp.url, entries[1].path));
    assert(!creations && !destructions);
    solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK};
    rtsp.network_done = true; assert(event(&ctx, &tick));
    assert(!creations && !destructions);
    rtsp.decode_done = true; assert(event(&ctx, &tick));
    assert(creations == 1 && destructions == 1 && !rtsp.stopped);
    assert(!rtsp.restart && !rtsp.paused);
}

static void type_text(solar_os_context_t *ctx, const char *text)
{
    while (*text) {
        solar_os_event_t typed = {0};
        if ((unsigned char)*text < 0x20U) {
            typed.type = SOLAR_OS_EVENT_CHAR; typed.data.ch = *text++;
        } else {
            uint32_t codepoint;
            const size_t bytes = solar_os_ui_utf8_decode(text, &codepoint);
            typed.type = SOLAR_OS_EVENT_TEXT;
            memcpy(typed.data.text, text, bytes); text += bytes;
        }
        assert(event(ctx, &typed));
    }
}

static void empty_launch_and_edit_test(bool graphical)
{
    solar_os_gfx_t gfx = {.width = 480, .height = 320};
    solar_os_context_t ctx = {.argc = 1, .gfx = graphical ? &gfx : NULL};
    reset(); rtsp.client = NULL; saved_count = 0;
    mock_media_preferences[SOLAR_OS_MEDIA_RTSP].volume = 64;
    assert(start(&ctx) == ESP_OK && !finishes && !creations);
    assert(solar_os_media_preferences_volume(rtsp.preferences) == 64);
    assert(rtsp.stopped && rtsp.playlist_visible && rtsp.active == SIZE_MAX);
    solar_os_event_t key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = '\r'};
    assert(event(&ctx, &key) && rtsp.playlist_visible && !rtsp.restart && !finishes);
    type_text(&ctx, "aQuiet camera\n");
    assert(rtsp.dialog == 2 && !finishes && !saved_count);
    type_text(&ctx, "\n"); assert(rtsp.dialog == 2 && rtsp.message[0]); /* Incomplete URL. */
    type_text(&ctx, "127.0.0.1/media");
    save_failed = true; type_text(&ctx, "\n");
    assert(rtsp.dialog == 2 && !saved_count && rtsp.message[0]);
    save_failed = false; type_text(&ctx, "\n");
    assert(!rtsp.dialog && saved_count == 1 && !creations && !finishes);
    assert(!strcmp(saved_entries[0].title, "Quiet camera"));
    type_text(&ctx, "e2\n2\n");
    assert(!strcmp(saved_entries[0].title, "Quiet camera2"));
    assert(!strcmp(saved_entries[0].path, "rtsp://127.0.0.1/media2"));
    type_text(&ctx, "e"); key.data.ch = SOLAR_OS_KEY_ESCAPE;
    assert(event(&ctx, &key) && !rtsp.dialog && !finishes);
    save_failed = true; key.data.ch = SOLAR_OS_KEY_DELETE;
    assert(event(&ctx, &key) && saved_count == 1 && rtsp.message[0]);
    save_failed = false; assert(event(&ctx, &key) && saved_count == 0 && !finishes);
    reset(); rtsp.client = NULL; saved_count = 1;
    assert(start(&ctx) == ESP_OK && rtsp.playlist_visible && !creations && !finishes);
    solar_os_event_t volume_key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = '\t'};
    assert(event(&ctx, &volume_key));
    volume_key.data.ch = '+'; assert(event(&ctx, &volume_key));
    assert(solar_os_media_preferences_volume(rtsp.preferences) == 69);
    assert(start_playback(&ctx) == ESP_OK && last_options.use_volume && last_options.volume == 69);
    assert(last_options.device == solar_os_media_preferences_device && last_options.user == rtsp.preferences);
}

static void port_dialog_cursor_test(void)
{
    reset(); rtsp.graphical = false; rtsp.dialog = 1; rtsp.playlist_dirty = true;
    strcpy(rtsp.dialog_input, "Camera");
    rtsp.dialog_input_state.cursor = strlen(rtsp.dialog_input);
    solar_os_context_t ctx = {0};
    track_port_dialog = true; port_cursor_visible = false;
    draw_dialog(&ctx);
    track_port_dialog = false;
    assert(port_cursor_visible);
    const char *help = strstr(port_dialog_output, "Enter next/save");
    const char *input = strstr(port_dialog_output, "Stream name: Camera");
    assert(help && input && help < input);
    assert(!strcmp(input, "Stream name: Camera\033[K")); /* Cursor remains at input end. */
    rtsp.dialog_input_state.cursor = 2; rtsp.playlist_dirty = true;
    track_port_dialog = true;
    draw_dialog(&ctx);
    track_port_dialog = false;
    assert(strstr(port_dialog_output, "Stream name: Camera\033[K\033[4D"));
    port_cols = 16;
    rtsp.dialog_input_state.cursor = strlen(rtsp.dialog_input); rtsp.playlist_dirty = true;
    track_port_dialog = true;
    draw_dialog(&ctx);
    track_port_dialog = false;
    assert(rtsp.dialog_input_state.view == 5 && strstr(port_dialog_output, "Stream name: a\033[K"));
    port_cols = 80;
}

static void utf8_dialog_and_password_cursor_test(bool graphical)
{
    reset(); rtsp.graphical = graphical; rtsp.dialog = 1;
    solar_os_gfx_t gfx = {.width=320,.height=240};
    solar_os_context_t ctx = {.gfx = graphical ? &gfx : NULL};
    type_text(&ctx, "T\xc3\xb6ne");
    assert(!strcmp(rtsp.dialog_input, "T\xc3\xb6ne"));
    solar_os_event_t key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = SOLAR_OS_KEY_HOME};
    assert(event(&ctx, &key) && !rtsp.dialog_input_state.cursor);
    type_text(&ctx, "\xc3\x84");
    assert(!strcmp(rtsp.dialog_input, "\xc3\x84T\xc3\xb6ne"));
    key.data.ch = SOLAR_OS_KEY_DELETE;
    assert(event(&ctx, &key) && !strcmp(rtsp.dialog_input, "\xc3\x84\xc3\xb6ne"));
    key.data.ch = '\r'; assert(event(&ctx, &key) && rtsp.dialog == 2);
    type_text(&ctx, "rtsp://user:secret@127.0.0.1/media");
    rtsp.dialog_input_state.cursor = 16; rtsp.playlist_dirty = true;
    char masked[SOLAR_OS_RTSP_URL_MAX];
    const char *displayed = dialog_display_text(masked, sizeof(masked));
    assert(!strcmp(displayed, "rtsp://user:******@127.0.0.1/media"));
    assert(strlen(displayed) == strlen(rtsp.dialog_input));
    track_port_dialog = !graphical;
    draw_dialog(&ctx);
    track_port_dialog = false;
    assert(rtsp.dialog_input_state.cursor == 16 && strstr(rtsp.dialog_input, "secret"));
    if (!graphical) assert(!strstr(port_dialog_output, "secret") && strstr(port_dialog_output, "******"));
}

int main(void)
{
    layout_test(480, 320);
    layout_test(400, 300);
    layout_test(320, 240);
    circular_fullscreen_test();
    transport_test();
    reconnect_test();
    playlist_and_pause_test();
    empty_launch_and_edit_test(true);
    empty_launch_and_edit_test(false);
    port_dialog_cursor_test();
    utf8_dialog_and_password_cursor_test(true);
    utf8_dialog_and_password_cursor_test(false);
    puts("rtsp UI layout and transport tests: ok");
    return 0;
}
