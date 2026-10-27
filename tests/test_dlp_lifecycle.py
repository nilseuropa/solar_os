"""Run production lifecycle functions with controlled input/transport failures."""
from pathlib import Path
import unittest

from test_shutdown_policy import function, run_c

ROOT = Path(__file__).resolve().parents[1]


class DlpLifecycleTest(unittest.TestCase):
    def test_exit_keys_disconnect_retry_and_resume(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define SOLAR_OS_EVENT_RESUME 1
#define SOLAR_OS_EVENT_CHAR 2
#define SOLAR_OS_EVENT_TICK 3
#define SOLAR_OS_EVENT_KEY 4
#define SOLAR_OS_KEY_APP_EXIT 0x92u
#define SOLAR_OS_KEY_DELETE 0x96u
#define SOLAR_OS_KEY_ESCAPE 27
#define SOLAR_OS_INPUT_KEY_PRESS 0
#define SOLAR_OS_INPUT_KEY_RELEASE 1
#define SOLAR_OS_INPUT_MOD_CTRL 1
#define SOLAR_OS_INPUT_MOD_ALT 2
#define SOLAR_OS_STREAM_DIRECTION_DUPLEX 3
#define SOLAR_OS_GFX_COLOR_WHITE 0
#define SOLAR_OS_GFX_COLOR_BLACK 3
#define DLP_OK 0
typedef struct { int type; union {
    char ch;
    struct { uint8_t key, modifiers; int action; } key;
} data; } solar_os_event_t;
typedef struct { int direction; } solar_os_stream_open_options_t;
typedef struct { bool valid; } stream_t;
typedef struct { bool finished, graphics; } solar_os_context_t;
static struct {
    struct {
        struct { uint64_t boot; struct { int width, height; } info; } device;
        uint8_t *front;
    } receiver;
    stream_t stream;
    char stream_name[16];
    int64_t retry_at_us;
    bool direct_mono, direct_index2, suspended;
    unsigned presented_format;
    size_t presented_size;
    void *gfx;
} state;
static int64_t clock_us;
static int pump_result, pumps, resets, opens, closes, draws, waiting_draws;
static esp_err_t open_result, close_result;
static uint8_t image;
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool solar_os_stream_handle_valid(stream_t *s) { return s->valid; }
static esp_err_t solar_os_stream_close_ex(stream_t *s) {
    closes++; if (!close_result) s->valid = false; return close_result;
}
static esp_err_t solar_os_stream_open_ex(const char *name, const char *owner,
    const solar_os_stream_open_options_t *options, stream_t *s) {
    assert(!s->valid && !strcmp(name, "test") && !strcmp(owner, "app-dlp"));
    assert(options->direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX);
    opens++; if (!open_result) s->valid = true; return open_result;
}
static int solar_os_dlp_receiver_pump(void *r, int64_t now) {
    assert(r == &state.receiver && now == clock_us && state.stream.valid);
    pumps++; return pump_result;
}
static void solar_os_dlp_receiver_reset(void *r, uint64_t boot) {
    assert(r == &state.receiver && boot && boot != state.receiver.device.boot);
    state.receiver.device.boot = boot; resets++;
}
static void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message) {
    assert(!code && !message); ctx->finished = true;
}
static void publish_status(bool running) { assert(running); }
static void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active) { ctx->graphics = active; }
static void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx, bool active) { ctx->graphics = active; }
static void solar_os_gfx_clear(void *gfx, int color) { (void)gfx; assert(color == 0); }
static void solar_os_gfx_set_color(void *gfx, int color) { (void)gfx; assert(color == 3); }
static void solar_os_gfx_text(void *gfx, int x, int y, const char *text) {
    (void)gfx; assert(x == 8 && y == 20 && strstr(text, "waiting")); waiting_draws++;
}
static void solar_os_gfx_present(void *gfx) { (void)gfx; }
static int draw_image(void *s, unsigned format, const uint8_t *data, size_t size,
    int width, int height, uint32_t *render, uint32_t *display) {
    assert(s == &state && data == &image && size == 1 && format == 4);
    assert(width == 8 && height == 1); (void)render; (void)display; draws++; return DLP_OK;
}
'''
        for name in ("draw_waiting", "reopen_stream", "resume", "event"):
            # event returns bool, resume/draw_waiting/reopen_stream return void.
            source += function("src/apps/solar_os_dlp_app.c", name) + "\n"
        run_c(source + r'''
int main(void) {
    solar_os_context_t ctx = {0};
    solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK};
    strcpy(state.stream_name, "test"); state.stream.valid = true;
    state.receiver.device.boot = UINT64_MAX; pump_result = -1;
    assert(event(&ctx, &tick));
    assert(!ctx.finished && resets == 1 && opens == 1 && closes == 1);
    assert(state.receiver.device.boot == 1 && !state.retry_at_us);
    /* A removed/busy provider retries with backoff; it never ends the app. */
    open_result = ESP_ERR_NOT_FOUND; clock_us = 1;
    event(&ctx, &tick); assert(!state.stream.valid && state.retry_at_us == 250001);
    int previous_pumps = pumps;
    clock_us = 250000; event(&ctx, &tick);
    assert(pumps == previous_pumps && opens == 2);
    open_result = ESP_OK; clock_us = 250001; event(&ctx, &tick);
    assert(state.stream.valid && opens == 3 && !state.retry_at_us && !ctx.finished);
    /* A failed close retains its handle and is retried before pumping. */
    close_result = ESP_FAIL; event(&ctx, &tick);
    assert(state.stream.valid && state.retry_at_us && !ctx.finished);
    close_result = ESP_OK; clock_us += 250000; event(&ctx, &tick);
    assert(state.stream.valid && !state.retry_at_us && opens == 4);
    solar_os_event_t redraw = {.type = SOLAR_OS_EVENT_RESUME};
    state.direct_index2 = true; state.suspended = true;
    event(&ctx, &redraw); assert(!state.suspended && ctx.graphics && waiting_draws == 1);
    state.presented_size = 1; state.presented_format = 4; state.receiver.front = &image;
    state.receiver.device.info.width = 8; state.receiver.device.info.height = 1;
    event(&ctx, &redraw); assert(draws == 1 && waiting_draws == 1);
    solar_os_event_t key = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = 'x'};
    event(&ctx, &key); assert(!ctx.finished);
    key.data.ch = (char)SOLAR_OS_KEY_APP_EXIT; event(&ctx, &key); assert(ctx.finished);
    ctx.finished = false; state.retry_at_us = clock_us + 250000;
    key.data.ch = SOLAR_OS_KEY_ESCAPE; event(&ctx, &key); assert(ctx.finished);
    ctx.finished = false; key.data.ch = 0x1d;
    event(&ctx, &key); assert(ctx.finished);
    /* Raw key sources preserve modifiers instead of sending APP_EXIT. Test
     * both an idle receiver and one backing off after a transport failure. */
    for (unsigned retry = 0; retry < 2; retry++) {
        state.retry_at_us = retry ? clock_us + 250000 : 0;
        key.type = SOLAR_OS_EVENT_KEY;
        key.data.key.action = SOLAR_OS_INPUT_KEY_PRESS;
        key.data.key.key = SOLAR_OS_KEY_DELETE;
        key.data.key.modifiers = SOLAR_OS_INPUT_MOD_ALT;
        ctx.finished = false; event(&ctx, &key); assert(!ctx.finished);
        key.data.key.modifiers |= SOLAR_OS_INPUT_MOD_CTRL;
        key.data.key.action = SOLAR_OS_INPUT_KEY_RELEASE;
        event(&ctx, &key); assert(!ctx.finished);
        key.data.key.action = SOLAR_OS_INPUT_KEY_PRESS;
        event(&ctx, &key); assert(ctx.finished);
        ctx.finished = false;
        key.data.key.key = ']'; key.data.key.modifiers = SOLAR_OS_INPUT_MOD_CTRL;
        event(&ctx, &key); assert(ctx.finished);
        ctx.finished = false;
        key.data.key.key = SOLAR_OS_KEY_APP_EXIT; key.data.key.modifiers = 0;
        event(&ctx, &key); assert(ctx.finished);
        ctx.finished = false;
        key.data.key.key = SOLAR_OS_KEY_ESCAPE;
        event(&ctx, &key); assert(ctx.finished);
    }
}
''')

    def test_receiver_requests_key_events_with_modifiers(self):
        source = (ROOT / "src/apps/solar_os_dlp_app.c").read_text()
        descriptor = source[source.index("const solar_os_app_t solar_os_dlp_app ="):]
        self.assertIn("SOLAR_OS_APP_FLAG_KEY_EVENTS", descriptor)

    def test_remote_close_resumes_display_parent_before_restoring_caller(self):
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define SOLAR_OS_SESSION_MAX 3
#define SOLAR_OS_LOGI(...) ((void)0)
typedef struct {
    bool used, has_return_session;
    uint8_t id, return_session_id;
    const void *app;
} solar_os_session_entry_t;
typedef struct { int context; } solar_os_session_context_snapshot_t;
static struct {
    solar_os_session_entry_t sessions[3], *foreground_session;
    const void *foreground_app;
    bool legacy_return_session_valid;
    uint8_t legacy_return_session_id;
    void *ctx, *shell_terminal;
} session_state;
static int context, resumed, restored, stopped;
static bool stop_ok = true;
static bool session_is_closable(solar_os_session_entry_t *s) { return s && s->used; }
static solar_os_session_entry_t *session_return_target(uint8_t id, solar_os_session_entry_t *s) {
    assert(id != s->id); return &session_state.sessions[id];
}
static bool session_owns_current_context(solar_os_session_entry_t *s) { return context == s->id; }
static void session_context_capture(solar_os_session_context_snapshot_t *s) { s->context = context; }
static void session_context_restore(const solar_os_session_context_snapshot_t *s) { context = s->context; restored++; }
static void session_prepare_context(solar_os_session_entry_t *s) { context = s->id; }
static bool solar_os_app_stop_checked(const void *app, void *ctx) { (void)ctx; assert(app); stopped++; return stop_ok; }
static void session_transfer_exit_result(solar_os_session_entry_t *s) { assert(context == s->id); }
static void session_release_display(solar_os_session_entry_t *s) { (void)s; }
static void session_release_display_target(solar_os_session_entry_t *s) { (void)s; }
static void session_free_terminal(solar_os_session_entry_t *s) { (void)s; }
static void session_free_shell_session(solar_os_session_entry_t *s) { (void)s; }
static bool switch_to_session_or_shell(solar_os_session_entry_t *s) {
    assert(s->used && !session_state.foreground_session);
    session_state.foreground_session = s; session_state.foreground_app = s->app;
    context = s->id; resumed++; return true;
}
static solar_os_session_entry_t *ensure_shell_session(void) { return &session_state.sessions[0]; }
static bool switch_to_session(solar_os_session_entry_t *s, bool overlay) { assert(!overlay); return switch_to_session_or_shell(s); }
static void session_restore_base_context(void) { context = -1; }
static void restore_foreground_context(void) { context = session_state.foreground_session->id; }
''' + function("src/services/solar_os_sessions.c", "close_session") + r'''
static solar_os_session_entry_t *setup(void) {
    static int app;
    session_state.sessions[0] = (solar_os_session_entry_t){.used = true, .id = 0, .app = &app};
    session_state.sessions[1] = (solar_os_session_entry_t){.used = true, .id = 1, .app = &app,
        .has_return_session = true, .return_session_id = 0};
    session_state.foreground_session = &session_state.sessions[1];
    session_state.foreground_app = &app;
    return &session_state.sessions[1];
}
int main(void) {
    solar_os_session_entry_t *child = setup();
    context = 2; /* Auxiliary console closes an app on display0. */
    assert(close_session(child, true));
    assert(!child->used && resumed == 1 && restored == 1 && context == 2);
    assert(session_state.foreground_session == &session_state.sessions[0]);
    child = setup(); context = 1; assert(close_session(child, true));
    assert(resumed == 2 && restored == 1 && context == 0);
    child = setup(); context = 2; stop_ok = false;
    assert(!close_session(child, true));
    assert(child->used && session_state.foreground_session == child && context == 2);
    assert(resumed == 2 && stopped == 3);
}
''')


if __name__ == "__main__":
    unittest.main()
