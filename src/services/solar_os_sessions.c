#include "solar_os_sessions.h"

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "solar_os_app_registry.h"
#include "solar_os_display.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_port_shell.h"
#include "solar_os_queue.h"
#include "solar_os_scheduler.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_terminal_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SOLAR_OS_SESSION_MAX 8
#define SOLAR_OS_SESSION_TITLE_MAX 48
#define SOLAR_OS_SESSION_OPERATION_QUEUE_LEN 2

static const char *TAG = "solar_os_sessions";

typedef struct {
    bool used;
    bool reserved;
    bool started;
    bool suspended;
    bool claimed;
    bool owns_terminal;
    bool owns_display_target;
    bool close_on_exit;
    bool has_return_session;
    bool preserve_terminal_on_resume;
    bool graphics_active;
    bool terminal_redraw_requested;
    uint8_t id;
    uint8_t return_session_id;
    int argc;
    uint32_t argv_hash;
    const solar_os_app_t *app;
    solar_os_terminal_t *terminal;
    solar_os_gfx_t *gfx;
    solar_os_gfx_snapshot_t *graphics_snapshot;
    solar_os_shell_io_t *io;
    solar_os_shell_session_t *shell_session;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char display_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    char title[SOLAR_OS_SESSION_TITLE_MAX];
    solar_os_tick_stats_t tick_stats;
} solar_os_session_entry_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_terminal_t *shell_terminal;
    solar_os_terminal_t *current_terminal;
    u8g2_t *display_u8g2;
    solar_os_gfx_t *default_gfx;
    solar_os_sessions_terminal_fn terminal_fn;
    solar_os_sessions_overlay_fn overlay_fn;
    void *user;
    const solar_os_app_t *foreground_app;
    bool foreground_app_claimed;
    solar_os_session_entry_t sessions[SOLAR_OS_SESSION_MAX];
    solar_os_session_entry_t *foreground_session;
    bool legacy_return_session_valid;
    uint8_t legacy_return_session_id;
    const solar_os_app_t *legacy_tick_app;
    solar_os_tick_stats_t legacy_tick_stats;
    char input_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} solar_os_session_state_t;

typedef struct {
    solar_os_terminal_t *terminal;
    solar_os_gfx_t *gfx;
    solar_os_shell_io_t *shell_io;
    solar_os_shell_session_t *shell_session;
    bool graphics_active;
} solar_os_session_context_snapshot_t;

typedef enum {
    SOLAR_OS_SESSION_OPERATION_CREATE_DISPLAY_APP = 0,
    SOLAR_OS_SESSION_OPERATION_CLOSE_SESSION,
    SOLAR_OS_SESSION_OPERATION_SEND_COMMAND,
} solar_os_session_operation_type_t;

typedef struct {
    solar_os_session_operation_type_t type;
    const solar_os_app_t *app;
    const char *target_name;
    int argc;
    char **argv;
    uint8_t close_session_id;
    uint8_t command_session_id;
    const char *command;
    solar_os_shell_io_t *caller_io;
    SemaphoreHandle_t complete;
    esp_err_t result;
    uint8_t session_id;
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
} solar_os_session_operation_request_t;

static solar_os_session_state_t session_state;
static portMUX_TYPE input_focus_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t session_operation_queue;
static TaskHandle_t session_scheduler_task;

static solar_os_session_entry_t *session_active_for_display(const char *target_name);
static esp_err_t session_create_display_app_internal(
    const solar_os_app_t *app,
    const char *target_name,
    int argc,
    char **argv,
    uint8_t *session_id,
    char *busy_owner,
    size_t busy_owner_len);
static esp_err_t session_close_internal(uint8_t session_id,
                                        bool reject_current_shell);
static esp_err_t session_send_command_internal(uint8_t session_id,
                                               const char *command,
                                               solar_os_shell_io_t *caller_io);

static const char *app_display_name(const solar_os_app_t *app)
{
    return app != NULL && app->name != NULL ? app->name : "?";
}

static bool app_is_resumable(const solar_os_app_t *app)
{
    return app != NULL && (app->flags & SOLAR_OS_APP_FLAG_RESUMABLE) != 0;
}

static bool app_is_shell_inline(const solar_os_app_t *app)
{
    return app != NULL && (app->flags & SOLAR_OS_APP_FLAG_SHELL_INLINE) != 0;
}

static void session_store_input_focus(const char *target_name)
{
    portENTER_CRITICAL(&input_focus_lock);
    strlcpy(session_state.input_target,
            target_name != NULL ? target_name : "",
            sizeof(session_state.input_target));
    portEXIT_CRITICAL(&input_focus_lock);
}

static bool session_copy_input_focus(char *target_name, size_t target_name_len)
{
    if (target_name == NULL || target_name_len == 0) {
        return false;
    }

    portENTER_CRITICAL(&input_focus_lock);
    strlcpy(target_name, session_state.input_target, target_name_len);
    portEXIT_CRITICAL(&input_focus_lock);
    return target_name[0] != '\0';
}

static void session_default_input_focus_for_u8g2(const u8g2_t *u8g2)
{
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (u8g2 == NULL ||
        session_copy_input_focus(current, sizeof(current)) ||
        !solar_os_display_target_name_for_u8g2(u8g2, target, sizeof(target))) {
        return;
    }
    session_store_input_focus(target);
}

static const solar_os_terminal_t *session_text_profile_source(void)
{
    return session_state.current_terminal != NULL ?
        session_state.current_terminal :
        session_state.shell_terminal;
}

static void session_inherit_status_bar(solar_os_terminal_t *terminal)
{
    const solar_os_terminal_t *source = session_text_profile_source();
    if (terminal == NULL || source == NULL || terminal == source) {
        return;
    }

    solar_os_status_bar_t status;
    solar_os_terminal_get_status_bar(source, &status);
    solar_os_terminal_set_status_bar(terminal, &status);
}

static bool session_apply_registered_display_profile(
    solar_os_terminal_t *terminal,
    const char *target_name,
    const u8g2_t *u8g2)
{
    if (terminal == NULL) {
        return false;
    }

    char resolved_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX] = {0};
    if (target_name != NULL && target_name[0] != '\0') {
        strlcpy(resolved_target, target_name, sizeof(resolved_target));
    } else {
        (void)solar_os_display_target_name_for_u8g2(
            u8g2, resolved_target, sizeof(resolved_target));
    }
    if (resolved_target[0] == '\0') {
        return false;
    }

    solar_os_terminal_profile_t profile;
    if (solar_os_display_get_terminal_profile(resolved_target, &profile) != ESP_OK) {
        return false;
    }
    return solar_os_terminal_apply_profile_transient(terminal, &profile) == ESP_OK;
}

static void set_current_terminal(solar_os_terminal_t *terminal)
{
    session_state.current_terminal = terminal;
    if (session_state.ctx != NULL) {
        session_state.ctx->terminal = terminal;
    }
    if (session_state.terminal_fn != NULL) {
        session_state.terminal_fn(terminal, session_state.user);
    }
}

static void session_restore_graphics_state(bool active)
{
    if (session_state.ctx == NULL) {
        return;
    }

    /*
     * solar_os_context_set_graphics_active(true) begins a graphics transition
     * and clears the selected framebuffer. Context switching must only restore
     * the session's saved state.
     */
    session_state.ctx->graphics_active = active;
}

static void session_context_capture(solar_os_session_context_snapshot_t *snapshot)
{
    if (snapshot == NULL || session_state.ctx == NULL) {
        return;
    }
    *snapshot = (solar_os_session_context_snapshot_t){
        .terminal = session_state.current_terminal,
        .gfx = solar_os_context_gfx(session_state.ctx),
        .shell_io = solar_os_context_shell_io(session_state.ctx),
        .shell_session = solar_os_context_shell_session(session_state.ctx),
        .graphics_active = solar_os_context_graphics_active(session_state.ctx),
    };
}

static void session_context_restore(const solar_os_session_context_snapshot_t *snapshot)
{
    if (snapshot == NULL || session_state.ctx == NULL) {
        return;
    }
    set_current_terminal(snapshot->terminal);
    solar_os_context_set_gfx(session_state.ctx, snapshot->gfx);
    solar_os_context_set_shell_io(session_state.ctx, snapshot->shell_io);
    solar_os_context_set_shell_session(session_state.ctx, snapshot->shell_session);
    session_restore_graphics_state(snapshot->graphics_active);
}

static bool launch_should_use_display_sessions(void)
{
    if (session_state.ctx == NULL) {
        return false;
    }

    solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
    if (io != NULL && solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        return false;
    }

    return session_state.shell_terminal != NULL || session_state.foreground_session != NULL;
}

static void session_owner_name(const solar_os_session_entry_t *session,
                               char *buffer,
                               size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (session == NULL) {
        strlcpy(buffer, "session", buffer_len);
        return;
    }
    snprintf(buffer, buffer_len, "session %u", (unsigned)session->id);
}

static void session_prepare_context(solar_os_session_entry_t *session)
{
    if (session == NULL || session_state.ctx == NULL) {
        return;
    }

    set_current_terminal(session->terminal);
    solar_os_context_set_gfx(session_state.ctx,
                             session->gfx != NULL ? session->gfx : session_state.default_gfx);
    session_restore_graphics_state(session->graphics_active);
    if (session->app == solar_os_shell_app() && session->shell_session != NULL) {
        solar_os_context_set_shell_session(session_state.ctx, session->shell_session);
        solar_os_context_set_shell_io(session_state.ctx,
                                      solar_os_shell_session_io(session->shell_session));
    } else if (session->io != NULL) {
        solar_os_context_set_shell_session(session_state.ctx, NULL);
        solar_os_context_set_shell_io(session_state.ctx, session->io);
    } else {
        solar_os_context_set_shell_io(session_state.ctx, NULL);
        solar_os_context_set_shell_session(session_state.ctx, NULL);
    }
}

static solar_os_shell_io_t *session_shell_io(const solar_os_session_entry_t *session)
{
    if (session == NULL) {
        return NULL;
    }
    if (session->app == solar_os_shell_app() && session->shell_session != NULL) {
        return solar_os_shell_session_io(session->shell_session);
    }
    return session->io;
}

static bool session_owns_current_context(const solar_os_session_entry_t *session)
{
    if (session == NULL || session_state.ctx == NULL) {
        return false;
    }
    if (session->app == solar_os_shell_app() && session->shell_session != NULL) {
        return solar_os_context_shell_session(session_state.ctx) == session->shell_session;
    }
    if (session->io != NULL) {
        return solar_os_context_shell_io(session_state.ctx) == session->io;
    }
    return session->terminal != NULL && session_state.current_terminal == session->terminal;
}

static void session_restore_base_context(void)
{
    set_current_terminal(NULL);
    solar_os_context_set_gfx(session_state.ctx, session_state.default_gfx);
    solar_os_context_set_shell_io(session_state.ctx, NULL);
    solar_os_context_set_shell_session(session_state.ctx, NULL);
}

static void restore_foreground_context(void)
{
    if (session_state.foreground_session != NULL) {
        session_prepare_context(session_state.foreground_session);
    } else {
        /* A detached close may have just freed the current session terminal. */
        session_restore_base_context();
    }
}

static void session_request_text_present_mode(const solar_os_session_entry_t *session)
{
    if (session_state.ctx == NULL ||
        (session != NULL ?
            session->graphics_active :
            solar_os_context_graphics_active(session_state.ctx))) {
        return;
    }

    solar_os_terminal_t *terminal = session != NULL ? session->terminal : session_state.current_terminal;
    if (terminal != NULL && terminal->u8g2 != NULL) {
        (void)solar_os_display_request_present_mode(terminal->u8g2, SOLAR_OS_DISPLAY_PRESENT_TEXT);
    }
}

static void session_update_title(solar_os_session_entry_t *session)
{
    if (session == NULL || session->app == NULL) {
        return;
    }

    if (session->app == solar_os_shell_app() &&
        session->owns_display_target &&
        session->display_target[0] != '\0') {
        snprintf(session->title,
                 sizeof(session->title),
                 "shell on %s",
                 session->display_target);
        return;
    }

    if (session->started && session->app->title != NULL) {
        session_prepare_context(session);
        session->title[0] = '\0';
        session->app->title(session_state.ctx, session->title, sizeof(session->title));
    }
    if (session->title[0] == '\0') {
        strlcpy(session->title, app_display_name(session->app), sizeof(session->title));
    }
    restore_foreground_context();
}

static solar_os_session_entry_t *session_by_id(uint8_t id)
{
    if (id >= SOLAR_OS_SESSION_MAX || !session_state.sessions[id].used) {
        return NULL;
    }
    return &session_state.sessions[id];
}

static solar_os_session_entry_t *session_return_target(uint8_t session_id,
                                                       const solar_os_session_entry_t *self)
{
    solar_os_session_entry_t *target = session_by_id(session_id);
    if (target == NULL || target == self || target->app == NULL) {
        return NULL;
    }
    return target;
}

static solar_os_session_entry_t *ensure_shell_session(void);

static solar_os_session_entry_t *session_return_shell(
    const solar_os_session_entry_t *session)
{
    const solar_os_session_entry_t *current = session;
    for (size_t depth = 0; depth < SOLAR_OS_SESSION_MAX; depth++) {
        if (current == NULL || !current->has_return_session) {
            break;
        }
        solar_os_session_entry_t *target =
            session_return_target(current->return_session_id, current);
        if (target == NULL) {
            break;
        }
        if (target->app == solar_os_shell_app()) {
            return target;
        }
        current = target;
    }
    return NULL;
}

static void session_transfer_terminal_output(solar_os_session_entry_t *session)
{
    if (session == NULL || session_state.ctx == NULL) {
        return;
    }

    const bool preserve_requested =
        solar_os_context_take_terminal_preserve(session_state.ctx);

    solar_os_session_entry_t *shell = session_return_shell(session);
    if (shell == NULL && session_state.shell_terminal != NULL &&
        session != &session_state.sessions[0]) {
        shell = ensure_shell_session();
    }
    if (shell == NULL || shell == session || shell->terminal == NULL) {
        return;
    }

    const bool shared_terminal = session->terminal == shell->terminal;
    const bool appended = !shared_terminal &&
        solar_os_terminal_append_text(shell->terminal, session->terminal);
    if (preserve_requested || shared_terminal || appended) {
        shell->preserve_terminal_on_resume = true;
    }
}

static bool switch_to_session(solar_os_session_entry_t *session, bool show_overlay);
static bool close_session(solar_os_session_entry_t *session, bool preserve_context);

static bool session_is_closable(const solar_os_session_entry_t *session)
{
    if (session == NULL || !session->used) {
        return false;
    }
    return session->app != solar_os_shell_app() || session->owns_display_target;
}

static void session_bind_builtin_display(solar_os_session_entry_t *session)
{
    if (session == NULL) {
        return;
    }

    session->display_target[0] = '\0';
    if (session_state.display_u8g2 != NULL) {
        (void)solar_os_display_target_name_for_u8g2(
            session_state.display_u8g2,
            session->display_target,
            sizeof(session->display_target));
    }
}

static solar_os_session_entry_t *ensure_shell_session(void)
{
    solar_os_session_entry_t *session = &session_state.sessions[0];
    if (!session->used) {
        memset(session, 0, sizeof(*session));
        session->used = true;
        session->id = 0;
        session->app = solar_os_shell_app();
        session->terminal = session_state.shell_terminal;
        session->gfx = session_state.default_gfx;
        session_bind_builtin_display(session);
        strlcpy(session->title, "shell", sizeof(session->title));
    } else if (session->app == solar_os_shell_app()) {
        session->terminal = session_state.shell_terminal;
        session->gfx = session_state.default_gfx;
        session_bind_builtin_display(session);
    }
    return session;
}

static bool switch_to_session_or_shell(solar_os_session_entry_t *session)
{
    if (session != NULL && session->used && session->app != NULL) {
        return switch_to_session(session, false);
    }
    if (session_state.shell_terminal == NULL) {
        return false;
    }
    return switch_to_session(ensure_shell_session(), false);
}

static solar_os_session_entry_t *session_find_by_app(const solar_os_app_t *app)
{
    if (app == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        if (session_state.sessions[i].used && session_state.sessions[i].app == app) {
            return &session_state.sessions[i];
        }
    }
    return NULL;
}

static uint32_t session_context_argv_hash(const solar_os_context_t *ctx)
{
    uint32_t hash = 2166136261UL;
    if (ctx == NULL) {
        return hash;
    }

    const int argc = solar_os_context_argc(ctx);
    hash ^= (uint32_t)argc;
    hash *= 16777619UL;
    for (int i = 0; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (arg == NULL) {
            arg = "";
        }
        for (const unsigned char *p = (const unsigned char *)arg; *p != '\0'; p++) {
            hash ^= (uint32_t)*p;
            hash *= 16777619UL;
        }
        hash ^= 0xffU;
        hash *= 16777619UL;
    }
    return hash;
}

static bool session_args_match_context(const solar_os_session_entry_t *session,
                                       const solar_os_context_t *ctx)
{
    return session != NULL &&
        ctx != NULL &&
        session->argc == solar_os_context_argc(ctx) &&
        session->argv_hash == session_context_argv_hash(ctx);
}

static void session_store_context_args(solar_os_session_entry_t *session,
                                       const solar_os_context_t *ctx)
{
    if (session == NULL || ctx == NULL) {
        return;
    }

    session->argc = solar_os_context_argc(ctx);
    session->argv_hash = session_context_argv_hash(ctx);
}

static solar_os_session_entry_t *session_alloc_from(const solar_os_app_t *app, size_t start_index)
{
    if (app == NULL) {
        return NULL;
    }

    for (size_t i = start_index; i < SOLAR_OS_SESSION_MAX; i++) {
        if (session_state.sessions[i].used || session_state.sessions[i].reserved) {
            continue;
        }
        solar_os_session_entry_t *session = &session_state.sessions[i];
        memset(session, 0, sizeof(*session));
        session->reserved = true;
        session->id = (uint8_t)i;
        session->app = app;
        strlcpy(session->title, app_display_name(app), sizeof(session->title));
        return session;
    }
    return NULL;
}

static solar_os_session_entry_t *session_alloc(const solar_os_app_t *app)
{
    return session_alloc_from(app, 0);
}

static bool session_is_allocated(const solar_os_session_entry_t *session)
{
    return session != NULL && (session->used || session->reserved);
}

static void session_publish(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->reserved) {
        return;
    }
    session->used = true;
    session->reserved = false;
}

static solar_os_gfx_t *session_effective_gfx(const solar_os_session_entry_t *session)
{
    return session != NULL && session->gfx != NULL ?
        session->gfx :
        session_state.default_gfx;
}

static void session_discard_graphics_snapshot(solar_os_session_entry_t *session)
{
    if (session == NULL || session->graphics_snapshot == NULL) {
        return;
    }
    solar_os_gfx_snapshot_destroy(session->graphics_snapshot);
    session->graphics_snapshot = NULL;
}

static void session_capture_graphics_snapshot(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->graphics_active || session->app == NULL ||
        session->app->resume != NULL) {
        session_discard_graphics_snapshot(session);
        return;
    }

    const esp_err_t err = solar_os_gfx_snapshot_capture(
        session_effective_gfx(session), &session->graphics_snapshot);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "session %u graphics snapshot failed: %s",
                      (unsigned)session->id,
                      esp_err_to_name(err));
    }
}

static void session_restore_graphics_snapshot(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->graphics_active ||
        session->graphics_snapshot == NULL) {
        return;
    }

    const esp_err_t err = solar_os_gfx_snapshot_restore(
        session_effective_gfx(session), session->graphics_snapshot);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "session %u graphics restore failed: %s",
                      (unsigned)session->id,
                      esp_err_to_name(err));
    }
}

static void session_bind_display(solar_os_session_entry_t *session,
                                 const solar_os_session_entry_t *parent)
{
    if (session == NULL) {
        return;
    }

    session->gfx = session_effective_gfx(parent);
    if (parent != NULL && parent->display_target[0] != '\0') {
        strlcpy(session->display_target,
                parent->display_target,
                sizeof(session->display_target));
    }
}

static bool session_uses_same_display(const solar_os_session_entry_t *left,
                                      const solar_os_session_entry_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    return session_effective_gfx(left) == session_effective_gfx(right) &&
        strcmp(left->display_target, right->display_target) == 0;
}

static solar_os_session_entry_t *session_find_by_terminal(const solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        if (session_state.sessions[i].used &&
            session_state.sessions[i].terminal == terminal) {
            return &session_state.sessions[i];
        }
    }
    return NULL;
}

static bool session_terminal_setting_is_persistent(const solar_os_session_entry_t *session)
{
    return session == NULL ||
        session_effective_gfx(session) == session_state.default_gfx;
}

static void session_set_gfx_palette_inverted(const solar_os_session_entry_t *session,
                                             bool inverted)
{
    solar_os_gfx_set_palette_inverted(session_effective_gfx(session), inverted);

    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX] = {0};
    if (session != NULL && session->display_target[0] != '\0') {
        strlcpy(target_name, session->display_target, sizeof(target_name));
    } else {
        (void)solar_os_display_target_name_for_u8g2(session_state.display_u8g2,
                                                    target_name,
                                                    sizeof(target_name));
    }
    if (target_name[0] != '\0') {
        (void)solar_os_display_set_palette_inverted(target_name, inverted);
    }
}

static const u8g2_cb_t *session_terminal_base_rotation(
    const char *target_name,
    const u8g2_t *u8g2)
{
    solar_os_display_target_t target;
    if (target_name != NULL && target_name[0] != '\0' &&
        solar_os_display_find_target(target_name, &target)) {
        return target.base_rotation;
    }

    char resolved_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (solar_os_display_target_name_for_u8g2(
            u8g2, resolved_target, sizeof(resolved_target)) &&
        solar_os_display_find_target(resolved_target, &target)) {
        return target.base_rotation;
    }
    return u8g2 != NULL ? u8g2->cb : U8G2_R0;
}

static void session_free_terminal(solar_os_session_entry_t *session)
{
    if (session == NULL) {
        return;
    }

    session_discard_graphics_snapshot(session);
    if (session->io != NULL) {
        solar_os_memory_free(session->io);
        session->io = NULL;
    }
    if (session->owns_terminal && session->terminal != NULL) {
        solar_os_terminal_deinit(session->terminal);
        solar_os_memory_free(session->terminal);
        session->terminal = NULL;
        session->owns_terminal = false;
    }
}

static void session_free_shell_session(solar_os_session_entry_t *session)
{
    if (session == NULL || session->shell_session == NULL) {
        return;
    }

    if (session_state.ctx != NULL) {
        solar_os_context_detach_shell_session(session_state.ctx, session->shell_session);
    }
    solar_os_shell_session_destroy(session->shell_session);
    session->shell_session = NULL;
}

static esp_err_t session_ensure_terminal(solar_os_session_entry_t *session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->terminal != NULL) {
        if (session->app != solar_os_shell_app() &&
            session->io == NULL) {
            session->io =
                solar_os_memory_calloc(1,
                                       sizeof(*session->io),
                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "session.io");
            if (session->io == NULL) {
                return ESP_ERR_NO_MEM;
            }
            solar_os_shell_io_init_terminal(session->io, session->terminal);
        }
        return ESP_OK;
    }
    if (session->app == solar_os_shell_app()) {
        session->terminal = session_state.shell_terminal;
        return session->terminal != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    u8g2_t *u8g2 = session_state.display_u8g2;
    bool black_is_one = false;
    if (session->display_target[0] != '\0') {
        solar_os_display_target_t target;
        if (!solar_os_display_find_target(session->display_target, &target) ||
            !target.ready ||
            target.u8g2 == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        u8g2 = target.u8g2;
        black_is_one = target.black_is_one;
    }
    if (u8g2 == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_terminal_t *session_terminal =
        solar_os_memory_calloc(1,
                               sizeof(*session_terminal),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "session.terminal");
    if (session_terminal == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_terminal_init_with_rotation(
        session_terminal,
        u8g2,
        session_terminal_base_rotation(session->display_target, u8g2));
    if (!session_apply_registered_display_profile(
            session_terminal, session->display_target, u8g2)) {
        solar_os_terminal_inherit_text_profile(session_terminal, session_text_profile_source());
        (void)solar_os_terminal_set_orientation_transient(session_terminal, 0);
    }
    session_inherit_status_bar(session_terminal);
    solar_os_terminal_set_black_is_one(session_terminal, black_is_one);

    solar_os_shell_io_t *session_io =
        solar_os_memory_calloc(1,
                               sizeof(*session_io),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "session.io");
    if (session_io == NULL) {
        solar_os_terminal_deinit(session_terminal);
        solar_os_memory_free(session_terminal);
        return ESP_ERR_NO_MEM;
    }
    solar_os_shell_io_init_terminal(session_io, session_terminal);

    session->terminal = session_terminal;
    session->owns_terminal = true;
    session->io = session_io;
    session_set_gfx_palette_inverted(
        session,
        solar_os_terminal_palette_inverted(session_terminal));
    return ESP_OK;
}

static void session_mark_dirty(solar_os_session_entry_t *session)
{
    if (session != NULL && session->terminal != NULL) {
        solar_os_terminal_invalidate_render(session->terminal);
    }
}

static void session_draw_terminal_if_needed(solar_os_session_entry_t *session)
{
    if (session != NULL &&
        session->used &&
        !session->suspended &&
        !session->graphics_active &&
        session->terminal != NULL &&
        solar_os_terminal_needs_draw(session->terminal)) {
        solar_os_terminal_draw(session->terminal);
    }
}

static bool session_claim_display(solar_os_session_entry_t *session)
{
    if (session == NULL || session->app == NULL || session->app == solar_os_shell_app()) {
        return true;
    }
    if (session->claimed) {
        return true;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
    session_owner_name(session, owner, sizeof(owner));
    const esp_err_t err =
        solar_os_app_registry_claim(session->app, owner, busy_owner, sizeof(busy_owner));
    if (err == ESP_OK) {
        session->claimed = solar_os_app_registry_find_by_app(session->app) != NULL;
        return true;
    }

    solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
    if (io != NULL && solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_printf(io,
                                 "%s: already running on %s\n",
                                 app_display_name(session->app),
                                 busy_owner[0] != '\0' ? busy_owner : "another session");
        solar_os_shell_io_flush(io);
    }
    return false;
}

static void session_release_display(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->claimed || session->app == NULL) {
        return;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    session_owner_name(session, owner, sizeof(owner));
    solar_os_app_registry_release(session->app, owner);
    session->claimed = false;
}

static void session_release_display_target(solar_os_session_entry_t *session)
{
    if (session == NULL ||
        !session->owns_display_target ||
        session->display_target[0] == '\0' ||
        session->display_owner[0] == '\0') {
        return;
    }

    (void)solar_os_display_release(session->display_target, session->display_owner);
    session->owns_display_target = false;
    session->display_target[0] = '\0';
    session->display_owner[0] = '\0';
    session->gfx = NULL;
}

static void session_dispose_unstarted(solar_os_session_entry_t *session)
{
    if (session == NULL) {
        return;
    }

    session_release_display(session);
    session_release_display_target(session);
    session_free_terminal(session);
    session_free_shell_session(session);
    memset(session, 0, sizeof(*session));
}

static bool display_claim_app(const solar_os_app_t *app, bool *claimed)
{
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];

    if (claimed != NULL) {
        *claimed = false;
    }

    const esp_err_t err =
        solar_os_app_registry_claim(app, "display", busy_owner, sizeof(busy_owner));
    if (err == ESP_OK) {
        if (claimed != NULL) {
            *claimed = solar_os_app_registry_find_by_app(app) != NULL;
        }
        return true;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
        if (io != NULL && solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_NONE) {
            solar_os_shell_io_printf(io,
                                     "%s: already running on %s\n",
                                     app != NULL && app->name != NULL ? app->name : "app",
                                     busy_owner[0] != '\0' ? busy_owner : "another session");
            solar_os_shell_io_flush(io);
        }
    } else {
        SOLAR_OS_LOGW(TAG,
                      "App %s claim failed: %s",
                      app != NULL && app->name != NULL ? app->name : "?",
                      esp_err_to_name(err));
    }
    return false;
}

static void display_release_app(const solar_os_app_t *app)
{
    if (!session_state.foreground_app_claimed || app == NULL) {
        return;
    }

    solar_os_app_registry_release(app, "display");
    session_state.foreground_app_claimed = false;
}

static void display_prompt_after_failed_launch(void)
{
    if (session_state.foreground_app != solar_os_shell_app() || session_state.ctx == NULL) {
        return;
    }

    solar_os_shell_session_t *session = solar_os_context_shell_session(session_state.ctx);
    if (session != NULL) {
        solar_os_shell_session_prompt(session_state.ctx, session);
    }
}

static void show_session_overlay(const solar_os_session_entry_t *session,
                                 bool after_next_frame)
{
    if (session == NULL || session->title[0] == '\0' || session_state.display_u8g2 == NULL) {
        return;
    }
    if (session_state.overlay_fn != NULL) {
        session_state.overlay_fn(session->title,
                                 after_next_frame,
                                 session_state.user);
    }
}

static void stop_legacy_foreground(void)
{
    if (session_state.foreground_app != NULL) {
        SOLAR_OS_LOGI(TAG, "stop app: %s", app_display_name(session_state.foreground_app));
        solar_os_app_stop(session_state.foreground_app, session_state.ctx);
    }
    display_release_app(session_state.foreground_app);
    solar_os_context_set_graphics_active(session_state.ctx, false);
    session_state.legacy_return_session_valid = false;
}

static void suspend_foreground_session(void)
{
    if (session_state.foreground_session == NULL) {
        stop_legacy_foreground();
        return;
    }

    solar_os_session_entry_t *session = session_state.foreground_session;
    session_prepare_context(session);
    if (session->app != NULL && session->app->suspend != NULL) {
        session->app->suspend(session_state.ctx);
    }
    session->graphics_active = solar_os_context_graphics_active(session_state.ctx);
    session_capture_graphics_snapshot(session);
    session->suspended = true;
    session_update_title(session);
}

static bool start_or_resume_session(solar_os_session_entry_t *session)
{
    if (session == NULL || session->app == NULL) {
        return false;
    }
    if (session_ensure_terminal(session) != ESP_OK || !session_claim_display(session)) {
        if (session->reserved) {
            session_dispose_unstarted(session);
        }
        return false;
    }

    const bool was_started = session->started;
    solar_os_shell_io_t *launch_io = solar_os_context_shell_io(session_state.ctx);
    session_prepare_context(session);
    if (session->preserve_terminal_on_resume) {
        session->preserve_terminal_on_resume = false;
        solar_os_context_request_terminal_preserve(session_state.ctx);
    }
    /* A resumed graphics session owns a retained frame. Do not tear down its
     * backing surface before an app without a resume renderer can restore it. */
    if (!was_started || !session->graphics_active) {
        solar_os_context_set_graphics_active(session_state.ctx, false);
    }

    if (!session->started) {
        session_store_context_args(session, session_state.ctx);
        if (session->app->start != NULL || session->app->state_size > 0U) {
            const esp_err_t app_err =
                solar_os_app_start(session->app, session_state.ctx);
            if (app_err != ESP_OK) {
                SOLAR_OS_LOGE(TAG,
                              "App %s failed to start: %s",
                              app_display_name(session->app),
                              esp_err_to_name(app_err));
                if (launch_io != NULL) {
                    solar_os_shell_io_printf(launch_io,
                                             "%s: start failed: %s\n",
                                             app_display_name(session->app),
                                             esp_err_to_name(app_err));
                    solar_os_shell_io_flush(launch_io);
                }
                session_dispose_unstarted(session);
                return false;
            }
        }
        session->started = true;
    } else if (session->app->resume != NULL) {
        session->app->resume(session_state.ctx);
    } else {
        session_restore_graphics_snapshot(session);
    }
    session->graphics_active = solar_os_context_graphics_active(session_state.ctx);
    session->terminal_redraw_requested = false;

    session_state.foreground_session = session;
    session_state.foreground_app = session->app;
    session_state.foreground_app_claimed = false;
    session->suspended = false;
    session_update_title(session);
    session_mark_dirty(session);
    session_request_text_present_mode(session);
    session_publish(session);
    return true;
}

static bool start_or_resume_detached_session(solar_os_session_entry_t *session)
{
    if (session == NULL || session->app == NULL) {
        return false;
    }

    solar_os_session_context_snapshot_t previous = {0};
    session_context_capture(&previous);
    solar_os_shell_io_t *launch_io = solar_os_context_shell_io(session_state.ctx);
    if (session_ensure_terminal(session) != ESP_OK || !session_claim_display(session)) {
        if (session->reserved) {
            session_dispose_unstarted(session);
        }
        session_context_restore(&previous);
        return false;
    }

    const bool was_started = session->started;
    session_prepare_context(session);
    if (session->preserve_terminal_on_resume) {
        session->preserve_terminal_on_resume = false;
        solar_os_context_request_terminal_preserve(session_state.ctx);
    }
    /* Keep the retained graphics backing alive across detached focus changes. */
    if (!was_started || !session->graphics_active) {
        solar_os_context_set_graphics_active(session_state.ctx, false);
    }
    if (!session->started) {
        session_store_context_args(session, session_state.ctx);
        if (session->app->start != NULL || session->app->state_size > 0U) {
            const esp_err_t app_err =
                solar_os_app_start(session->app, session_state.ctx);
            if (app_err != ESP_OK) {
                SOLAR_OS_LOGE(TAG,
                              "Detached app %s failed to start: %s",
                              app_display_name(session->app),
                              esp_err_to_name(app_err));
                if (launch_io != NULL) {
                    solar_os_shell_io_printf(launch_io,
                                             "%s: start failed: %s\n",
                                             app_display_name(session->app),
                                             esp_err_to_name(app_err));
                    solar_os_shell_io_flush(launch_io);
                }
                session_dispose_unstarted(session);
                session_context_restore(&previous);
                return false;
            }
        }
        session->started = true;
    } else if (session->app->resume != NULL) {
        session->app->resume(session_state.ctx);
    } else {
        session_restore_graphics_snapshot(session);
    }

    session->graphics_active = solar_os_context_graphics_active(session_state.ctx);
    session->terminal_redraw_requested = false;
    session->suspended = false;
    session_update_title(session);
    session_mark_dirty(session);
    session_request_text_present_mode(session);
    session_context_restore(&previous);
    session_publish(session);
    return true;
}

static void suspend_detached_session(solar_os_session_entry_t *session)
{
    if (session == NULL || session->suspended) {
        return;
    }

    solar_os_session_context_snapshot_t previous = {0};
    session_context_capture(&previous);
    session_prepare_context(session);
    if (session->app != NULL && session->app->suspend != NULL) {
        session->app->suspend(session_state.ctx);
    }
    session->graphics_active = solar_os_context_graphics_active(session_state.ctx);
    session_capture_graphics_snapshot(session);
    session->suspended = true;
    session_update_title(session);
    session_context_restore(&previous);
}

static bool switch_to_session(solar_os_session_entry_t *session, bool show_overlay)
{
    if (session == NULL || session->app == NULL) {
        return false;
    }
    if (session == session_state.foreground_session &&
        session_state.foreground_app == session->app) {
        return true;
    }

    SOLAR_OS_LOGI(TAG,
                  "switch session: %s -> %s",
                  session_state.foreground_app != NULL ?
                      app_display_name(session_state.foreground_app) : "(none)",
                  app_display_name(session->app));
    solar_os_session_entry_t *previous_session = session_state.foreground_session;
    suspend_foreground_session();
    if (show_overlay) {
        show_session_overlay(session, true);
    }
    if (!start_or_resume_session(session)) {
        if (previous_session != NULL && previous_session->used) {
            if (show_overlay) {
                show_session_overlay(previous_session, true);
            }
            (void)start_or_resume_session(previous_session);
        }
        return false;
    }
    return true;
}

static bool switch_to_display_app(const solar_os_app_t *app,
                                  solar_os_session_entry_t *parent,
                                  bool return_to_parent)
{
    if (app == NULL || app == solar_os_shell_app()) {
        return false;
    }

    solar_os_session_entry_t *session =
        app_is_resumable(app) ? session_find_by_app(app) : NULL;
    if (session != NULL &&
        parent != NULL &&
        !session_uses_same_display(session, parent)) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
        if (io != NULL) {
            solar_os_shell_io_printf(io,
                                     "%s: already belongs to session %u on another display\n",
                                     app_display_name(app),
                                     (unsigned)session->id);
            solar_os_shell_io_flush(io);
        }
        return false;
    }
    if (session != NULL &&
        session->started &&
        !session_args_match_context(session, session_state.ctx)) {
        (void)close_session(session, true);
        session = NULL;
    }

    if (session == NULL) {
        session = session_alloc(app);
        if (session == NULL) {
            SOLAR_OS_LOGW(TAG, "No free app session for %s", app_display_name(app));
            return false;
        }
        session_bind_display(session, parent);
        if (app_is_shell_inline(app) &&
            parent != NULL &&
            parent->app == solar_os_shell_app()) {
            session->terminal = parent->terminal;
        }
    }
    if (session == parent) {
        return true;
    }

    if (!app_is_resumable(app)) {
        session->close_on_exit = true;
    }
    if (return_to_parent && parent != NULL) {
        session->close_on_exit = true;
        session->has_return_session = true;
        session->return_session_id = parent->id;
    }
    return switch_to_session(session, false);
}

static bool switch_to_app(const solar_os_app_t *app)
{
    bool new_app_claimed = false;

    if (app == NULL) {
        return false;
    }
    if (app == session_state.foreground_app) {
        return true;
    }

    const bool use_display_session = launch_should_use_display_sessions();

    if (use_display_session && app == solar_os_shell_app()) {
        return switch_to_session(ensure_shell_session(), false);
    }

    if (use_display_session) {
        return switch_to_display_app(app, session_state.foreground_session, false);
    }

    if (app != solar_os_shell_app() && !display_claim_app(app, &new_app_claimed)) {
        return false;
    }

    SOLAR_OS_LOGI(TAG,
                  "switch app: %s -> %s",
                  session_state.foreground_app != NULL ?
                      app_display_name(session_state.foreground_app) : "(none)",
                  app_display_name(app));

    suspend_foreground_session();

    solar_os_context_set_graphics_active(session_state.ctx, false);
    session_state.foreground_session = NULL;
    session_state.foreground_app = app;
    session_state.foreground_app_claimed = new_app_claimed;
    if (session_state.foreground_app->start == NULL &&
        session_state.foreground_app->state_size == 0U) {
        return true;
    }

    const esp_err_t app_err = solar_os_app_start(session_state.foreground_app,
                                                 session_state.ctx);
    if (app_err == ESP_OK) {
        return true;
    }

    SOLAR_OS_LOGE(TAG,
                  "App %s failed to start: %s",
                  session_state.foreground_app->name,
                  esp_err_to_name(app_err));
    display_release_app(session_state.foreground_app);
    solar_os_context_set_graphics_active(session_state.ctx, false);
    session_state.foreground_app = NULL;
    session_state.foreground_session = NULL;
    solar_os_session_entry_t *return_session = session_state.legacy_return_session_valid ?
        session_return_target(session_state.legacy_return_session_id, NULL) :
        NULL;
    session_state.legacy_return_session_valid = false;
    (void)switch_to_session_or_shell(return_session);
    return false;
}

static bool switch_to_child_app(const solar_os_app_t *app)
{
    if (app == NULL) {
        return false;
    }
    if (!launch_should_use_display_sessions() ||
        session_state.foreground_session == NULL ||
        app == solar_os_shell_app()) {
        return switch_to_app(app);
    }

    solar_os_session_entry_t *parent = session_state.foreground_session;
    return switch_to_display_app(app, parent, true);
}

static bool close_session(solar_os_session_entry_t *session, bool preserve_context)
{
    if (!session_is_closable(session)) {
        return false;
    }

    const bool was_foreground = session == session_state.foreground_session;
    const uint8_t closing_id = session->id;
    solar_os_session_entry_t *return_session = was_foreground && session->has_return_session ?
        session_return_target(session->return_session_id, session) :
        NULL;
    solar_os_session_context_snapshot_t previous = {0};
    const bool preserve_caller_context =
        preserve_context && !session_owns_current_context(session);

    if (preserve_caller_context) {
        session_context_capture(&previous);
    }

    if (was_foreground) {
        session_state.foreground_session = NULL;
        session_state.foreground_app = NULL;
    }

    session_prepare_context(session);
    if (session->app != NULL) {
        SOLAR_OS_LOGI(TAG,
                      "close session %u: %s",
                      (unsigned)session->id,
                      app_display_name(session->app));
        solar_os_app_stop(session->app, session_state.ctx);
    }
    session_transfer_terminal_output(session);
    session_release_display(session);
    session_release_display_target(session);
    session_free_terminal(session);
    session_free_shell_session(session);
    memset(session, 0, sizeof(*session));

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        if (session_state.sessions[i].used &&
            session_state.sessions[i].has_return_session &&
            session_state.sessions[i].return_session_id == closing_id) {
            session_state.sessions[i].has_return_session = false;
        }
    }
    if (session_state.legacy_return_session_valid &&
        session_state.legacy_return_session_id == closing_id) {
        session_state.legacy_return_session_valid = false;
    }

    if (was_foreground) {
        if (preserve_caller_context) {
            session_context_restore(&previous);
            return true;
        }
        if (return_session != NULL) {
            return switch_to_session_or_shell(return_session);
        }
        if (session_state.shell_terminal != NULL) {
            return switch_to_session(ensure_shell_session(), false);
        }
        session_restore_base_context();
        return true;
    }
    if (preserve_caller_context) {
        session_context_restore(&previous);
        return true;
    }
    restore_foreground_context();
    return true;
}

static bool close_detached_session_and_resume(solar_os_session_entry_t *session)
{
    if (session == NULL || session == session_state.foreground_session) {
        return false;
    }

    solar_os_session_entry_t *return_session = session->has_return_session ?
        session_return_target(session->return_session_id, session) :
        NULL;
    session->has_return_session = false;
    if (!close_session(session, false)) {
        return false;
    }
    if (return_session != NULL &&
        return_session->used &&
        return_session->display_target[0] != '\0') {
        return start_or_resume_detached_session(return_session);
    }
    return true;
}

static bool launch_detached_child(solar_os_session_entry_t *parent,
                                  const solar_os_app_t *app,
                                  solar_os_launch_policy_t policy)
{
    (void)policy;
    if (parent == NULL ||
        app == NULL ||
        app == solar_os_shell_app() ||
        parent->display_target[0] == '\0') {
        return false;
    }

    solar_os_session_entry_t *child = session_alloc_from(app, 1);
    if (child == NULL) {
        SOLAR_OS_LOGW(TAG, "No free detached session for %s", app_display_name(app));
        return false;
    }
    strlcpy(child->display_target,
            parent->display_target,
            sizeof(child->display_target));
    child->gfx = parent->gfx;
    child->close_on_exit = true;
    child->has_return_session = true;
    child->return_session_id = parent->id;

    suspend_detached_session(parent);
    if (start_or_resume_detached_session(child)) {
        return true;
    }

    if (session_is_allocated(child)) {
        session_dispose_unstarted(child);
    }
    (void)start_or_resume_detached_session(parent);
    return false;
}

static void prompt_addressed_shell(solar_os_session_entry_t *session)
{
    if (session != NULL &&
        session->app == solar_os_shell_app() &&
        session->shell_session != NULL) {
        solar_os_shell_session_prompt(session_state.ctx, session->shell_session);
    }
}

static bool process_addressed_session_requests(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->used || session_state.ctx == NULL) {
        return false;
    }

    if (solar_os_context_take_sleep_request(session_state.ctx)) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
        if (io != NULL) {
            solar_os_shell_io_writeln(io,
                                      "sleep is unavailable from a detached display");
            solar_os_shell_io_flush(io);
        }
    }
    if (solar_os_context_take_suspend_request(session_state.ctx)) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
        if (io != NULL) {
            solar_os_shell_io_writeln(io,
                                      "suspend is unavailable from a detached display");
            solar_os_shell_io_flush(io);
        }
    }

    solar_os_session_request_type_t request = SOLAR_OS_SESSION_REQUEST_NONE;
    uint8_t requested_id = 0;
    while (solar_os_context_take_session_request(session_state.ctx,
                                                 &request,
                                                 &requested_id)) {
        (void)requested_id;
        solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);
        if (request == SOLAR_OS_SESSION_REQUEST_LIST) {
            solar_os_sessions_print_list(io, NULL);
        } else if (io != NULL) {
            solar_os_shell_io_writeln(
                io,
                "fg/close are unavailable from a detached display");
            solar_os_shell_io_flush(io);
        }
        prompt_addressed_shell(session);
    }

    if (solar_os_context_take_exit_request(session_state.ctx)) {
        if (session->app != solar_os_shell_app()) {
            return close_detached_session_and_resume(session);
        }
        return true;
    }

    const solar_os_app_t *requested_app =
        solar_os_context_take_launch_request(session_state.ctx);
    if (requested_app == NULL) {
        return true;
    }
    const solar_os_launch_policy_t policy =
        solar_os_context_take_launch_policy(session_state.ctx);
    if (!launch_detached_child(session, requested_app, policy)) {
        prompt_addressed_shell(session);
    }
    return true;
}

static void process_inactive_session_requests(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->used || session_state.ctx == NULL) {
        return;
    }

    if (solar_os_context_take_exit_request(session_state.ctx)) {
        (void)close_session(session, false);
        return;
    }

    const solar_os_app_t *requested_app =
        solar_os_context_take_launch_request(session_state.ctx);
    if (requested_app != NULL) {
        (void)solar_os_context_take_launch_policy(session_state.ctx);
        SOLAR_OS_LOGW(TAG,
                      "Ignoring launch from inactive session %u: %s",
                      (unsigned)session->id,
                      app_display_name(requested_app));
    }
    (void)solar_os_context_take_sleep_request(session_state.ctx);
    (void)solar_os_context_take_suspend_request(session_state.ctx);

    solar_os_session_request_type_t request = SOLAR_OS_SESSION_REQUEST_NONE;
    uint8_t requested_id = 0;
    while (solar_os_context_take_session_request(session_state.ctx,
                                                 &request,
                                                 &requested_id)) {
        (void)request;
        (void)requested_id;
    }
}

static void handle_session_request(void)
{
    solar_os_session_request_type_t request = SOLAR_OS_SESSION_REQUEST_NONE;
    uint8_t session_id = 0;

    while (solar_os_context_take_session_request(session_state.ctx, &request, &session_id)) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(session_state.ctx);

        switch (request) {
        case SOLAR_OS_SESSION_REQUEST_LIST:
            solar_os_sessions_print_list(io, NULL);
            solar_os_sessions_prompt_if_shell_active();
            break;
        case SOLAR_OS_SESSION_REQUEST_FG: {
            solar_os_session_entry_t *session = session_by_id(session_id);
            if (session == NULL) {
                if (io != NULL) {
                    solar_os_shell_io_printf(io,
                                             "fg: no such session: %u\n",
                                             (unsigned)session_id);
                    solar_os_shell_io_flush(io);
                }
                solar_os_sessions_prompt_if_shell_active();
                break;
            }
            if (switch_to_session(session, true)) {
                if (session->display_target[0] != '\0') {
                    session_store_input_focus(session->display_target);
                }
            } else if (io != NULL) {
                solar_os_shell_io_printf(io, "fg: failed: %u\n", (unsigned)session_id);
                solar_os_shell_io_flush(io);
                solar_os_sessions_prompt_if_shell_active();
            }
            break;
        }
        case SOLAR_OS_SESSION_REQUEST_CLOSE: {
            solar_os_session_entry_t *session = session_by_id(session_id);
            if (!session_is_closable(session)) {
                if (io != NULL) {
                    solar_os_shell_io_printf(io,
                                             "close: no such closable session: %u\n",
                                             (unsigned)session_id);
                    solar_os_shell_io_flush(io);
                }
                solar_os_sessions_prompt_if_shell_active();
                break;
            }
            if (close_session(session, true)) {
                if (io != NULL) {
                    solar_os_shell_io_printf(io,
                                             "closed session %u\n",
                                             (unsigned)session_id);
                    solar_os_shell_io_flush(io);
                }
            }
            solar_os_sessions_prompt_if_shell_active();
            break;
        }
        case SOLAR_OS_SESSION_REQUEST_NONE:
        default:
            break;
        }
    }
}

static solar_os_session_entry_t *session_next_in_ring(void)
{
    const uint8_t start = session_state.foreground_session != NULL ?
        session_state.foreground_session->id :
        0;
    for (size_t step = 1; step <= SOLAR_OS_SESSION_MAX; step++) {
        const uint8_t index = (uint8_t)((start + step) % SOLAR_OS_SESSION_MAX);
        if (session_state.sessions[index].used &&
            session_state.sessions[index].app != NULL) {
            return &session_state.sessions[index];
        }
    }
    return NULL;
}

static solar_os_session_entry_t *session_previous_in_ring(void)
{
    const uint8_t start = session_state.foreground_session != NULL ?
        session_state.foreground_session->id :
        0;
    for (size_t step = 1; step <= SOLAR_OS_SESSION_MAX; step++) {
        const uint8_t index = (uint8_t)((start + SOLAR_OS_SESSION_MAX - step) %
                                        SOLAR_OS_SESSION_MAX);
        if (session_state.sessions[index].used &&
            session_state.sessions[index].app != NULL) {
            return &session_state.sessions[index];
        }
    }
    return NULL;
}

static solar_os_session_entry_t *session_next_for_display(
    const char *target_name,
    const solar_os_session_entry_t *current)
{
    if (target_name == NULL || target_name[0] == '\0') {
        return NULL;
    }
    const uint8_t start = current != NULL ? current->id : 0;
    for (size_t step = 1; step <= SOLAR_OS_SESSION_MAX; step++) {
        const uint8_t index = (uint8_t)((start + step) % SOLAR_OS_SESSION_MAX);
        solar_os_session_entry_t *candidate = &session_state.sessions[index];
        if (candidate->used &&
            candidate->app != NULL &&
            strcmp(candidate->display_target, target_name) == 0) {
            return candidate;
        }
    }
    return NULL;
}

static solar_os_session_entry_t *session_previous_for_display(
    const char *target_name,
    const solar_os_session_entry_t *current)
{
    if (target_name == NULL || target_name[0] == '\0') {
        return NULL;
    }
    const uint8_t start = current != NULL ? current->id : 0;
    for (size_t step = 1; step <= SOLAR_OS_SESSION_MAX; step++) {
        const uint8_t index = (uint8_t)((start + SOLAR_OS_SESSION_MAX - step) %
                                        SOLAR_OS_SESSION_MAX);
        solar_os_session_entry_t *candidate = &session_state.sessions[index];
        if (candidate->used &&
            candidate->app != NULL &&
            strcmp(candidate->display_target, target_name) == 0) {
            return candidate;
        }
    }
    return NULL;
}

static bool switch_detached_display_session(solar_os_session_entry_t *current,
                                            solar_os_session_entry_t *next)
{
    if (current == NULL || next == NULL) {
        return false;
    }
    if (current == next) {
        return true;
    }

    suspend_detached_session(current);
    if (start_or_resume_detached_session(next)) {
        return true;
    }
    (void)start_or_resume_detached_session(current);
    return false;
}

static void dispatch_session_event(solar_os_session_entry_t *session,
                                   const solar_os_event_t *event)
{
    if (session == NULL || !session->used || session->suspended ||
        session->app == NULL || event == NULL) {
        return;
    }

    if (session->terminal_redraw_requested) {
        session->terminal_redraw_requested = false;
        session_prepare_context(session);
        if (session->app->resume != NULL) {
            session->app->resume(session_state.ctx);
        } else if (session->app->event != NULL) {
            const solar_os_event_t resume_event = {
                .type = SOLAR_OS_EVENT_RESUME,
            };
            session->app->event(session_state.ctx, &resume_event);
        }
        session->graphics_active = solar_os_context_graphics_active(session_state.ctx);
        if (session->app->resume == NULL) {
            session_restore_graphics_snapshot(session);
        }
    }
    if (session->app->event == NULL) {
        return;
    }

    const bool tick = event->type == SOLAR_OS_EVENT_TICK;
    if (tick &&
        !solar_os_tick_due(&session->tick_stats,
                           solar_os_app_tick_interval_ms(
                               session->app,
                               SOLAR_OS_TICK_INTERVAL_DEFAULT_MS),
                           session->app->tick_deadline_ms,
                           SOLAR_OS_TICK_INTERVAL_DEFAULT_MS,
                           SOLAR_OS_TICK_DEADLINE_DEFAULT_MS,
                           event->data.tick_ms)) {
        return;
    }

    session_prepare_context(session);
    const int64_t started_us = tick ? solar_os_tick_begin() : 0;
    session->app->event(session_state.ctx, event);
    session->graphics_active = solar_os_context_graphics_active(session_state.ctx);
    if (event->type == SOLAR_OS_EVENT_RESUME && session->app->resume == NULL) {
        session_restore_graphics_snapshot(session);
    }
    if (tick && solar_os_tick_end(&session->tick_stats, started_us) &&
        solar_os_tick_should_log_miss(&session->tick_stats)) {
        SOLAR_OS_LOGW(TAG,
                      "tick miss: #%u %s %" PRIu32 "us>%" PRIu32 "ms n=%" PRIu32,
                      (unsigned)session->id,
                      app_display_name(session->app),
                      session->tick_stats.last_duration_us,
                      session->tick_stats.deadline_ms,
                      session->tick_stats.deadline_miss_count);
    }
}

static void dispatch_legacy_event(const solar_os_event_t *event)
{
    const solar_os_app_t *app = session_state.foreground_app;
    if (app == NULL || app->event == NULL || event == NULL) {
        return;
    }
    if (session_state.legacy_tick_app != app) {
        session_state.legacy_tick_app = app;
        solar_os_tick_stats_reset(&session_state.legacy_tick_stats);
    }

    const bool tick = event->type == SOLAR_OS_EVENT_TICK;
    if (tick &&
        !solar_os_tick_due(&session_state.legacy_tick_stats,
                           solar_os_app_tick_interval_ms(
                               app,
                               SOLAR_OS_TICK_INTERVAL_DEFAULT_MS),
                           app->tick_deadline_ms,
                           SOLAR_OS_TICK_INTERVAL_DEFAULT_MS,
                           SOLAR_OS_TICK_DEADLINE_DEFAULT_MS,
                           event->data.tick_ms)) {
        return;
    }

    const int64_t started_us = tick ? solar_os_tick_begin() : 0;
    app->event(session_state.ctx, event);
    if (tick && solar_os_tick_end(&session_state.legacy_tick_stats, started_us) &&
        solar_os_tick_should_log_miss(&session_state.legacy_tick_stats)) {
        SOLAR_OS_LOGW(TAG,
                      "tick miss: %s %" PRIu32 "us>%" PRIu32 "ms n=%" PRIu32,
                      app_display_name(app),
                      session_state.legacy_tick_stats.last_duration_us,
                      session_state.legacy_tick_stats.deadline_ms,
                      session_state.legacy_tick_stats.deadline_miss_count);
    }
}

esp_err_t solar_os_sessions_init(solar_os_context_t *ctx,
                                 solar_os_terminal_t *shell_terminal,
                                 u8g2_t *display_u8g2,
                                 solar_os_sessions_terminal_fn terminal_fn,
                                 solar_os_sessions_overlay_fn overlay_fn,
                                 void *user)
{
    if (session_operation_queue == NULL) {
        session_operation_queue =
            solar_os_queue_create(SOLAR_OS_SESSION_OPERATION_QUEUE_LEN,
                                  sizeof(solar_os_session_operation_request_t *));
        if (session_operation_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&session_state, 0, sizeof(session_state));
    session_scheduler_task = xTaskGetCurrentTaskHandle();
    session_state.ctx = ctx;
    session_state.shell_terminal = shell_terminal;
    session_state.current_terminal = shell_terminal;
    session_state.display_u8g2 = display_u8g2;
    session_state.default_gfx = solar_os_context_gfx(ctx);
    session_state.terminal_fn = terminal_fn;
    session_state.overlay_fn = overlay_fn;
    session_state.user = user;
    session_set_gfx_palette_inverted(
        NULL,
        solar_os_terminal_palette_inverted(shell_terminal));
    session_default_input_focus_for_u8g2(display_u8g2);
    if (ctx != NULL) {
        solar_os_context_set_session_list_handler(ctx, solar_os_sessions_print_list, NULL);
    }
    return ESP_OK;
}

void solar_os_sessions_set_display(solar_os_terminal_t *shell_terminal, u8g2_t *display_u8g2)
{
    session_state.shell_terminal = shell_terminal;
    session_state.display_u8g2 = display_u8g2;
    session_state.default_gfx = solar_os_context_gfx(session_state.ctx);
    session_set_gfx_palette_inverted(
        NULL,
        solar_os_terminal_palette_inverted(shell_terminal));
    session_default_input_focus_for_u8g2(display_u8g2);
    if (session_state.sessions[0].used &&
        session_state.sessions[0].app == solar_os_shell_app()) {
        session_state.sessions[0].terminal = shell_terminal;
        session_state.sessions[0].gfx = session_state.default_gfx;
        session_bind_builtin_display(&session_state.sessions[0]);
    }
    if (session_state.current_terminal == NULL) {
        set_current_terminal(shell_terminal);
    }
}

const solar_os_app_t *solar_os_sessions_foreground_app(void)
{
    return session_state.foreground_app;
}

solar_os_terminal_t *solar_os_sessions_foreground_terminal(void)
{
    return session_state.current_terminal;
}

bool solar_os_sessions_foreground_is_shell(void)
{
    return session_state.foreground_app == solar_os_shell_app();
}

bool solar_os_sessions_has_display_shell(void)
{
    if (session_state.shell_terminal != NULL) {
        return true;
    }
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        const solar_os_session_entry_t *session = &session_state.sessions[i];
        if (session->used &&
            session->app == solar_os_shell_app() &&
            session->owns_display_target) {
            return true;
        }
    }
    return false;
}

void solar_os_sessions_set_status_bar(const solar_os_status_bar_t *status)
{
    if (status == NULL) {
        return;
    }

    solar_os_terminal_set_status_bar(session_state.shell_terminal, status);
    if (session_state.current_terminal != session_state.shell_terminal) {
        solar_os_terminal_set_status_bar(session_state.current_terminal, status);
    }
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_terminal_t *terminal = session_state.sessions[i].used ?
            session_state.sessions[i].terminal :
            NULL;
        if (terminal != session_state.shell_terminal &&
            terminal != session_state.current_terminal) {
            solar_os_terminal_set_status_bar(terminal, status);
        }
    }
}

static bool session_terminal_target_name(
    const solar_os_session_entry_t *session,
    const solar_os_terminal_t *terminal,
    char *target_name,
    size_t target_name_len)
{
    if (target_name == NULL || target_name_len == 0) {
        return false;
    }
    target_name[0] = '\0';
    if (session != NULL && session->display_target[0] != '\0') {
        strlcpy(target_name, session->display_target, target_name_len);
        return true;
    }
    return terminal != NULL &&
        solar_os_display_target_name_for_u8g2(
            terminal->u8g2, target_name, target_name_len);
}

static void session_store_terminal_profile(
    const solar_os_session_entry_t *session,
    const solar_os_terminal_t *terminal)
{
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (!session_terminal_target_name(
            session, terminal, target_name, sizeof(target_name))) {
        return;
    }

    solar_os_terminal_profile_t profile;
    solar_os_terminal_get_profile(terminal, &profile);
    (void)solar_os_display_set_terminal_profile(target_name, &profile);
}

static bool session_terminal_layout_changed(
    const solar_os_terminal_profile_t *before,
    const solar_os_terminal_profile_t *after)
{
    return before != NULL && after != NULL &&
        (before->orientation_degrees != after->orientation_degrees ||
         before->font != after->font ||
         before->text_size != after->text_size ||
         before->status_bar_visible != after->status_bar_visible);
}

esp_err_t solar_os_sessions_set_display_terminal_profile(
    const char *target_name,
    const solar_os_terminal_profile_t *profile)
{
    esp_err_t err = solar_os_display_set_terminal_profile(target_name, profile);
    if (err != ESP_OK) {
        return err;
    }

    bool shell_terminal_applied = false;
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *session = &session_state.sessions[i];
        char session_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
        if (!session->used || session->terminal == NULL ||
            !session_terminal_target_name(
                session, session->terminal, session_target, sizeof(session_target)) ||
            strcmp(session_target, target_name) != 0) {
            continue;
        }
        solar_os_terminal_profile_t previous_profile;
        solar_os_terminal_get_profile(session->terminal, &previous_profile);
        const esp_err_t apply_err = solar_os_terminal_apply_profile_transient(
            session->terminal, profile);
        if (err == ESP_OK && apply_err != ESP_OK) {
            err = apply_err;
        }
        if (apply_err == ESP_OK && session->started && !session->suspended &&
            !session->graphics_active && session->app != solar_os_shell_app() &&
            session_terminal_layout_changed(&previous_profile, profile)) {
            session->terminal_redraw_requested = true;
        }
        if (session->terminal == session_state.shell_terminal) {
            shell_terminal_applied = true;
        }
    }

    if (!shell_terminal_applied && session_state.shell_terminal != NULL) {
        char shell_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
        if (session_terminal_target_name(NULL,
                                         session_state.shell_terminal,
                                         shell_target,
                                         sizeof(shell_target)) &&
            strcmp(shell_target, target_name) == 0) {
            const esp_err_t apply_err = solar_os_terminal_apply_profile_transient(
                session_state.shell_terminal, profile);
            if (err == ESP_OK && apply_err != ESP_OK) {
                err = apply_err;
            }
        }
    }
    return err;
}

esp_err_t solar_os_sessions_set_terminal_orientation(solar_os_terminal_t *terminal,
                                                     uint16_t degrees)
{
    solar_os_session_entry_t *owner = session_find_by_terminal(terminal);
    const esp_err_t err = session_terminal_setting_is_persistent(owner) ?
        solar_os_terminal_set_orientation(terminal, degrees) :
        solar_os_terminal_set_orientation_transient(terminal, degrees);
    if (err == ESP_ERR_INVALID_ARG) {
        return err;
    }
    session_store_terminal_profile(owner, terminal);

    if (owner != NULL) {
        for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
            solar_os_session_entry_t *peer = &session_state.sessions[i];
            if (peer->used &&
                peer->terminal != NULL &&
                peer->terminal != terminal &&
                session_uses_same_display(owner, peer)) {
                (void)solar_os_terminal_set_orientation_transient(peer->terminal, degrees);
            }
        }
    }
    return err;
}

esp_err_t solar_os_sessions_set_terminal_font(solar_os_terminal_t *terminal,
                                              solar_os_terminal_font_t font)
{
    solar_os_session_entry_t *owner = session_find_by_terminal(terminal);
    const esp_err_t err = session_terminal_setting_is_persistent(owner) ?
        solar_os_terminal_set_font(terminal, font) :
        solar_os_terminal_set_font_transient(terminal, font);
    if (err == ESP_ERR_INVALID_ARG) {
        return err;
    }
    session_store_terminal_profile(owner, terminal);

    if (owner != NULL) {
        for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
            solar_os_session_entry_t *peer = &session_state.sessions[i];
            if (peer->used &&
                peer->terminal != NULL &&
                peer->terminal != terminal &&
                session_uses_same_display(owner, peer)) {
                (void)solar_os_terminal_set_font_transient(peer->terminal, font);
            }
        }
    }
    return err;
}

esp_err_t solar_os_sessions_set_terminal_text_size(solar_os_terminal_t *terminal,
                                                   solar_os_terminal_text_size_t text_size)
{
    solar_os_session_entry_t *owner = session_find_by_terminal(terminal);
    const esp_err_t err = session_terminal_setting_is_persistent(owner) ?
        solar_os_terminal_set_text_size(terminal, text_size) :
        solar_os_terminal_set_text_size_transient(terminal, text_size);
    if (err == ESP_ERR_INVALID_ARG) {
        return err;
    }
    session_store_terminal_profile(owner, terminal);

    if (owner != NULL) {
        for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
            solar_os_session_entry_t *peer = &session_state.sessions[i];
            if (peer->used &&
                peer->terminal != NULL &&
                peer->terminal != terminal &&
                session_uses_same_display(owner, peer)) {
                (void)solar_os_terminal_set_text_size_transient(peer->terminal, text_size);
            }
        }
    }
    return err;
}

esp_err_t solar_os_sessions_set_terminal_palette_inverted(solar_os_terminal_t *terminal,
                                                          bool inverted)
{
    if (terminal == NULL) {
        return solar_os_terminal_set_palette_preference(inverted);
    }

    solar_os_session_entry_t *owner = session_find_by_terminal(terminal);
    const esp_err_t err = session_terminal_setting_is_persistent(owner) ?
        solar_os_terminal_set_palette_inverted(terminal, inverted) :
        solar_os_terminal_set_palette_inverted_transient(terminal, inverted);
    if (err == ESP_ERR_INVALID_ARG) {
        return err;
    }
    session_store_terminal_profile(owner, terminal);
    session_set_gfx_palette_inverted(owner, inverted);

    if (owner != NULL) {
        for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
            solar_os_session_entry_t *peer = &session_state.sessions[i];
            if (peer->used &&
                peer->terminal != NULL &&
                peer->terminal != terminal &&
                session_uses_same_display(owner, peer)) {
                (void)solar_os_terminal_set_palette_inverted_transient(peer->terminal,
                                                                       inverted);
            }
        }
    }
    return err;
}

esp_err_t solar_os_sessions_set_terminal_status_bar_visible(solar_os_terminal_t *terminal,
                                                            bool visible)
{
    if (terminal == NULL) {
        return solar_os_terminal_set_status_bar_preference(visible);
    }

    solar_os_session_entry_t *owner = session_find_by_terminal(terminal);
    const esp_err_t err = session_terminal_setting_is_persistent(owner) ?
        solar_os_terminal_set_status_bar_visible(terminal, visible) :
        solar_os_terminal_set_status_bar_visible_transient(terminal, visible);
    if (err == ESP_ERR_INVALID_ARG) {
        return err;
    }
    session_store_terminal_profile(owner, terminal);

    if (owner != NULL) {
        for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
            solar_os_session_entry_t *peer = &session_state.sessions[i];
            if (peer->used &&
                peer->terminal != NULL &&
                peer->terminal != terminal &&
                session_uses_same_display(owner, peer)) {
                (void)solar_os_terminal_set_status_bar_visible_transient(peer->terminal,
                                                                         visible);
            }
        }
    }
    return err;
}

static solar_os_session_entry_t *session_active_for_display(const char *target_name)
{
    if (target_name == NULL || target_name[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *session = &session_state.sessions[i];
        if (session->used &&
            !session->suspended &&
            session->app != NULL &&
            strcmp(session->display_target, target_name) == 0) {
            return session;
        }
    }
    return NULL;
}

bool solar_os_sessions_active_for_display(const char *target_name, uint8_t *session_id)
{
    if (session_id == NULL) {
        return false;
    }
    solar_os_session_entry_t *session = session_active_for_display(target_name);
    if (session == NULL) {
        return false;
    }
    *session_id = session->id;
    return true;
}

bool solar_os_sessions_display_accepts_pointer_events(const char *target_name)
{
    solar_os_session_entry_t *session = session_active_for_display(target_name);
    return session != NULL && session->app != NULL &&
        (session->app->flags & SOLAR_OS_APP_FLAG_POINTER_EVENTS) != 0;
}

bool solar_os_sessions_context_uses_display(solar_os_context_t *ctx,
                                            const char *target_name)
{
    if (ctx == NULL || target_name == NULL || target_name[0] == '\0') {
        return false;
    }

    solar_os_display_target_t target;
    solar_os_terminal_t *terminal = solar_os_context_terminal(ctx);
    return terminal != NULL &&
        solar_os_display_find_target(target_name, &target) &&
        target.u8g2 != NULL &&
        terminal->u8g2 == target.u8g2;
}

esp_err_t solar_os_sessions_focus_display(const char *target_name)
{
    if (target_name == NULL || target_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(target_name, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!target.ready || target.u8g2 == NULL ||
        session_active_for_display(target.name) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    session_store_input_focus(target.name);
    return ESP_OK;
}

bool solar_os_sessions_input_focus(char *target_name, size_t target_name_len)
{
    return session_copy_input_focus(target_name, target_name_len);
}

bool solar_os_sessions_switch_to_app(const solar_os_app_t *app)
{
    return switch_to_app(app);
}

bool solar_os_sessions_switch_to_app_with_policy(const solar_os_app_t *app,
                                                 solar_os_launch_policy_t policy)
{
    if (policy == SOLAR_OS_LAUNCH_CHILD_RETURN) {
        return switch_to_child_app(app);
    }

    session_state.legacy_return_session_valid = false;
    return switch_to_app(app);
}

void solar_os_sessions_cycle_next(void)
{
    solar_os_session_entry_t *next = session_next_in_ring();
    if (next != NULL) {
        (void)switch_to_session(next, true);
    }
}

bool solar_os_sessions_cycle_input_focus(void)
{
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (!session_copy_input_focus(target_name, sizeof(target_name))) {
        solar_os_sessions_cycle_next();
        return session_state.foreground_session != NULL;
    }

    solar_os_session_entry_t *current = session_active_for_display(target_name);
    solar_os_session_entry_t *next =
        session_next_for_display(target_name, current);
    if (current == NULL || next == NULL) {
        return false;
    }
    if (current == session_state.foreground_session) {
        return next == current || switch_to_session(next, true);
    }
    return switch_detached_display_session(current, next);
}

bool solar_os_sessions_cycle_input_focus_previous(void)
{
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (!session_copy_input_focus(target_name, sizeof(target_name))) {
        solar_os_session_entry_t *previous = session_previous_in_ring();
        return previous != NULL && switch_to_session(previous, true);
    }

    solar_os_session_entry_t *current = session_active_for_display(target_name);
    solar_os_session_entry_t *previous =
        session_previous_for_display(target_name, current);
    if (current == NULL || previous == NULL) {
        return false;
    }
    if (current == session_state.foreground_session) {
        return previous == current || switch_to_session(previous, true);
    }
    return switch_detached_display_session(current, previous);
}

void solar_os_sessions_show_input_focus_overlay(void)
{
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    solar_os_session_entry_t *session = session_state.foreground_session;
    if (session_copy_input_focus(target_name, sizeof(target_name))) {
        solar_os_session_entry_t *focused =
            session_active_for_display(target_name);
        if (focused != NULL) {
            session = focused;
        }
    }
    show_session_overlay(session, false);
}

void solar_os_sessions_mark_foreground_dirty(void)
{
    if (session_state.foreground_session != NULL) {
        session_mark_dirty(session_state.foreground_session);
    } else if (session_state.current_terminal != NULL) {
        solar_os_terminal_invalidate_render(session_state.current_terminal);
    }
}

void solar_os_sessions_dispatch_foreground_event(const solar_os_event_t *event)
{
    if (event == NULL) {
        return;
    }
    if (session_state.foreground_session != NULL) {
        dispatch_session_event(session_state.foreground_session, event);
        restore_foreground_context();
        return;
    }
    if (session_state.foreground_app != NULL && session_state.foreground_app->event != NULL) {
        dispatch_legacy_event(event);
    }
}

const solar_os_app_t *solar_os_sessions_input_app(void)
{
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (!session_copy_input_focus(target_name, sizeof(target_name))) {
        return session_state.foreground_app;
    }
    solar_os_session_entry_t *session = session_active_for_display(target_name);
    return session != NULL ? session->app : NULL;
}

bool solar_os_sessions_dispatch_input_event(const solar_os_event_t *event)
{
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    if (event == NULL) {
        return false;
    }
    if (!session_copy_input_focus(target_name, sizeof(target_name))) {
        solar_os_sessions_dispatch_foreground_event(event);
        return session_state.foreground_app != NULL;
    }

    solar_os_session_entry_t *session = session_active_for_display(target_name);
    return session != NULL &&
        solar_os_sessions_dispatch_session_event(session->id, event);
}

bool solar_os_sessions_dispatch_session_event(uint8_t session_id,
                                              const solar_os_event_t *event)
{
    solar_os_session_entry_t *session = session_by_id(session_id);
    if (session == NULL || session->suspended || event == NULL) {
        return false;
    }
    if (session == session_state.foreground_session) {
        solar_os_sessions_dispatch_foreground_event(event);
        solar_os_sessions_process_requests();
        return true;
    }

    solar_os_session_context_snapshot_t previous = {0};
    solar_os_session_entry_t *previous_foreground = session_state.foreground_session;
    session_context_capture(&previous);
    dispatch_session_event(session, event);
    (void)process_addressed_session_requests(session);
    session_draw_terminal_if_needed(session);
    if (session_state.foreground_session == previous_foreground &&
        (previous_foreground == NULL || previous_foreground->used)) {
        session_context_restore(&previous);
    } else if (session_state.foreground_session != NULL) {
        restore_foreground_context();
    } else {
        session_restore_base_context();
    }
    return true;
}

static esp_err_t session_send_command_internal(uint8_t session_id,
                                               const char *command,
                                               solar_os_shell_io_t *caller_io)
{
    if (command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_session_entry_t *session = session_by_id(session_id);
    if (session == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (session->app != solar_os_shell_app() || session->shell_session == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (session->suspended || !session->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (caller_io != NULL && caller_io == session_shell_io(session)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    solar_os_session_context_snapshot_t previous = {0};
    solar_os_session_entry_t *previous_foreground = session_state.foreground_session;
    session_context_capture(&previous);
    session_prepare_context(session);
    const esp_err_t result =
        solar_os_shell_session_submit_command(session_state.ctx,
                                              session->shell_session,
                                              command);
    if (result == ESP_OK) {
        if (session == previous_foreground) {
            solar_os_sessions_process_requests();
        } else {
            (void)process_addressed_session_requests(session);
            session_draw_terminal_if_needed(session);
        }
    }
    if (session_state.foreground_session == previous_foreground &&
        (previous_foreground == NULL || previous_foreground->used)) {
        session_context_restore(&previous);
    } else if (session_state.foreground_session != NULL) {
        restore_foreground_context();
    } else {
        session_restore_base_context();
    }
    return result;
}

static void session_process_operation_requests(void)
{
    if (session_operation_queue == NULL) {
        return;
    }

    solar_os_session_operation_request_t *request = NULL;
    while (xQueueReceive(session_operation_queue, &request, 0) == pdTRUE) {
        if (request == NULL) {
            continue;
        }
        switch (request->type) {
        case SOLAR_OS_SESSION_OPERATION_CREATE_DISPLAY_APP:
            request->result =
                session_create_display_app_internal(request->app,
                                                    request->target_name,
                                                    request->argc,
                                                    request->argv,
                                                    &request->session_id,
                                                    request->busy_owner,
                                                    sizeof(request->busy_owner));
            break;
        case SOLAR_OS_SESSION_OPERATION_CLOSE_SESSION:
            request->result =
                session_close_internal(request->close_session_id, false);
            break;
        case SOLAR_OS_SESSION_OPERATION_SEND_COMMAND:
            request->result =
                session_send_command_internal(request->command_session_id,
                                              request->command,
                                              request->caller_io);
            break;
        default:
            request->result = ESP_ERR_INVALID_ARG;
            break;
        }
        xSemaphoreGive(request->complete);
    }
}

void solar_os_sessions_dispatch_tick(uint32_t now_ms)
{
    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };
    session_process_operation_requests();

    solar_os_session_context_snapshot_t previous = {0};
    session_context_capture(&previous);

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *session = &session_state.sessions[i];
        if (!session->used || session->app == NULL ||
            session->suspended ||
            session == session_state.foreground_session) {
            continue;
        }
        dispatch_session_event(session, &event);
        if (session->used) {
            /*
             * A throttled tick returns before dispatch_session_event() prepares
             * the session context. Requests and terminal drawing must still be
             * scoped to this session, never to the previously dispatched one.
             */
            session_prepare_context(session);
            if (!session->suspended && session->display_target[0] != '\0') {
                (void)process_addressed_session_requests(session);
                session_draw_terminal_if_needed(session);
            } else {
                process_inactive_session_requests(session);
            }
        }
        session_context_restore(&previous);
    }
    session_context_restore(&previous);

    if (session_state.foreground_session != NULL) {
        dispatch_session_event(session_state.foreground_session, &event);
        restore_foreground_context();
        solar_os_sessions_process_requests();
    } else if (session_state.foreground_app != NULL &&
               session_state.foreground_app->event != NULL) {
        dispatch_legacy_event(&event);
        solar_os_sessions_process_requests();
    }
}

uint32_t solar_os_sessions_requested_tick_interval_ms(void)
{
    uint32_t interval_ms = SOLAR_OS_TICK_INTERVAL_DEFAULT_MS;

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        const solar_os_session_entry_t *session = &session_state.sessions[i];
        if (!session->used || session->suspended ||
            session->app == NULL || session->app->event == NULL) {
            continue;
        }
        const uint32_t requested_ms =
            solar_os_app_tick_interval_ms(
                session->app,
                SOLAR_OS_TICK_INTERVAL_DEFAULT_MS);
        if (requested_ms < interval_ms) {
            interval_ms = requested_ms;
        }
    }

    const solar_os_app_t *legacy_app = session_state.foreground_app;
    if (session_state.foreground_session == NULL &&
        legacy_app != NULL && legacy_app->event != NULL) {
        const uint32_t requested_ms =
            solar_os_app_tick_interval_ms(
                legacy_app,
                SOLAR_OS_TICK_INTERVAL_DEFAULT_MS);
        if (requested_ms < interval_ms) {
            interval_ms = requested_ms;
        }
    }

    return interval_ms;
}

void solar_os_sessions_dispatch_resume(uint32_t now_ms)
{
    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_RESUME,
        .data.tick_ms = now_ms,
    };
    solar_os_sessions_dispatch_foreground_event(&event);
    solar_os_sessions_process_requests();
}

void solar_os_sessions_process_requests(void)
{
    handle_session_request();

    if (solar_os_context_take_exit_request(session_state.ctx)) {
        SOLAR_OS_LOGI(TAG,
                      "exit request for foreground app: %s",
                      session_state.foreground_app != NULL ?
                          app_display_name(session_state.foreground_app) : "(none)");
        if (session_state.foreground_session != NULL &&
            session_state.foreground_session->app != solar_os_shell_app() &&
            (app_is_resumable(session_state.foreground_session->app) ||
             session_state.foreground_session->close_on_exit)) {
            (void)close_session(session_state.foreground_session, false);
        } else if (session_state.foreground_app != solar_os_shell_app()) {
            solar_os_session_entry_t *return_session = session_state.legacy_return_session_valid ?
                session_return_target(session_state.legacy_return_session_id, NULL) :
                NULL;
            session_state.legacy_return_session_valid = false;
            (void)switch_to_session_or_shell(return_session);
        }
    }

    const solar_os_app_t *requested_app = solar_os_context_take_launch_request(session_state.ctx);
    if (requested_app != NULL) {
        const solar_os_launch_policy_t policy =
            solar_os_context_take_launch_policy(session_state.ctx);
        if (!solar_os_sessions_switch_to_app_with_policy(requested_app, policy)) {
            display_prompt_after_failed_launch();
        }
    }

    handle_session_request();
}

void solar_os_sessions_prompt_if_shell_active(void)
{
    if (session_state.foreground_app != solar_os_shell_app() || session_state.ctx == NULL) {
        return;
    }

    solar_os_shell_session_t *session = solar_os_context_shell_session(session_state.ctx);
    if (session != NULL) {
        solar_os_shell_session_prompt(session_state.ctx, session);
    }
}

esp_err_t solar_os_sessions_send_command(uint8_t session_id,
                                         const char *command,
                                         solar_os_shell_io_t *caller_io)
{
    if (command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskGetCurrentTaskHandle() == session_scheduler_task) {
        return session_send_command_internal(session_id, command, caller_io);
    }
    if (session_operation_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_session_operation_request_t request = {
        .type = SOLAR_OS_SESSION_OPERATION_SEND_COMMAND,
        .command_session_id = session_id,
        .command = command,
        .caller_io = caller_io,
        .complete = xSemaphoreCreateBinary(),
    };
    if (request.complete == NULL) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_session_operation_request_t *queued_request = &request;
    if (xQueueSend(session_operation_queue,
                   &queued_request,
                   pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(request.complete);
        return ESP_ERR_TIMEOUT;
    }

    (void)xSemaphoreTake(request.complete, portMAX_DELAY);
    const esp_err_t result = request.result;
    vSemaphoreDelete(request.complete);
    return result;
}

static esp_err_t create_display_shell(const char *target_name,
                                      bool activate,
                                      uint8_t *session_id,
                                      char *busy_owner,
                                      size_t busy_owner_len)
{
    if (session_id != NULL) {
        *session_id = 0;
    }
    if (busy_owner != NULL && busy_owner_len > 0) {
        busy_owner[0] = '\0';
    }
    if (target_name == NULL || target_name[0] == '\0' || session_state.ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(target_name, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!target.ready || target.u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_session_entry_t *active = session_active_for_display(target.name);
    if (active != NULL) {
        if (busy_owner != NULL && busy_owner_len > 0) {
            session_owner_name(active, busy_owner, busy_owner_len);
        }
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_session_entry_t *session = session_alloc_from(solar_os_shell_app(), 1);
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_shell_session_t *shell_session = solar_os_shell_session_create();
    if (shell_session == NULL) {
        session_dispose_unstarted(session);
        return ESP_ERR_NO_MEM;
    }
    session->shell_session = shell_session;

    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    session_owner_name(session, owner, sizeof(owner));

    solar_os_gfx_t *gfx = NULL;
    esp_err_t err = solar_os_display_open_gfx(target.name,
                                             owner,
                                             &gfx,
                                             busy_owner,
                                             busy_owner_len);
    if (err != ESP_OK) {
        session_dispose_unstarted(session);
        return err;
    }
    session->owns_display_target = true;
    session->gfx = gfx;
    strlcpy(session->display_target, target.name, sizeof(session->display_target));
    strlcpy(session->display_owner, owner, sizeof(session->display_owner));

    solar_os_terminal_t *terminal =
        solar_os_memory_calloc(1,
                               sizeof(*terminal),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "session.display");
    if (terminal == NULL) {
        session_dispose_unstarted(session);
        return ESP_ERR_NO_MEM;
    }
    solar_os_terminal_init_with_rotation(
        terminal, target.u8g2, target.base_rotation);
    if (!session_apply_registered_display_profile(terminal, target.name, target.u8g2)) {
        solar_os_terminal_inherit_text_profile(terminal, session_text_profile_source());
        (void)solar_os_terminal_set_orientation_transient(terminal, 0);
    }
    session_inherit_status_bar(terminal);
    solar_os_terminal_set_black_is_one(terminal, target.black_is_one);

    session->terminal = terminal;
    session->owns_terminal = true;
    session_set_gfx_palette_inverted(
        session,
        solar_os_terminal_palette_inverted(terminal));
    session_update_title(session);

    if (session_id != NULL) {
        *session_id = session->id;
    }

    const bool started = activate ?
        switch_to_session(session, true) :
        start_or_resume_detached_session(session);
    if (!started) {
        if (session_is_allocated(session)) {
            session_dispose_unstarted(session);
        }
        return ESP_FAIL;
    }

    session_default_input_focus_for_u8g2(target.u8g2);
    return ESP_OK;
}

esp_err_t solar_os_sessions_create_display_shell(const char *target_name,
                                                 uint8_t *session_id,
                                                 char *busy_owner,
                                                 size_t busy_owner_len)
{
    return create_display_shell(target_name,
                                true,
                                session_id,
                                busy_owner,
                                busy_owner_len);
}

esp_err_t solar_os_sessions_create_detached_display_shell(const char *target_name,
                                                          uint8_t *session_id,
                                                          char *busy_owner,
                                                          size_t busy_owner_len)
{
    return create_display_shell(target_name,
                                false,
                                session_id,
                                busy_owner,
                                busy_owner_len);
}

static esp_err_t session_set_context_args(int argc, char **argv)
{
    if (session_state.ctx == NULL ||
        argc < 0 ||
        argc > SOLAR_OS_APP_ARG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < argc; i++) {
        if (argv == NULL ||
            argv[i] == NULL ||
            strlen(argv[i]) >= SOLAR_OS_APP_ARG_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    memset(session_state.ctx->argv, 0, sizeof(session_state.ctx->argv));
    for (int i = 0; i < argc; i++) {
        strlcpy(session_state.ctx->argv[i],
                argv[i],
                sizeof(session_state.ctx->argv[i]));
    }
    session_state.ctx->argc = argc;
    return ESP_OK;
}

static esp_err_t session_create_display_app_internal(
    const solar_os_app_t *app,
    const char *target_name,
    int argc,
    char **argv,
    uint8_t *session_id,
    char *busy_owner,
    size_t busy_owner_len)
{
    if (session_id != NULL) {
        *session_id = 0;
    }
    if (busy_owner != NULL && busy_owner_len > 0) {
        busy_owner[0] = '\0';
    }
    if (app == NULL ||
        app == solar_os_shell_app() ||
        target_name == NULL ||
        target_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_app_registry_entry_t *entry =
        solar_os_app_registry_find_by_app(app);
    if (entry == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((entry->capabilities & SOLAR_OS_APP_CAP_DISPLAY) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(target_name, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!target.ready || target.u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char current_owner[SOLAR_OS_APP_OWNER_MAX];
    if (solar_os_app_registry_owner(app,
                                    current_owner,
                                    sizeof(current_owner))) {
        if (busy_owner != NULL && busy_owner_len > 0) {
            strlcpy(busy_owner, current_owner, busy_owner_len);
        }
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t args_err = session_set_context_args(argc, argv);
    if (args_err != ESP_OK) {
        return args_err;
    }

    char previous_focus[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    const bool had_input_focus =
        session_copy_input_focus(previous_focus, sizeof(previous_focus));
    bool created_parent = false;
    solar_os_session_entry_t *parent = session_active_for_display(target.name);
    if (parent == NULL) {
        uint8_t parent_id = 0;
        const esp_err_t shell_err =
            create_display_shell(target.name,
                                 false,
                                 &parent_id,
                                 busy_owner,
                                 busy_owner_len);
        if (shell_err != ESP_OK) {
            return shell_err;
        }
        created_parent = true;
        parent = session_by_id(parent_id);
    }
    if (parent == NULL) {
        if (!had_input_focus) {
            session_store_input_focus("");
        }
        return ESP_FAIL;
    }

    const bool started = parent == session_state.foreground_session ?
        switch_to_display_app(app, parent, true) :
        launch_detached_child(parent, app, SOLAR_OS_LAUNCH_CHILD_RETURN);
    if (!started) {
        if (created_parent && parent->used) {
            (void)close_session(parent, true);
        }
        if (created_parent && !had_input_focus) {
            session_store_input_focus("");
        }
        if (solar_os_app_registry_owner(app,
                                        current_owner,
                                        sizeof(current_owner))) {
            if (busy_owner != NULL && busy_owner_len > 0) {
                strlcpy(busy_owner, current_owner, busy_owner_len);
            }
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_FAIL;
    }

    solar_os_session_entry_t *session = session_find_by_app(app);
    if (session_id != NULL && session != NULL) {
        *session_id = session->id;
    }
    return ESP_OK;
}

esp_err_t solar_os_sessions_create_display_app(const solar_os_app_t *app,
                                               const char *target_name,
                                               int argc,
                                               char **argv,
                                               uint8_t *session_id,
                                               char *busy_owner,
                                               size_t busy_owner_len)
{
    if (xTaskGetCurrentTaskHandle() == session_scheduler_task) {
        return session_create_display_app_internal(app,
                                                   target_name,
                                                   argc,
                                                   argv,
                                                   session_id,
                                                   busy_owner,
                                                   busy_owner_len);
    }
    if (session_operation_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * The request and argument storage belong to the calling shell task. That
     * task remains blocked until the scheduler has copied the arguments and
     * completed the operation, so no additional request buffer is required.
     */
    solar_os_session_operation_request_t request = {
        .type = SOLAR_OS_SESSION_OPERATION_CREATE_DISPLAY_APP,
        .app = app,
        .target_name = target_name,
        .argc = argc,
        .argv = argv,
        .complete = xSemaphoreCreateBinary(),
    };
    if (request.complete == NULL) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_session_operation_request_t *queued_request = &request;
    if (xQueueSend(session_operation_queue,
                   &queued_request,
                   pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(request.complete);
        return ESP_ERR_TIMEOUT;
    }

    (void)xSemaphoreTake(request.complete, portMAX_DELAY);
    const esp_err_t result = request.result;
    if (session_id != NULL) {
        *session_id = request.session_id;
    }
    if (busy_owner != NULL && busy_owner_len > 0) {
        strlcpy(busy_owner, request.busy_owner, busy_owner_len);
    }
    vSemaphoreDelete(request.complete);
    return result;
}

esp_err_t solar_os_sessions_close_display(const char *target_name)
{
    if (target_name == NULL || target_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_session_context_snapshot_t previous = {0};
    session_context_capture(&previous);
    bool found = false;

    for (size_t pass = 0; pass < 2; pass++) {
        const bool close_owner = pass != 0;
        for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
            solar_os_session_entry_t *session = &session_state.sessions[i];
            if (!session->used ||
                strcmp(session->display_target, target_name) != 0 ||
                session->owns_display_target != close_owner) {
                continue;
            }
            found = true;
            session->has_return_session = false;
            (void)close_session(session, false);
        }
    }

    session_context_restore(&previous);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t session_close_internal(uint8_t session_id,
                                        bool reject_current_shell)
{
    solar_os_session_entry_t *session = session_by_id(session_id);
    if (!session_is_closable(session)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (session->app == solar_os_shell_app() &&
        reject_current_shell) {
        return ESP_ERR_INVALID_STATE;
    }
    if (session->app == solar_os_shell_app() &&
        solar_os_sessions_shell_count() <= 1U) {
        return ESP_ERR_NOT_ALLOWED;
    }

    const bool closed =
        session != session_state.foreground_session &&
        !session->suspended &&
        session->has_return_session ?
            close_detached_session_and_resume(session) :
            close_session(session, true);
    if (!closed) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void session_print_close_result(solar_os_shell_io_t *io,
                                       uint8_t session_id,
                                       esp_err_t result)
{
    if (io == NULL) {
        return;
    }
    if (result == ESP_OK) {
        solar_os_shell_io_printf(io, "closed session %u\n", (unsigned)session_id);
    } else if (result == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(io,
                                 "close: no such closable session: %u\n",
                                 (unsigned)session_id);
    } else if (result == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(io,
                                  "close: cannot close the current shell from itself");
    } else if (result == ESP_ERR_NOT_ALLOWED) {
        solar_os_shell_io_writeln(io,
                                  "close: cannot close the last shell");
    } else {
        solar_os_shell_io_printf(io, "close: failed: %u\n", (unsigned)session_id);
    }
    solar_os_shell_io_flush(io);
}

esp_err_t solar_os_sessions_close_session(uint8_t session_id, solar_os_shell_io_t *io)
{
    esp_err_t result = ESP_OK;
    if (xTaskGetCurrentTaskHandle() == session_scheduler_task) {
        solar_os_session_entry_t *session = session_by_id(session_id);
        const bool reject_current_shell =
            session != NULL &&
            session->app == solar_os_shell_app() &&
            io != NULL &&
            io == session_shell_io(session);
        result = session_close_internal(session_id, reject_current_shell);
    } else if (session_operation_queue == NULL) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        solar_os_session_operation_request_t request = {
            .type = SOLAR_OS_SESSION_OPERATION_CLOSE_SESSION,
            .close_session_id = session_id,
            .complete = xSemaphoreCreateBinary(),
        };
        if (request.complete == NULL) {
            result = ESP_ERR_NO_MEM;
        } else {
            solar_os_session_operation_request_t *queued_request = &request;
            if (xQueueSend(session_operation_queue,
                           &queued_request,
                           pdMS_TO_TICKS(1000)) != pdTRUE) {
                result = ESP_ERR_TIMEOUT;
            } else {
                (void)xSemaphoreTake(request.complete, portMAX_DELAY);
                result = request.result;
            }
            vSemaphoreDelete(request.complete);
        }
    }

    session_print_close_result(io, session_id, result);
    return result;
}

esp_err_t solar_os_sessions_close_any(uint8_t session_id, solar_os_shell_io_t *io)
{
    if (solar_os_port_shell_is_app_session_id(session_id)) {
        const esp_err_t err =
            solar_os_port_shell_close_app_session(session_id);
        session_print_close_result(io, session_id, err);
        return err;
    }
    if (solar_os_port_shell_is_session_id(session_id)) {
        if (solar_os_sessions_shell_count() <= 1U) {
            session_print_close_result(io,
                                       session_id,
                                       ESP_ERR_NOT_ALLOWED);
            return ESP_ERR_NOT_ALLOWED;
        }
        const esp_err_t err = solar_os_port_shell_stop(session_id);
        session_print_close_result(io, session_id, err);
        return err;
    }

    return solar_os_sessions_close_session(session_id, io);
}

size_t solar_os_sessions_active_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        if (session_state.sessions[i].used && session_state.sessions[i].app != NULL) {
            count++;
        }
    }
    return count;
}

size_t solar_os_sessions_shell_count(void)
{
    size_t count = solar_os_port_shell_session_count();

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        if (session_state.sessions[i].used &&
            session_state.sessions[i].app == solar_os_shell_app()) {
            count++;
        }
    }
    return count;
}

bool solar_os_sessions_get_active_id(size_t index, uint8_t *session_id)
{
    size_t current = 0;

    if (session_id == NULL) {
        return false;
    }

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        const solar_os_session_entry_t *session = &session_state.sessions[i];
        if (!session->used || session->app == NULL) {
            continue;
        }
        if (current == index) {
            *session_id = session->id;
            return true;
        }
        current++;
    }
    return false;
}

void solar_os_sessions_print_list(solar_os_shell_io_t *io, void *user)
{
    (void)user;

    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        return;
    }

    solar_os_shell_io_writeln(io,
                              "ID  TITLE        APP      STATE     OWNER    TIME");
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *session = &session_state.sessions[i];
        if (!session->used || session->app == NULL) {
            continue;
        }
        session_update_title(session);
        const char *state = session == session_state.foreground_session ? "active" :
            session->suspended ? "suspended" :
            session->display_target[0] != '\0' ? "detached" : "ready";
        const char *owner =
            session->display_target[0] != '\0' ?
                session->display_target : "display";
        if (session->app->event != NULL) {
            solar_os_shell_io_printf(io,
                                     "%-3u %-12.12s %-8.8s %-9.9s %-8.8s "
                                     "%" PRIu32 "/%" PRIu32 "ms %" PRIu32
                                     "/%" PRIu32 "us n=%" PRIu32 " !%" PRIu32 "\n",
                                     (unsigned)session->id,
                                     session->title,
                                     app_display_name(session->app),
                                     state,
                                     owner,
                                     session->tick_stats.interval_ms,
                                     session->tick_stats.deadline_ms,
                                     session->tick_stats.last_duration_us,
                                     session->tick_stats.max_duration_us,
                                     session->tick_stats.dispatch_count,
                                     session->tick_stats.deadline_miss_count);
        } else {
            solar_os_shell_io_printf(io,
                                     "%-3u %-12.12s %-8.8s %-9.9s %-8.8s -\n",
                                     (unsigned)session->id,
                                     session->title,
                                     app_display_name(session->app),
                                     state,
                                     owner);
        }
    }
    solar_os_port_shell_print_list(io);
    solar_os_shell_io_flush(io);
}
