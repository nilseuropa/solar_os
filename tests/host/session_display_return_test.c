#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOLAR_OS_SESSION_MAX 8
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 24
#define SOLAR_OS_LOGI(...) ((void)0)
#define SOLAR_OS_LOGW(...) ((void)0)
typedef int solar_os_gfx_t;
typedef int solar_os_shell_io_t;
typedef int solar_os_event_t;
typedef struct { const char *name; bool resumable; } solar_os_app_t;
typedef struct {
    bool used, suspended, started, has_return_session, close_on_exit;
    uint8_t id, return_session_id;
    const solar_os_app_t *app;
    void *terminal;
    solar_os_gfx_t *gfx;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} solar_os_session_entry_t;
typedef struct { int context; } solar_os_session_context_snapshot_t;
static const solar_os_app_t shell = {"shell", false};
static const solar_os_app_t game = {"gameboy", true};
static const solar_os_app_t gui = {"gui", false};
static const solar_os_app_t files = {"files", true};
static struct {
    solar_os_session_entry_t sessions[SOLAR_OS_SESSION_MAX], *foreground_session;
    const solar_os_app_t *foreground_app;
    bool legacy_return_session_valid;
    uint8_t legacy_return_session_id;
    void *ctx, *shell_terminal;
    solar_os_gfx_t *default_gfx;
} session_state;
static char input_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
static int context, delivered, prompts, result_shell;

static size_t strlcpy(char *dest, const char *source, size_t size)
{
    size_t len = strlen(source);
    if (size) snprintf(dest, size, "%s", source);
    return len;
}
static const solar_os_app_t *solar_os_shell_app(void) { return &shell; }
static bool app_is_resumable(const solar_os_app_t *app) { return app->resumable; }
static bool app_is_shell_inline(const solar_os_app_t *app) { (void)app; return false; }
static const char *app_display_name(const solar_os_app_t *app) { return app->name; }
static solar_os_shell_io_t *solar_os_context_shell_io(void *ctx) { (void)ctx; return NULL; }
#define solar_os_shell_io_printf(io, ...) ((void)(io), (void)printf(__VA_ARGS__))
#define solar_os_shell_io_flush(...) ((void)0)
static bool session_args_match_context(solar_os_session_entry_t *s, void *ctx)
{ (void)s; (void)ctx; return true; }
static solar_os_session_entry_t *session_find_by_app(const solar_os_app_t *app)
{
    for (int i = 0; i < SOLAR_OS_SESSION_MAX; i++)
        if (session_state.sessions[i].used && session_state.sessions[i].app == app)
            return &session_state.sessions[i];
    return NULL;
}
static solar_os_session_entry_t *session_alloc(const solar_os_app_t *app)
{
    for (int i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *s = &session_state.sessions[i];
        if (s->used) continue;
        *s = (solar_os_session_entry_t){.id = i, .app = app};
        return s;
    }
    return NULL;
}
static bool switch_to_session(solar_os_session_entry_t *s, bool overlay)
{
    assert(!overlay && s);
    if (session_state.foreground_session) session_state.foreground_session->suspended = true;
    s->used = s->started = true;
    s->suspended = false;
    session_state.foreground_session = s;
    session_state.foreground_app = s->app;
    context = s->id;
    if (s->app == &shell) prompts++;
    return true;
}
static bool close_session(solar_os_session_entry_t *s, bool preserve);
static bool session_is_closable(solar_os_session_entry_t *s) { return s && s->used; }
static bool session_owns_current_context(solar_os_session_entry_t *s) { return context == s->id; }
static void session_context_capture(solar_os_session_context_snapshot_t *s) { s->context = context; }
static void session_context_restore(const solar_os_session_context_snapshot_t *s) { context = s->context; }
static void session_prepare_context(solar_os_session_entry_t *s) { context = s->id; }
static bool solar_os_app_stop_checked(const solar_os_app_t *app, void *ctx)
{ (void)ctx; assert(app); return true; }
static void session_transfer_exit_result(solar_os_session_entry_t *s);
static void session_release_display(solar_os_session_entry_t *s) { (void)s; }
static void session_release_display_target(solar_os_session_entry_t *s) { (void)s; }
static void session_free_terminal(solar_os_session_entry_t *s) { (void)s; }
static void session_free_shell_session(solar_os_session_entry_t *s) { (void)s; }
static solar_os_session_entry_t *ensure_shell_session(void) { return &session_state.sessions[0]; }
static bool switch_to_session_or_shell(solar_os_session_entry_t *s)
{ return switch_to_session(s ? s : ensure_shell_session(), false); }
static void session_restore_base_context(void) { context = -1; }
static void restore_foreground_context(void) { context = session_state.foreground_session->id; }
static bool session_copy_input_focus(char *target, size_t size)
{ strlcpy(target, input_target, size); return target[0] != '\0'; }
static bool solar_os_sessions_dispatch_session_event(uint8_t id, const solar_os_event_t *event)
{ assert(event); delivered = id; return true; }
static void solar_os_sessions_dispatch_foreground_event(const solar_os_event_t *event)
{ assert(event && session_state.foreground_session); delivered = session_state.foreground_session->id; }

#include "session_display_return.inc"

static void session_transfer_exit_result(solar_os_session_entry_t *s)
{
    solar_os_session_entry_t *destination = session_return_shell(s);
    result_shell = destination ? destination->id : 0;
}

static solar_os_session_entry_t *setup(bool external)
{
    static solar_os_gfx_t builtin_gfx, external_gfx;
    memset(&session_state, 0, sizeof(session_state));
    session_state.default_gfx = &builtin_gfx;
    session_state.shell_terminal = &builtin_gfx;
    session_state.sessions[0] = (solar_os_session_entry_t){
        .used = true, .started = true, .id = 0, .app = &shell, .gfx = &builtin_gfx};
    strlcpy(session_state.sessions[0].display_target, "display0", SOLAR_OS_DISPLAY_TARGET_NAME_MAX);
    session_state.sessions[1] = (solar_os_session_entry_t){
        .used = true, .started = true, .id = 1, .app = &shell, .gfx = &external_gfx};
    strlcpy(session_state.sessions[1].display_target, "lcd0", SOLAR_OS_DISPLAY_TARGET_NAME_MAX);
    solar_os_session_entry_t *parent = &session_state.sessions[external ? 1 : 0];
    session_state.sessions[external ? 0 : 1].suspended = true;
    session_state.foreground_session = parent;
    session_state.foreground_app = &shell;
    strlcpy(input_target, parent->display_target, sizeof(input_target));
    context = parent->id;
    delivered = result_shell = -1;
    prompts = 0;
    return parent;
}

static void assert_shell_return(solar_os_session_entry_t *parent, solar_os_session_entry_t *child)
{
    assert(close_session(child, false));
    assert(!child->used && session_state.foreground_session == parent);
    assert(!parent->suspended && prompts == 1 && result_shell == parent->id);
    solar_os_event_t event = 1;
    assert(solar_os_sessions_dispatch_input_event(&event));
    assert(delivered == parent->id);
}

int main(void)
{
    for (int external = 0; external < 2; external++) {
        for (int resumable = 0; resumable < 2; resumable++) {
            const solar_os_app_t *app = resumable ? &game : &gui;
            solar_os_session_entry_t *parent = setup(external);
            assert(switch_to_display_app(app, parent, false));
            solar_os_session_entry_t *child = session_state.foreground_session;
            assert(child != parent && parent->suspended);
            assert(session_uses_same_display(parent, child));
            assert(child->close_on_exit == !resumable);
            assert_shell_return(parent, child);
            /* A retained app still returns to its display after fg resumes it. */
            parent = setup(external);
            assert(switch_to_display_app(app, parent, false));
            child = session_state.foreground_session;
            assert(switch_to_session(parent, false));
            prompts = 0;
            assert(switch_to_session(child, false));
            assert_shell_return(parent, child);
        }
    }
    /* Explicit children return to their parent app, then to its own shell. */
    solar_os_session_entry_t *parent = setup(true);
    assert(switch_to_display_app(&files, parent, false));
    solar_os_session_entry_t *file_session = session_state.foreground_session;
    assert(switch_to_display_app(&gui, file_session, true));
    solar_os_session_entry_t *child = session_state.foreground_session;
    assert(child->close_on_exit);
    assert(close_session(child, false));
    assert(session_state.foreground_session == file_session && !file_session->suspended);
    assert_shell_return(parent, file_session);

    /* A replacement handoff skips the old app and returns to the same shell. */
    parent = setup(true);
    assert(switch_to_display_app(&files, parent, false));
    file_session = session_state.foreground_session;
    assert(switch_to_display_app(&gui, file_session, false));
    child = session_state.foreground_session;
    assert_shell_return(parent, child);
    assert(file_session->used && file_session->suspended);
    return 0;
}
