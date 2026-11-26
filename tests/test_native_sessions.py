from pathlib import Path
import re
import unittest

from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class NativeSessionsTest(unittest.TestCase):
    def test_copy_display_and_port_sessions_through_one_table(self):
        source = ROOT / "src/services"
        sessions = (source / "solar_os_sessions.c").read_text()
        ports = (source / "solar_os_port_shell.c").read_text()
        request = re.search(r"typedef struct \{\n    uint32_t kinds;[\s\S]*?\} session_snapshot_request_t;",
                            sessions).group()
        request += re.search(r"typedef struct \{\n    solar_os_context_t \*caller;[\s\S]*?\} session_open_shell_request_t;",
                             sessions).group()
        request += re.search(r"typedef struct \{\n    uint32_t direction;[\s\S]*?\} session_focus_move_t;", sessions).group()
        functions = production_functions(ports, ("port_shell_copy_info", "solar_os_port_shell_snapshot"))
        functions += production_functions(sessions, ("sessions_snapshot_on_owner", "solar_os_sessions_snapshot",
            "sessions_display_focus_on_owner", "solar_os_sessions_get_display_focus",
             "session_open_shell_on_owner", "solar_os_sessions_open_shell", "session_active_for_display",
            "solar_os_sessions_active_for_display", "session_move_display_focus_on_owner",
            "solar_os_sessions_move_display_focus"))
        run_c(r'''
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "solar_os_app_registry.h"
#include "solar_os_memory.h"
#include "solar_os_native_sessions.h"
#include "solar_os_native_sessions_abi.h"
#include "solar_os_native_terminal_abi.h"
#define SOLAR_OS_SESSION_MAX 4
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
#define PORT_SHELL_MAX 2
#define PORT_APP_SESSION_MAX 2
struct solar_os_terminal { int marker; };
struct solar_os_shell_io { int marker; };
enum { SOLAR_OS_SHELL_IO_KIND_NONE, SOLAR_OS_SHELL_IO_KIND_TERMINAL, SOLAR_OS_SHELL_IO_KIND_PORT };
static solar_os_terminal_t own_terminal;
static solar_os_shell_io_t port_io;
static bool using_port;
typedef struct { int terminal_profile, charset; uint16_t cols, rows; } solar_os_port_shell_options_t;
#define SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO 0
#define SOLAR_OS_SHELL_CHARSET_UTF8 0
typedef struct { const char *name; void *event; } test_app_t;
typedef struct {
    bool used, suspended;
    uint8_t id;
    const test_app_t *app;
    char title[48], display_target[16];
    int *terminal;
    solar_os_job_tick_stats_v1_t tick_stats;
} solar_os_session_entry_t;
typedef solar_os_session_entry_t port_app_session_t;
typedef struct {
    bool used, running, stop_requested;
    uint8_t id;
    int terminal_profile;
    char port_name[24];
    port_app_session_t app_sessions[PORT_APP_SESSION_MAX];
    port_app_session_t *active_app_session;
    solar_os_job_tick_stats_v1_t tick_stats;
} port_shell_state_t;
static port_shell_state_t port_shells[PORT_SHELL_MAX];
static struct {
    solar_os_session_entry_t sessions[SOLAR_OS_SESSION_MAX];
    solar_os_session_entry_t *foreground_session;
    solar_os_context_t *ctx;
} session_state;
static solar_os_context_t base_context, app_context;
static bool app_scope, shell_scope;
static const solar_os_app_t shell_descriptor, app_descriptor;
solar_os_context_t *solar_os_application_context(void) { return app_scope ? &app_context : NULL; }
const solar_os_app_t *solar_os_application_descriptor(void) {
    return app_scope ? (shell_scope ? &shell_descriptor : &app_descriptor) : NULL;
}
const solar_os_app_t *solar_os_shell_app(void) { return &shell_descriptor; }
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx) {
    assert(ctx == &app_context);
    return using_port ? &port_io : NULL;
}
int solar_os_shell_io_kind(const solar_os_shell_io_t *io) {
    return io == &port_io ? SOLAR_OS_SHELL_IO_KIND_PORT : SOLAR_OS_SHELL_IO_KIND_TERMINAL;
}
solar_os_terminal_t *solar_os_context_terminal(solar_os_context_t *ctx) {
    assert(ctx == &app_context && !using_port);
    return &own_terminal;
}
esp_err_t solar_os_sessions_set_terminal_preference(solar_os_terminal_t *term, uint32_t field, uint32_t value) {
    assert(term == (using_port ? NULL : &own_terminal));
    return field == SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE && value <= 1 ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t solar_os_sessions_set_display_preference(const char *name, uint32_t field, uint32_t value, bool save) {
    assert(!strcmp(name, "display1") && field == SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_SCALE && value == 3 && save);
    return ESP_OK;
}
static bool on_owner, shutting_down, port_shells_lock;
static uint8_t closed_id, deferred_id;
static const solar_os_event_t *caller_event;
static unsigned port_switches;
bool solar_os_sessions_dispatch_session_event(uint8_t id, const solar_os_event_t *event) {
    assert(on_owner && event != caller_event && event->type == SOLAR_OS_EVENT_TEXT);
    assert(!strcmp(event->data.text, "é"));
    return id == 2;
}
esp_err_t solar_os_sessions_close_display(const char *target) {
    assert(!app_scope);
    return !target ? ESP_ERR_INVALID_ARG : !strcmp(target, "display1") ? ESP_OK : ESP_ERR_NOT_FOUND;
}
esp_err_t solar_os_port_shell_stop(uint8_t id) {
    return id == 16 ? ESP_OK : ESP_ERR_NOT_FOUND;
}
bool solar_os_port_shell_is_app_session_id(uint8_t id) { return id >= 32 && id < 36; }
bool solar_os_port_shell_context_owns_session(const solar_os_context_t *ctx, uint8_t id) {
    assert(ctx == &app_context);
    return using_port && id == 16;
}
bool solar_os_port_shell_context_owns_app_session(const solar_os_context_t *ctx, uint8_t id) {
    assert(ctx == &app_context);
    return using_port && id == 32;
}
esp_err_t solar_os_port_shell_foreground_app_session(solar_os_context_t *ctx, uint8_t id) {
    assert(ctx == solar_os_application_context());
    if (id != 32) return ESP_ERR_NOT_FOUND;
    port_switches++;
    return ESP_OK;
}
esp_err_t solar_os_port_shell_foreground_last_app(solar_os_context_t *ctx, uint8_t *id) {
    assert(ctx == &app_context && using_port && shell_scope && id);
    *id = 32;
    port_switches++;
    return ESP_OK;
}
bool solar_os_sessions_cycle_input_focus(void) { assert(on_owner); return true; }
bool solar_os_sessions_cycle_input_focus_previous(void) { assert(on_owner); return false; }
esp_err_t solar_os_sessions_send_command(uint8_t id, const char *command, solar_os_shell_io_t *io) {
    assert(id == 0 && command && !strcmp(command, "status"));
    assert(io == (using_port && app_scope ? &port_io : NULL));
    return ESP_OK;
}
esp_err_t solar_os_sessions_close_any(uint8_t id, solar_os_shell_io_t *io) {
    assert(!io && !app_scope);
    closed_id = id;
    return ESP_OK;
}
void solar_os_context_request_session_close(solar_os_context_t *ctx, uint8_t id) {
    assert(app_scope && ctx == &app_context);
    deferred_id = id;
}
#define portENTER_CRITICAL(lock) do { assert(!*(lock)); *(lock) = true; } while (0)
#define portEXIT_CRITICAL(lock) do { assert(*(lock)); *(lock) = false; } while (0)
static test_app_t shell = {.name = "shell"};
static test_app_t editor = {.name = "editor", .event = &shell};
size_t strlcpy(char *dst, const char *src, size_t capacity) {
    const size_t size = strlen(src);
    if (capacity) {
        size_t bytes = size < capacity - 1 ? size : capacity - 1;
        memcpy(dst, src, bytes);
        dst[bytes] = 0;
    }
    return size;
}
static const test_app_t *fixture_shell_app(void) { return &shell; }
#define solar_os_shell_app fixture_shell_app
static const char *app_display_name(const test_app_t *app) { return app->name; }
static const char *solar_os_shell_terminal_profile_name(int profile) {
    assert(profile == 1); return "vt100";
}
static bool session_terminal_target_name(const solar_os_session_entry_t *entry,
    const int *terminal, char *target, size_t capacity) {
    assert(on_owner && terminal == entry->terminal);
    strlcpy(target, entry->display_target, capacity); return target[0] != 0;
}
bool solar_os_shutdown_requested(void) { return shutting_down; }
esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*run)(void *), void *context) {
    assert(!on_owner);
    on_owner = true; esp_err_t error = run(context); on_owner = false;
    return error;
}
typedef enum {
    SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL, SOLAR_OS_DISPLAY_LAYOUT_VERTICAL,
} solar_os_display_layout_axis_t;
static solar_os_display_layout_axis_t last_axis;
static bool last_previous;
static bool session_cycle_display_focus(solar_os_display_layout_axis_t axis, bool previous) {
    assert(on_owner);
    last_axis = axis;
    last_previous = previous;
    return !previous;
}
bool solar_os_sessions_context_uses_display(solar_os_context_t *ctx, const char *target) {
    assert(ctx == &app_context && !using_port);
    return target && !strcmp(target, "display1");
}
static const solar_os_app_t registered_app = {.name = "editor"};
static unsigned direct_launches, deferred_launches, launch_allocations;
static bool fail_launch_allocation;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag) {
    assert(size == 2 * SOLAR_OS_APP_ARG_LEN && cls == SOLAR_OS_MEMORY_TRANSIENT && !strcmp(tag, "session.launch.args"));
    if (fail_launch_allocation) return NULL;
    void *ptr = malloc(size);
    if (ptr) ++launch_allocations;
    return ptr;
}
void solar_os_memory_free(void *ptr) { assert(ptr && launch_allocations); --launch_allocations; free(ptr); }
const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name) {
    static const solar_os_app_registry_entry_t app = {.name = "editor", .app = &registered_app,
        .capabilities = SOLAR_OS_APP_CAP_DISPLAY, .min_argc = 2, .max_argc = 2};
    return !strcmp(name, app.name) ? &app : NULL;
}
esp_err_t solar_os_shell_resolve_path(solar_os_context_t *caller, const char *path, char *out, size_t capacity) {
    assert(caller == &app_context && !path);
    strlcpy(out, "/sd/documents", capacity);
    return ESP_OK;
}
esp_err_t solar_os_storage_resolve_path_at(const char *base, const char *path, char *out, size_t capacity) {
    assert(!base && !path);
    strlcpy(out, "/flash", capacity);
    return ESP_OK;
}
esp_err_t solar_os_app_registry_request_launch(solar_os_context_t *caller, const char *name,
    size_t count, const char *const args[]) {
    assert(caller == &app_context && !strcmp(name, "editor") && count == 1 && !strcmp(args[0], "notes.txt"));
    caller->launch_cwd[0] = 0;
    ++deferred_launches;
    return ESP_OK;
}
esp_err_t solar_os_sessions_create_display_app_at(const solar_os_app_t *app, const char *target,
    int argc, char **argv, const char *cwd, uint8_t *id, char *busy, size_t capacity) {
    assert(app == &registered_app && argc == 2 && launch_allocations == 1);
    app_context.argv[1][0] = 0;
    assert(!strcmp(argv[0], "editor") && !strcmp(argv[1], "notes.txt"));
    ++direct_launches;
    if (!strcmp(target, "busy")) {
        strlcpy(busy, "other-session", capacity);
        return ESP_ERR_INVALID_STATE;
    }
    assert(!strcmp(target, "display0"));
    assert(!strcmp(cwd, app_scope ? "/sd/documents" : "/flash"));
    *id = 4;
    return ESP_OK;
}
static char focus[16] = "display0";
static bool solar_os_sessions_input_focus(char *target, size_t capacity) {
    assert(on_owner);
    strlcpy(target, focus, capacity); return target[0] != 0;
}
esp_err_t solar_os_sessions_focus_display(const char *target) {
    if (!target) return ESP_ERR_INVALID_ARG;
    if (strcmp(target, "display1")) return ESP_ERR_NOT_FOUND;
    strlcpy(focus, target, sizeof(focus)); return ESP_OK;
}
static bool solar_os_shell_parse_terminal_profile(const char *name, int *out) {
    if (strcmp(name, "vt100")) return false;
    *out = 1; return true;
}
static bool solar_os_shell_parse_charset(const char *name, int *out) {
    if (strcmp(name, "ascii")) return false;
    *out = 1; return true;
}
static esp_err_t create_display_shell(const char *name, bool activate, uint8_t *id, char *owner, size_t capacity) {
    assert(on_owner && !activate);
    if (!strcmp(name, "busy")) {
        strlcpy(owner, "other-session", capacity);
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(name, "display1")) return ESP_ERR_NOT_FOUND;
    *id = 3; return ESP_OK;
}
static esp_err_t solar_os_port_shell_start_with_options(solar_os_context_t *ctx, const char *name,
    const solar_os_port_shell_options_t *options, bool startup, uint8_t *id) {
    assert(on_owner && ctx == (app_scope ? &app_context : &base_context) && !strcmp(name, "usb"));
    assert(options->terminal_profile == 1 && options->charset == 1 && startup);
    assert(options->cols == 100 && options->rows == 35);
    *id = 16; return ESP_OK;
}
esp_err_t solar_os_port_shell_set_dimensions(uint8_t id, uint16_t cols, uint16_t rows) {
    return id == 16 && cols == 100 && rows == 35 ? ESP_OK : ESP_ERR_NOT_FOUND;
}
''' + request + functions + r'''
int main(void) {
    const solar_os_native_sessions_api_v1_t *api =
        solar_os_native_sessions_get_service("sessions", 1, sizeof(*api));
    assert(api && !solar_os_native_sessions_get_service("sessions", 2, 8));
    session_state.sessions[0] = (solar_os_session_entry_t){
        .used = true, .id = 0, .app = &shell, .title = "console", .display_target = "display0"};
    session_state.sessions[2] = (solar_os_session_entry_t){
        .used = true, .id = 2, .app = &editor, .title = "document", .display_target = "display1",
        .tick_stats.dispatch_count = 6};
    session_state.foreground_session = &session_state.sessions[0];
    port_shells[0] = (port_shell_state_t){.used = true, .running = true, .id = 16,
        .terminal_profile = 1, .port_name = "usb",
        .app_sessions[0] = {.used = true, .suspended = true, .id = 32, .app = &editor, .title = "remote"}};
    solar_os_native_session_info_v1_t entries[3];
    size_t total;
    assert(api->snapshot(SOLAR_OS_NATIVE_SESSION_ALL, NULL, 0, &total) == 0 && total == 4);
    assert(api->snapshot(SOLAR_OS_NATIVE_SESSION_ALL, entries, 3, &total) == 0 && total == 4);
    assert(entries[0].id == 0 && entries[0].kind == SOLAR_OS_NATIVE_SESSION_DISPLAY_SHELL);
    assert(entries[0].state == SOLAR_OS_NATIVE_SESSION_ACTIVE);
    assert(entries[1].id == 2 && entries[1].state == SOLAR_OS_NATIVE_SESSION_DETACHED &&
           entries[1].has_tick && entries[1].tick_stats.dispatch_count == 6);
    assert(entries[2].id == 16 && !strcmp(entries[2].terminal_profile, "vt100"));
    assert(api->find(0, &entries[0]) == 0 && !strcmp(entries[0].title, "console"));
    assert(api->find(32, &entries[1]) == 0 && entries[1].state == SOLAR_OS_NATIVE_SESSION_SUSPENDED &&
           entries[1].parent_id == 16 && !strcmp(entries[1].target, "usb"));
    port_shells[0].used = false;
    assert(!strcmp(entries[1].title, "remote"));
    assert(api->find(32, &entries[1]) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !entries[1].title[0]);
    assert(api->find(256, &entries[1]) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->snapshot(SOLAR_OS_NATIVE_SESSION_PORT_APP, entries, 3, &total) == 0 && total == 0);
    char target[16];
    assert(api->get_display_focus(target, 4) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !target[0]);
    assert(api->focus_display("display1") == 0);
    assert(api->get_display_focus(target, sizeof(target)) == 0 && !strcmp(target, "display1"));
    solar_os_native_shell_session_result_v1_t opened;
    solar_os_native_shell_session_request_v1_t request = {
        .kind = SOLAR_OS_NATIVE_SESSION_DISPLAY_SHELL, .target = "display1"};
    assert(api->open_shell(&request, &opened) == 0 && opened.id == 3 && !opened.busy_owner[0]);
    request.target = "busy";
    assert(api->open_shell(&request, &opened) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE &&
           !opened.id && !strcmp(opened.busy_owner, "other-session"));
    request = (solar_os_native_shell_session_request_v1_t){
        .kind = SOLAR_OS_NATIVE_SESSION_PORT_SHELL, .target = "usb", .run_startup = true,
        .terminal_profile = "vt100", .charset = "ascii", .columns = 100, .rows = 35};
    assert(api->open_shell(&request, &opened) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !opened.id);
    session_state.ctx = &base_context;
    assert(api->open_shell(&request, &opened) == 0 && opened.id == 16);
    app_scope = true;
    assert(api->open_shell(&request, &opened) == 0 && opened.id == 16);
    request.charset = "unknown";
    assert(api->open_shell(&request, &opened) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !opened.id);
    assert(api->resize_port(16, 100, 35) == 0);
    assert(api->resize_port(256, 100, 35) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->set_display_preference("display1", SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_SCALE, 3, true) == 0);
    assert(api->set_terminal_preference(SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE, 1) == 0);
    using_port = true;
    assert(api->set_terminal_preference(SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE, 1) == 0);
    assert(api->set_terminal_preference(SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->application_uses_display("display1"));
    using_port = false;
    assert(api->application_uses_display("display1") && !api->application_uses_display("display0"));
    app_scope = false;
    assert(!api->application_uses_display("display1"));
    uint32_t active = 255;
    assert(api->active_on_display("display0", &active) == 0 && active == 0);
    assert(api->active_on_display("display1", &active) == 0 && active == 2);
    session_state.sessions[2].suspended = true;
    assert(api->active_on_display("display1", &active) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !active);
    assert(api->active_on_display("", &active) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    bool moved = false;
    assert(!api->move_display_focus(SOLAR_OS_NATIVE_SESSION_FOCUS_DOWN, &moved) &&
        moved && last_axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL && !last_previous);
    assert(!api->move_display_focus(SOLAR_OS_NATIVE_SESSION_FOCUS_LEFT, &moved) &&
        !moved && last_axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL && last_previous);
    moved = true;
    assert(api->move_display_focus(257, &moved) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !moved);
    assert(!api->cycle_focus(true, &moved) && moved);
    assert(!api->cycle_focus(false, &moved) && !moved);
    assert(api->cycle_focus(true, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->submit_command(0, "status"));
    assert(api->submit_command(256, "status") == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->request_close(16) && closed_id == 16 && !deferred_id);
    app_scope = true;
    using_port = true;
    assert(!api->submit_command(0, "status"));
    assert(!api->request_close(3) && deferred_id == 3 && closed_id == 16);
    app_scope = false;
    using_port = false;
    assert(api->set_terminal_preference(SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    const char *args[] = {app_context.argv[1]};
    solar_os_native_display_app_request_v1_t launch = {.name = "editor", .display = "display0", .arguments = args, .argument_count = 1};
    solar_os_native_display_app_result_v1_t launched;
    strlcpy(app_context.argv[1], "notes.txt", sizeof(app_context.argv[1]));
    assert(!api->open_display_app(&launch, &launched) && launched.id == 4 && !launched.deferred && !launch_allocations);
    strlcpy(app_context.argv[1], "notes.txt", sizeof(app_context.argv[1]));
    fail_launch_allocation = true;
    assert(api->open_display_app(&launch, &launched) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !launch_allocations);
    fail_launch_allocation = false;
    launch.display = "busy";
    assert(api->open_display_app(&launch, &launched) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !launched.id);
    assert(!strcmp(launched.busy_owner, "other-session"));
    app_scope = true;
    strlcpy(app_context.argv[1], "notes.txt", sizeof(app_context.argv[1]));
    launch.display = "display0";
    assert(!api->open_display_app(&launch, &launched) && launched.id == 4);
    launch.display = "display1";
    strlcpy(app_context.argv[1], "notes.txt", sizeof(app_context.argv[1]));
    strlcpy(app_context.launch_cwd, "/explicit", sizeof(app_context.launch_cwd));
    launch.cwd = app_context.launch_cwd;
    assert(!api->open_display_app(&launch, &launched) && launched.deferred && !launched.id);
    assert(!strcmp(app_context.launch_cwd, "/explicit") && direct_launches == 3 && deferred_launches == 1);
    launch.argument_count = 0;
    assert(api->open_display_app(&launch, &launched) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !launched.deferred);
    solar_os_event_t text = {.type = SOLAR_OS_EVENT_TEXT, .data.text = "é"};
    caller_event = &text;
    bool delivered = false;
    assert(api->dispatch_input(2, &text, &delivered) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !delivered);
    assert(api->close_display("display1") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    using_port = true;
    assert(api->port_session_is_local(16) && api->port_session_is_local(32) && !api->port_session_is_local(33));
    assert(api->foreground_port_app(32) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->foreground_last_port_app(&active) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !active);
    shell_scope = true;
    assert(!api->foreground_port_app(32));
    assert(!api->foreground_last_port_app(&active) && active == 32 && port_switches == 2);
    app_scope = false;
    assert(!api->port_session_is_local(16));
    assert(!api->dispatch_input(2, &text, &delivered) && delivered);
    assert(!api->dispatch_input(1, &text, &delivered) && !delivered);
    memset(text.data.text, 'a', sizeof(text.data.text));
    assert(api->dispatch_input(2, &text, &delivered) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !delivered);
    text.type = SOLAR_OS_EVENT_TICK;
    assert(api->dispatch_input(2, &text, &delivered) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->close_display("display1"));
    assert(api->close_display("external") == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->stop_port_shell(16) && api->stop_port_shell(17) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->foreground_port_app(32) && port_switches == 3);
    shutting_down = true;
    text = (solar_os_event_t){.type = SOLAR_OS_EVENT_TEXT, .data.text = "é"};
    assert(api->dispatch_input(2, &text, &delivered) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !delivered);
    assert(api->cycle_focus(true, &moved) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !moved);
    assert(api->request_close(16) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->snapshot(SOLAR_OS_NATIVE_SESSION_ALL, NULL, 0, &total) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
}
''', "-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1", "-I", str(ROOT / "src"), "-I", str(source),
              "-I", str(ROOT / "src/shell"), "-I", str(ROOT / "src/apps"), "-I", str(ROOT / "components/u8g2/src/clib"),
              str(source / "solar_os_native_sessions.c"), "-fsanitize=address,undefined", "-no-pie")
