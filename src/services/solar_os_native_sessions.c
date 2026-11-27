#include "solar_os_native_sessions.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_sessions_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_port_shell.h"
#include "solar_os_app_registry.h"
#include "solar_os_shell.h"
#include "solar_os_storage.h"
#include "solar_os_memory.h"
#include "solar_os_sessions.h"
#include "solar_os_shutdown.h"

static int session_snapshot(uint32_t kinds, solar_os_native_session_info_v1_t *entries,
    size_t capacity, size_t *total)
{
    return solar_os_native_result(solar_os_sessions_snapshot(kinds, UINT32_MAX, entries, capacity, total));
}

static int session_find(uint32_t id, solar_os_native_session_info_v1_t *entry)
{
    if (!entry) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(entry, 0, sizeof(*entry));
    if (id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t total = 0;
    esp_err_t error = solar_os_sessions_snapshot(SOLAR_OS_NATIVE_SESSION_ALL, id, entry, 1, &total);
    if (error == ESP_OK && !total) error = ESP_ERR_NOT_FOUND;
    return solar_os_native_result(error);
}

static int session_get_display_focus(char *target, size_t capacity)
{
    return solar_os_native_result(solar_os_sessions_get_display_focus(target, capacity));
}

static int session_focus_display(const char *target)
{
    return solar_os_native_result(solar_os_sessions_focus_display(target));
}

static int session_open_shell(const solar_os_native_shell_session_request_v1_t *request,
    solar_os_native_shell_session_result_v1_t *result)
{
    return solar_os_native_result(
        solar_os_sessions_open_shell(solar_os_application_context(), request, result));
}

static int session_resize_port(uint32_t id, uint16_t columns, uint16_t rows)
{
    if (id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_port_shell_set_dimensions(id, columns, rows));
}

static int session_set_display_preference(const char *target, uint32_t field, uint32_t value, bool save)
{
    return solar_os_native_result(solar_os_sessions_set_display_preference(target, field, value, save));
}

static int session_set_terminal_preference(uint32_t field, uint32_t value)
{
    solar_os_context_t *ctx = solar_os_application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    solar_os_terminal_t *terminal = solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT ?
        NULL : solar_os_context_terminal(ctx);
    return solar_os_native_result(solar_os_sessions_set_terminal_preference(terminal, field, value));
}

typedef struct {
    const char *target;
    uint8_t session;
} session_active_display_request_t;

static esp_err_t session_active_display_on_owner(void *context)
{
    session_active_display_request_t *request = context;
    return solar_os_sessions_active_for_display(request->target, &request->session) ?
        ESP_OK : ESP_ERR_NOT_FOUND;
}

static int session_active_on_display(const char *target, uint32_t *session)
{
    if (!session) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *session = 0;
    if (!target || !*target) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (solar_os_shutdown_requested()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    session_active_display_request_t request = {.target = target};
    const esp_err_t error = solar_os_sessions_run_on_owner(session_active_display_on_owner, &request);
    if (error == ESP_OK) *session = request.session;
    return solar_os_native_result(error);
}

static int session_move_display_focus(uint32_t direction, bool *moved)
{
    return solar_os_native_result(solar_os_sessions_move_display_focus(direction, moved));
}

static bool session_application_uses_display(const char *target)
{
    solar_os_context_t *ctx = solar_os_application_context();
    return ctx && solar_os_shell_io_kind(solar_os_context_shell_io(ctx)) != SOLAR_OS_SHELL_IO_KIND_PORT &&
        solar_os_sessions_context_uses_display(ctx, target);
}

typedef struct {
    bool forward, handled;
} session_cycle_request_t;

static esp_err_t session_cycle_on_owner(void *context)
{
    session_cycle_request_t *request = context;
    request->handled = request->forward ? solar_os_sessions_cycle_input_focus() :
        solar_os_sessions_cycle_input_focus_previous();
    return ESP_OK;
}

static int session_cycle_focus(bool forward, bool *handled)
{
    if (!handled) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *handled = false;
    if (solar_os_shutdown_requested()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    session_cycle_request_t request = {.forward = forward};
    const esp_err_t error = solar_os_sessions_run_on_owner(session_cycle_on_owner, &request);
    if (error == ESP_OK) *handled = request.handled;
    return solar_os_native_result(error);
}

static int session_submit_command(uint32_t id, const char *command)
{
    if (id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_context_t *ctx = solar_os_application_context();
    solar_os_shell_io_t *io = ctx ? solar_os_context_shell_io(ctx) : NULL;
    return solar_os_native_result(solar_os_sessions_send_command(id, command, io));
}

static int session_request_close(uint32_t id)
{
    if (id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (solar_os_shutdown_requested()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_context_t *ctx = solar_os_application_context();
    if (ctx) {
        solar_os_context_request_session_close(ctx, id);
        return SOLAR_OS_NATIVE_OK;
    }
    return solar_os_native_result(solar_os_sessions_close_any(id, NULL));
}

static int session_open_display_app(const solar_os_native_display_app_request_v1_t *request,
    solar_os_native_display_app_result_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_display_app_result_v1_t){0};
    if (!request || !request->name || !*request->name || !request->display || !*request->display ||
        request->argument_count >= SOLAR_OS_APP_ARG_MAX || (request->argument_count && !request->arguments))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (solar_os_shutdown_requested()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;

    const solar_os_app_registry_entry_t *app = solar_os_app_registry_find(request->name);
    if (!app || !app->app) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (!(app->capabilities & SOLAR_OS_APP_CAP_DISPLAY)) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    const size_t argc = request->argument_count + 1;
    if (argc < app->min_argc || (app->max_argc && argc > app->max_argc))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    for (size_t i = 1; i < argc; ++i) {
        const char *arg = request->arguments[i - 1];
        if (!arg) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (strlen(arg) >= SOLAR_OS_APP_ARG_LEN) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    solar_os_context_t *ctx = solar_os_application_context();
    char directory[SOLAR_OS_APP_ARG_LEN] = {0};
    if (!request->cwd) {
        const esp_err_t error = ctx ? solar_os_shell_resolve_path(ctx, NULL, directory, sizeof(directory)) :
            solar_os_storage_resolve_path_at(NULL, NULL, directory, sizeof(directory));
        if (error) return solar_os_native_result(error);
    } else {
        const size_t size = strlen(request->cwd);
        if (size >= sizeof(directory)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        memcpy(directory, request->cwd, size + 1);
    }

    if (ctx && session_application_uses_display(request->display)) {
        const esp_err_t error = solar_os_app_registry_request_launch(ctx, request->name,
            request->argument_count, request->arguments);
        if (!error) {
            memcpy(ctx->launch_cwd, directory, sizeof(directory));
            out->deferred = true;
        }
        return solar_os_native_result(error);
    }
    /* Input strings can refer to the launching application's argument buffer.
     * The scheduler replaces that buffer while it starts the requested app. */
    const size_t name_size = strlen(app->name) + 1;
    if (name_size > SOLAR_OS_APP_ARG_LEN) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    char (*storage)[SOLAR_OS_APP_ARG_LEN] = solar_os_memory_alloc(argc * SOLAR_OS_APP_ARG_LEN,
        SOLAR_OS_MEMORY_TRANSIENT, "session.launch.args");
    if (!storage) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    char *argv[SOLAR_OS_APP_ARG_MAX] = {storage[0]};
    memcpy(storage[0], app->name, name_size);
    for (size_t i = 1; i < argc; ++i) {
        argv[i] = storage[i];
        const char *arg = request->arguments[i - 1];
        memcpy(argv[i], arg, strlen(arg) + 1);
    }
    uint8_t id = 0;
    const esp_err_t error = solar_os_sessions_create_display_app_at(app->app, request->display,
        argc, argv, directory, &id, out->busy_owner, sizeof(out->busy_owner));
    solar_os_memory_free(storage);
    if (!error) out->id = id;
    return solar_os_native_result(error);
}

typedef struct {
    solar_os_application_event_v1_t event;
    uint8_t id;
    bool delivered;
} session_input_request_t;

static esp_err_t session_dispatch_input_on_owner(void *context)
{
    session_input_request_t *request = context;
    if (solar_os_shutdown_requested()) return ESP_ERR_INVALID_STATE;
    request->delivered = solar_os_sessions_dispatch_session_event(request->id, &request->event);
    return ESP_OK;
}

static int session_dispatch_input(uint32_t id, const solar_os_application_event_v1_t *event, bool *delivered)
{
    if (!delivered) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *delivered = false;
    if (id > UINT8_MAX || !event) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    switch (event->type) {
    case SOLAR_OS_EVENT_TEXT:
        if (!event->data.text[0] || !memchr(event->data.text, 0, sizeof(event->data.text)))
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        break;
    case SOLAR_OS_EVENT_CHAR:
    case SOLAR_OS_EVENT_KEY:
    case SOLAR_OS_EVENT_POINTER:
    case SOLAR_OS_EVENT_AXIS:
    case SOLAR_OS_EVENT_GESTURE:
        break;
    default:
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (solar_os_application_context()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    session_input_request_t request = {.id = id, .event = *event};
    const esp_err_t error = solar_os_sessions_run_on_owner(session_dispatch_input_on_owner, &request);
    if (!error) *delivered = request.delivered;
    return solar_os_native_result(error);
}

static int session_close_display(const char *target)
{
    if (solar_os_application_context()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return solar_os_native_result(solar_os_sessions_close_display(target));
}

static int session_stop_port_shell(uint32_t id)
{
    if (id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_port_shell_stop(id));
}

static bool session_port_is_local(uint32_t id)
{
    solar_os_context_t *ctx = solar_os_application_context();
    if (!ctx || id > UINT8_MAX) return false;
    return solar_os_port_shell_is_app_session_id(id) ? solar_os_port_shell_context_owns_app_session(ctx, id) :
        solar_os_port_shell_context_owns_session(ctx, id);
}

static int session_foreground_port_app(uint32_t id)
{
    if (id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (solar_os_shutdown_requested()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_context_t *ctx = solar_os_application_context();
    if (ctx && solar_os_shell_io_kind(solar_os_context_shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT &&
        solar_os_application_descriptor() != solar_os_shell_app()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return solar_os_native_result(solar_os_port_shell_foreground_app_session(ctx, id));
}

static int session_foreground_last_port_app(uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (solar_os_shutdown_requested()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_context_t *ctx = solar_os_application_context();
    if (!ctx || solar_os_application_descriptor() != solar_os_shell_app())
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    uint8_t id;
    const esp_err_t error = solar_os_port_shell_foreground_last_app(ctx, &id);
    if (!error) *out = id;
    return solar_os_native_result(error);
}

static const solar_os_native_sessions_api_v1_t sessions_api = {
    .abi_version = SOLAR_OS_NATIVE_SESSIONS_ABI,
    .struct_size = sizeof(sessions_api),
    .snapshot = session_snapshot,
    .find = session_find,
    .get_display_focus = session_get_display_focus,
    .focus_display = session_focus_display,
    .open_shell = session_open_shell,
    .resize_port = session_resize_port,
    .set_display_preference = session_set_display_preference,
    .set_terminal_preference = session_set_terminal_preference,
    .active_on_display = session_active_on_display,
    .move_display_focus = session_move_display_focus,
    .application_uses_display = session_application_uses_display,
    .cycle_focus = session_cycle_focus,
    .submit_command = session_submit_command,
    .request_close = session_request_close,
    .open_display_app = session_open_display_app,
    .dispatch_input = session_dispatch_input,
    .close_display = session_close_display,
    .stop_port_shell = session_stop_port_shell,
    .port_session_is_local = session_port_is_local,
    .foreground_port_app = session_foreground_port_app,
    .foreground_last_port_app = session_foreground_last_port_app,
};
#endif

const void *solar_os_native_sessions_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_sessions_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
