#include "solar_os_sessions.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_app_registry.h"
#include "solar_os_display.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_port_shell.h"
#include "solar_os_shell.h"
#include "solar_os_terminal_internal.h"

#define SOLAR_OS_SESSION_MAX 8
#define SOLAR_OS_SESSION_TITLE_MAX 48

static const char *TAG = "solar_os_sessions";

typedef struct {
    bool used;
    bool started;
    bool suspended;
    bool claimed;
    bool owns_terminal;
    bool owns_display_target;
    bool close_on_exit;
    bool has_return_session;
    uint8_t id;
    uint8_t return_session_id;
    int argc;
    uint32_t argv_hash;
    const solar_os_app_t *app;
    solar_os_terminal_t *terminal;
    solar_os_gfx_t *gfx;
    solar_os_shell_session_t *shell_session;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char display_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    char title[SOLAR_OS_SESSION_TITLE_MAX];
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
} solar_os_session_state_t;

static solar_os_session_state_t session_state;

static const char *app_display_name(const solar_os_app_t *app)
{
    return app != NULL && app->name != NULL ? app->name : "?";
}

static bool app_is_resumable(const solar_os_app_t *app)
{
    return app != NULL && (app->flags & SOLAR_OS_APP_FLAG_RESUMABLE) != 0;
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
    if (session->app == solar_os_shell_app() && session->shell_session != NULL) {
        solar_os_context_set_shell_session(session_state.ctx, session->shell_session);
        solar_os_context_set_shell_io(session_state.ctx,
                                      solar_os_shell_session_io(session->shell_session));
    } else {
        solar_os_context_set_shell_io(session_state.ctx, NULL);
        solar_os_context_set_shell_session(session_state.ctx, NULL);
    }
}

static void restore_foreground_context(void)
{
    if (session_state.foreground_session != NULL) {
        session_prepare_context(session_state.foreground_session);
    }
}

static void session_request_text_present_mode(const solar_os_session_entry_t *session)
{
    if (session_state.ctx == NULL || solar_os_context_graphics_active(session_state.ctx)) {
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

    if (session->app == solar_os_shell_app() && session->display_target[0] != '\0') {
        snprintf(session->title,
                 sizeof(session->title),
                 "shell on %s",
                 session->display_target);
        return;
    }

    if (session->app->title != NULL) {
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

static bool switch_to_session(solar_os_session_entry_t *session, bool show_overlay);
static bool close_session(solar_os_session_entry_t *session, bool preserve_context);

static bool session_is_closable(const solar_os_session_entry_t *session)
{
    if (session == NULL || !session->used) {
        return false;
    }
    return session->app != solar_os_shell_app() || session->owns_display_target;
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
        strlcpy(session->title, "shell", sizeof(session->title));
    } else if (session->app == solar_os_shell_app()) {
        session->terminal = session_state.shell_terminal;
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
        if (session_state.sessions[i].used) {
            continue;
        }
        solar_os_session_entry_t *session = &session_state.sessions[i];
        memset(session, 0, sizeof(*session));
        session->used = true;
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

static void session_free_terminal(solar_os_session_entry_t *session)
{
    if (session == NULL || !session->owns_terminal || session->terminal == NULL) {
        return;
    }

    heap_caps_free(session->terminal);
    session->terminal = NULL;
    session->owns_terminal = false;
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
        return ESP_OK;
    }
    if (session->app == solar_os_shell_app()) {
        session->terminal = session_state.shell_terminal;
        return session->terminal != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (session_state.display_u8g2 == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_terminal_t *session_terminal = solar_os_psram_calloc(1, sizeof(*session_terminal));
    if (session_terminal == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_terminal_init(session_terminal, session_state.display_u8g2);
    session->terminal = session_terminal;
    session->owns_terminal = true;
    return ESP_OK;
}

static void session_mark_dirty(solar_os_session_entry_t *session)
{
    if (session != NULL && session->terminal != NULL) {
        session->terminal->dirty = true;
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

static void show_session_overlay(const solar_os_session_entry_t *session)
{
    if (session == NULL || session->title[0] == '\0' || session_state.display_u8g2 == NULL) {
        return;
    }
    if (session_state.overlay_fn != NULL) {
        session_state.overlay_fn(session->title, session_state.user);
    }
}

static void stop_legacy_foreground(void)
{
    if (session_state.foreground_app != NULL && session_state.foreground_app->stop != NULL) {
        SOLAR_OS_LOGI(TAG, "stop app: %s", app_display_name(session_state.foreground_app));
        session_state.foreground_app->stop(session_state.ctx);
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
    session->suspended = true;
    session_update_title(session);
}

static bool start_or_resume_session(solar_os_session_entry_t *session)
{
    if (session == NULL || session->app == NULL) {
        return false;
    }
    if (session_ensure_terminal(session) != ESP_OK || !session_claim_display(session)) {
        return false;
    }

    session_prepare_context(session);
    solar_os_context_set_graphics_active(session_state.ctx, false);

    if (!session->started) {
        session_store_context_args(session, session_state.ctx);
        if (session->app->start != NULL) {
            const esp_err_t app_err = session->app->start(session_state.ctx);
            if (app_err != ESP_OK) {
                SOLAR_OS_LOGE(TAG,
                              "App %s failed to start: %s",
                              app_display_name(session->app),
                              esp_err_to_name(app_err));
                session_dispose_unstarted(session);
                return false;
            }
        }
        session->started = true;
    } else if (session->app->resume != NULL) {
        session->app->resume(session_state.ctx);
    }

    session_state.foreground_session = session;
    session_state.foreground_app = session->app;
    session_state.foreground_app_claimed = false;
    session->suspended = false;
    session_update_title(session);
    session_mark_dirty(session);
    session_request_text_present_mode(session);
    return true;
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
    if (!start_or_resume_session(session)) {
        if (previous_session != NULL && previous_session->used) {
            (void)start_or_resume_session(previous_session);
        }
        return false;
    }
    if (show_overlay) {
        show_session_overlay(session);
    }
    return true;
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

    if (use_display_session && app_is_resumable(app)) {
        solar_os_session_entry_t *session = session_find_by_app(app);
        if (session != NULL &&
            session->started &&
            !session_args_match_context(session, session_state.ctx)) {
            (void)close_session(session, true);
            session = NULL;
        }
        if (session == NULL) {
            session = session_alloc(app);
        }
        if (session == NULL) {
            SOLAR_OS_LOGW(TAG, "No free app session for %s", app_display_name(app));
            return false;
        }
        return switch_to_session(session, false);
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
    if (session_state.foreground_app->start == NULL) {
        return true;
    }

    const esp_err_t app_err = session_state.foreground_app->start(session_state.ctx);
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
    if (app_is_resumable(app)) {
        solar_os_session_entry_t *session = session_find_by_app(app);
        if (session != NULL &&
            session->started &&
            !session_args_match_context(session, session_state.ctx)) {
            (void)close_session(session, true);
            session = NULL;
        }
        if (session == NULL) {
            session = session_alloc(app);
        }
        if (session == NULL) {
            SOLAR_OS_LOGW(TAG, "No free child app session for %s", app_display_name(app));
            return false;
        }
        if (session == parent) {
            return true;
        }
        session->close_on_exit = true;
        session->has_return_session = true;
        session->return_session_id = parent->id;
        return switch_to_session(session, false);
    }

    session_state.legacy_return_session_valid = true;
    session_state.legacy_return_session_id = parent->id;
    const bool switched = switch_to_app(app);
    if (!switched) {
        session_state.legacy_return_session_valid = false;
    }
    return switched;
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
    solar_os_terminal_t *previous_terminal = NULL;
    solar_os_gfx_t *previous_gfx = NULL;
    solar_os_shell_io_t *previous_shell_io = NULL;
    solar_os_shell_session_t *previous_shell_session = NULL;

    if (preserve_context && !was_foreground) {
        previous_terminal = session_state.current_terminal;
        previous_gfx = solar_os_context_gfx(session_state.ctx);
        previous_shell_io = solar_os_context_shell_io(session_state.ctx);
        previous_shell_session = solar_os_context_shell_session(session_state.ctx);
    }

    if (was_foreground) {
        session_state.foreground_session = NULL;
        session_state.foreground_app = NULL;
    }

    session_prepare_context(session);
    if (session->app != NULL && session->app->stop != NULL) {
        SOLAR_OS_LOGI(TAG,
                      "close session %u: %s",
                      (unsigned)session->id,
                      app_display_name(session->app));
        session->app->stop(session_state.ctx);
    }
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
        if (return_session != NULL) {
            return switch_to_session_or_shell(return_session);
        }
        if (session_state.shell_terminal != NULL) {
            return switch_to_session(ensure_shell_session(), false);
        }
        set_current_terminal(NULL);
        solar_os_context_set_gfx(session_state.ctx, session_state.default_gfx);
        solar_os_context_set_shell_io(session_state.ctx, NULL);
        solar_os_context_set_shell_session(session_state.ctx, NULL);
        return true;
    }
    if (preserve_context) {
        set_current_terminal(previous_terminal);
        solar_os_context_set_gfx(session_state.ctx, previous_gfx);
        solar_os_context_set_shell_io(session_state.ctx, previous_shell_io);
        solar_os_context_set_shell_session(session_state.ctx, previous_shell_session);
        return true;
    }
    restore_foreground_context();
    return true;
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
            if (!switch_to_session(session, true) && io != NULL) {
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

static void dispatch_session_event(solar_os_session_entry_t *session,
                                   const solar_os_event_t *event)
{
    if (session == NULL || !session->used || session->app == NULL ||
        session->app->event == NULL || event == NULL) {
        return;
    }

    session_prepare_context(session);
    session->app->event(session_state.ctx, event);
    if (solar_os_context_take_exit_request(session_state.ctx)) {
        (void)close_session(session, false);
    }
}

esp_err_t solar_os_sessions_init(solar_os_context_t *ctx,
                                 solar_os_terminal_t *shell_terminal,
                                 u8g2_t *display_u8g2,
                                 solar_os_sessions_terminal_fn terminal_fn,
                                 solar_os_sessions_overlay_fn overlay_fn,
                                 void *user)
{
    memset(&session_state, 0, sizeof(session_state));
    session_state.ctx = ctx;
    session_state.shell_terminal = shell_terminal;
    session_state.current_terminal = shell_terminal;
    session_state.display_u8g2 = display_u8g2;
    session_state.default_gfx = solar_os_context_gfx(ctx);
    session_state.terminal_fn = terminal_fn;
    session_state.overlay_fn = overlay_fn;
    session_state.user = user;
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
    if (session_state.sessions[0].used &&
        session_state.sessions[0].app == solar_os_shell_app()) {
        session_state.sessions[0].terminal = shell_terminal;
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

void solar_os_sessions_mark_foreground_dirty(void)
{
    if (session_state.foreground_session != NULL) {
        session_mark_dirty(session_state.foreground_session);
    } else if (session_state.current_terminal != NULL) {
        session_state.current_terminal->dirty = true;
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
        session_state.foreground_app->event(session_state.ctx, event);
    }
}

void solar_os_sessions_dispatch_tick(uint32_t now_ms)
{
    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };

    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *session = &session_state.sessions[i];
        if (!session->used || session->app == NULL ||
            session == session_state.foreground_session) {
            continue;
        }
        dispatch_session_event(session, &event);
    }
    restore_foreground_context();

    if (session_state.foreground_session != NULL) {
        dispatch_session_event(session_state.foreground_session, &event);
        restore_foreground_context();
        solar_os_sessions_process_requests();
    } else if (session_state.foreground_app != NULL &&
               session_state.foreground_app->event != NULL) {
        session_state.foreground_app->event(session_state.ctx, &event);
        solar_os_sessions_process_requests();
    }
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

esp_err_t solar_os_sessions_create_display_shell(const char *target_name,
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

    solar_os_terminal_t *terminal = solar_os_psram_calloc(1, sizeof(*terminal));
    if (terminal == NULL) {
        session_dispose_unstarted(session);
        return ESP_ERR_NO_MEM;
    }
    solar_os_terminal_init(terminal, target.u8g2);
    solar_os_terminal_set_black_is_one(terminal, target.black_is_one);

    session->terminal = terminal;
    session->owns_terminal = true;
    session_update_title(session);

    if (session_id != NULL) {
        *session_id = session->id;
    }

    if (!switch_to_session(session, true)) {
        if (session->used) {
            session_dispose_unstarted(session);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t solar_os_sessions_close_session(uint8_t session_id, solar_os_shell_io_t *io)
{
    solar_os_session_entry_t *session = session_by_id(session_id);
    if (!session_is_closable(session)) {
        if (io != NULL) {
            solar_os_shell_io_printf(io,
                                     "close: no such closable session: %u\n",
                                     (unsigned)session_id);
            solar_os_shell_io_flush(io);
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (session == session_state.foreground_session &&
        session->app == solar_os_shell_app() &&
        io != NULL &&
        io == solar_os_context_shell_io(session_state.ctx)) {
        solar_os_shell_io_writeln(io, "close: cannot close the current shell from itself");
        solar_os_shell_io_flush(io);
        return ESP_ERR_INVALID_STATE;
    }

    if (!close_session(session, true)) {
        if (io != NULL) {
            solar_os_shell_io_printf(io, "close: failed: %u\n", (unsigned)session_id);
            solar_os_shell_io_flush(io);
        }
        return ESP_FAIL;
    }

    if (io != NULL) {
        solar_os_shell_io_printf(io, "closed session %u\n", (unsigned)session_id);
        solar_os_shell_io_flush(io);
    }
    return ESP_OK;
}

esp_err_t solar_os_sessions_close_any(uint8_t session_id, solar_os_shell_io_t *io)
{
    if (solar_os_port_shell_is_session_id(session_id)) {
        const esp_err_t err = solar_os_port_shell_stop(session_id);
        if (io != NULL) {
            if (err == ESP_OK) {
                solar_os_shell_io_printf(io,
                                         "closed session %u\n",
                                         (unsigned)session_id);
            } else {
                solar_os_shell_io_printf(io,
                                         "close: failed: %s\n",
                                         esp_err_to_name(err));
            }
            solar_os_shell_io_flush(io);
        }
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

    solar_os_shell_io_writeln(io, "ID  STATE       APP       TITLE");
    for (size_t i = 0; i < SOLAR_OS_SESSION_MAX; i++) {
        solar_os_session_entry_t *session = &session_state.sessions[i];
        if (!session->used || session->app == NULL) {
            continue;
        }
        session_update_title(session);
        const char *state = session == session_state.foreground_session ? "active" :
            session->suspended ? "suspended" : "ready";
        solar_os_shell_io_printf(io,
                                 "%-3u %-11s %-9s %s\n",
                                 (unsigned)session->id,
                                 state,
                                 app_display_name(session->app),
                                 session->title);
    }
    solar_os_port_shell_print_list(io);
    solar_os_shell_io_flush(io);
}
