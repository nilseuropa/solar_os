#include "solar_os_port_shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_app_registry.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#include "solar_os_queue.h"
#include "solar_os_scheduler.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_vt100.h"

#define PORT_SHELL_MAX 4
#define PORT_SHELL_TASK_STACK 16384
#define PORT_SHELL_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PORT_SHELL_TASK_STACK);
#define PORT_SHELL_READ_BUF 64
#define PORT_SHELL_READ_TIMEOUT_MS 50U
#define PORT_SHELL_ESC_FLUSH_MS 40U
#define PORT_SHELL_TICK_MS 100U
#define PORT_SHELL_DEFAULT_COLS 80
#define PORT_SHELL_DEFAULT_ROWS 24
#define PORT_SHELL_IDENTITY_PROBE_TIMEOUT_MS 200U
#define PORT_SHELL_IDENTITY_PROBE_READ_MS 25U
#define PORT_SHELL_SIZE_PROBE_TIMEOUT_MS 200U
#define PORT_SHELL_SIZE_PROBE_READ_MS 25U
#define PORT_SHELL_SIZE_PROBE_MIN_COLS 20U
#define PORT_SHELL_SIZE_PROBE_MIN_ROWS 8U
#define PORT_SHELL_SIZE_PROBE_MAX_COLS 300U
#define PORT_SHELL_SIZE_PROBE_MAX_ROWS 120U
#define PORT_APP_SESSION_MAX 4U
#define PORT_APP_OPERATION_QUEUE_LEN 2U
#define PORT_APP_SUSPEND_CHAR 0x1aU

static const char *TAG = "solar_os_port_shell";

typedef struct {
    bool used;
    bool started;
    bool suspended;
    bool has_return_session;
    uint8_t id;
    uint8_t return_session_id;
    const solar_os_app_t *app;
    solar_os_app_class_t app_class;
    char title[48];
    solar_os_tick_stats_t tick_stats;
} port_app_session_t;

typedef enum {
    PORT_APP_OPERATION_FOREGROUND,
    PORT_APP_OPERATION_CLOSE,
} port_app_operation_type_t;

typedef struct {
    port_app_operation_type_t type;
    uint8_t session_id;
    SemaphoreHandle_t complete;
    esp_err_t result;
} port_app_operation_request_t;

typedef struct {
    bool used;
    bool running;
    bool stop_requested;
    uint32_t generation;
    uint8_t id;
    TaskHandle_t task;
    solar_os_port_handle_t port;
    solar_os_shell_session_t *session;
    solar_os_context_t ctx;
    solar_os_vt100_input_t input;
    port_app_session_t app_sessions[PORT_APP_SESSION_MAX];
    port_app_session_t *active_app_session;
    uint8_t last_app_session_id;
    QueueHandle_t app_operations;
    uint32_t app_operation_users;
    bool run_startup;
    solar_os_shell_terminal_profile_t requested_terminal_profile;
    solar_os_shell_terminal_profile_t terminal_profile;
    bool configured_size;
    uint16_t configured_cols;
    uint16_t configured_rows;
    bool dimensions_pending;
    uint16_t pending_cols;
    uint16_t pending_rows;
    char port_name[SOLAR_OS_PORT_NAME_MAX];
    esp_err_t last_error;
    const solar_os_app_t *tick_app;
    solar_os_tick_stats_t tick_stats;
} port_shell_state_t;

/* Port-shell state is task-owned metadata, not DMA or ISR data. Keep the
 * substantial idle registry out of scarce internal RAM on PSRAM boards. */
static EXT_RAM_BSS_ATTR port_shell_state_t port_shells[PORT_SHELL_MAX];
static portMUX_TYPE port_shells_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t port_shell_reserved_task;
static port_shell_state_t *port_shell_reserved_state;
static bool port_shell_reserved_initializing;

static void port_shell_process_requests(port_shell_state_t *state);
static void port_shell_run(port_shell_state_t *state);
static void port_shell_process_app_operations(port_shell_state_t *state);
static esp_err_t port_app_foreground(port_shell_state_t *state,
                                     uint8_t session_id);
static esp_err_t port_app_close(port_shell_state_t *state,
                                uint8_t session_id,
                                bool return_to_parent,
                                bool show_prompt_when_idle,
                                bool allow_terminal_preserve);

static void port_app_report_launch_failure(port_shell_state_t *state,
                                           const solar_os_app_t *app,
                                           esp_err_t err)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    int exit_code = 1;
    (void)solar_os_context_take_exit_result(&state->ctx, &exit_code);
    char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX] = {0};
    const bool has_message = solar_os_context_take_status_message(
        &state->ctx, message, sizeof(message));
    (void)solar_os_context_take_exit_request(&state->ctx);
    solar_os_shell_session_set_exit_result(state->session, exit_code, NULL);
    if (has_message) {
        solar_os_shell_io_writeln(io, message);
        return;
    }
    const char *name = app != NULL && app->name != NULL ? app->name : "app";
    if (err == ESP_ERR_NOT_SUPPORTED &&
        !solar_os_shell_io_is_cursor_addressable(io)) {
        solar_os_shell_io_printf(io, "%s: can't run on a dumb terminal\n", name);
        return;
    }
    solar_os_shell_io_printf(io,
                             "%s: launch failed: %s\n",
                             name,
                             esp_err_to_name(err));
}

static bool port_shell_dimensions_valid(uint16_t cols, uint16_t rows)
{
    return cols >= PORT_SHELL_SIZE_PROBE_MIN_COLS &&
        rows >= PORT_SHELL_SIZE_PROBE_MIN_ROWS &&
        cols <= PORT_SHELL_SIZE_PROBE_MAX_COLS &&
        rows <= PORT_SHELL_SIZE_PROBE_MAX_ROWS;
}

static bool port_shell_should_stop(const port_shell_state_t *state)
{
    if (state == NULL) {
        return true;
    }
    portENTER_CRITICAL(&port_shells_lock);
    const bool stop = !state->used || state->stop_requested;
    portEXIT_CRITICAL(&port_shells_lock);
    return stop;
}

static uint32_t port_shell_now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void port_shell_owner(const port_shell_state_t *state, char *owner, size_t owner_len)
{
    if (owner == NULL || owner_len == 0) {
        return;
    }

    if (state == NULL) {
        strlcpy(owner, "session:?", owner_len);
        return;
    }
    snprintf(owner, owner_len, "session:%u", (unsigned)state->id);
}

static const solar_os_app_t *port_shell_foreground_app(port_shell_state_t *state)
{
    return state != NULL && state->active_app_session != NULL ?
        state->active_app_session->app : NULL;
}

static bool port_shell_terminal_profile_is_valid(solar_os_shell_terminal_profile_t profile)
{
    switch (profile) {
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO:
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB:
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI:
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100:
        return true;
    default:
        return false;
    }
}

static bool port_shell_charset_is_valid(solar_os_shell_charset_t charset)
{
    return charset == SOLAR_OS_SHELL_CHARSET_UTF8 ||
        charset == SOLAR_OS_SHELL_CHARSET_ASCII;
}

static bool port_shell_parse_da_report(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return false;
    }

    for (size_t i = 0; i + 3U < len; i++) {
        if (data[i] != 0x1b || data[i + 1U] != '[') {
            continue;
        }

        size_t pos = i + 2U;
        bool have_payload = false;
        if (pos < len && (data[pos] == '?' || data[pos] == '>')) {
            have_payload = true;
            pos++;
        }
        while (pos < len &&
               ((data[pos] >= '0' && data[pos] <= '9') || data[pos] == ';')) {
            have_payload = true;
            pos++;
        }
        if (have_payload && pos < len && data[pos] == 'c') {
            return true;
        }
    }

    return false;
}

static bool port_shell_parse_size_report(const uint8_t *data,
                                         size_t len,
                                         uint16_t *rows,
                                         uint16_t *cols)
{
    if (data == NULL || rows == NULL || cols == NULL) {
        return false;
    }

    for (size_t i = 0; i + 3U < len; i++) {
        if (data[i] != 0x1b || data[i + 1U] != '[') {
            continue;
        }

        size_t pos = i + 2U;
        unsigned parsed_rows = 0;
        unsigned parsed_cols = 0;
        bool have_rows = false;
        bool have_cols = false;

        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            have_rows = true;
            parsed_rows = (parsed_rows * 10U) + (unsigned)(data[pos] - '0');
            pos++;
        }
        if (!have_rows || pos >= len || data[pos] != ';') {
            continue;
        }
        pos++;
        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            have_cols = true;
            parsed_cols = (parsed_cols * 10U) + (unsigned)(data[pos] - '0');
            pos++;
        }
        if (!have_cols || pos >= len || data[pos] != 'R') {
            continue;
        }
        if (parsed_cols < PORT_SHELL_SIZE_PROBE_MIN_COLS ||
            parsed_rows < PORT_SHELL_SIZE_PROBE_MIN_ROWS ||
            parsed_cols > PORT_SHELL_SIZE_PROBE_MAX_COLS ||
            parsed_rows > PORT_SHELL_SIZE_PROBE_MAX_ROWS) {
            continue;
        }

        *rows = (uint16_t)parsed_rows;
        *cols = (uint16_t)parsed_cols;
        return true;
    }

    return false;
}

static bool port_shell_probe_terminal_identity(port_shell_state_t *state)
{
    uint8_t response[64];
    size_t response_len = 0;

    if (state == NULL || state->session == NULL ||
        !solar_os_port_handle_valid(&state->port)) {
        return false;
    }

    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    if (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT) {
        return false;
    }

    const char probe[] = "\x1b[c";
    (void)solar_os_shell_io_write_raw(io, probe, sizeof(probe) - 1U);

    const uint32_t start_ms = port_shell_now_ms();
    while ((uint32_t)(port_shell_now_ms() - start_ms) < PORT_SHELL_IDENTITY_PROBE_TIMEOUT_MS &&
           response_len < sizeof(response)) {
        size_t read_len = 0;
        const esp_err_t err = solar_os_port_read(&state->port,
                                                 response + response_len,
                                                 sizeof(response) - response_len,
                                                 PORT_SHELL_IDENTITY_PROBE_READ_MS,
                                                 &read_len);
        if (err == ESP_OK && read_len > 0) {
            response_len += read_len;
            if (port_shell_parse_da_report(response, response_len)) {
                return true;
            }
            continue;
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return false;
        }
    }

    return false;
}

static void port_shell_probe_terminal_size(port_shell_state_t *state)
{
    uint8_t response[48];
    size_t response_len = 0;
    uint16_t rows = 0;
    uint16_t cols = 0;

    if (state == NULL || state->session == NULL ||
        !solar_os_port_handle_valid(&state->port)) {
        return;
    }

    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    if (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT) {
        return;
    }

    const char probe[] = "\x1b[?25h" "\x1b" "7" "\x1b[999;999H" "\x1b[6n" "\x1b" "8";
    (void)solar_os_shell_io_write_raw(io, probe, sizeof(probe) - 1U);

    const uint32_t start_ms = port_shell_now_ms();
    while ((uint32_t)(port_shell_now_ms() - start_ms) < PORT_SHELL_SIZE_PROBE_TIMEOUT_MS &&
           response_len < sizeof(response)) {
        size_t read_len = 0;
        const esp_err_t err = solar_os_port_read(&state->port,
                                                 response + response_len,
                                                 sizeof(response) - response_len,
                                                 PORT_SHELL_SIZE_PROBE_READ_MS,
                                                 &read_len);
        if (err == ESP_OK && read_len > 0) {
            response_len += read_len;
            if (port_shell_parse_size_report(response, response_len, &rows, &cols)) {
                solar_os_shell_io_set_dimensions(io, cols, rows);
                SOLAR_OS_LOGI(TAG,
                              "terminal size on %s: %ux%u",
                              state->port_name,
                              (unsigned)cols,
                              (unsigned)rows);
                return;
            }
            continue;
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return;
        }
    }
}

static uint8_t port_app_session_id(const port_shell_state_t *state,
                                   size_t slot)
{
    const size_t shell_index = (size_t)(state - port_shells);
    return (uint8_t)(SOLAR_OS_PORT_APP_SESSION_ID_BASE +
                     shell_index * PORT_APP_SESSION_MAX +
                     slot);
}

static port_app_session_t *port_app_by_id(port_shell_state_t *state,
                                          uint8_t session_id)
{
    if (state == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PORT_APP_SESSION_MAX; i++) {
        port_app_session_t *session = &state->app_sessions[i];
        if (session->used && session->id == session_id) {
            return session;
        }
    }
    return NULL;
}

static port_app_session_t *port_app_find(port_shell_state_t *state,
                                         const solar_os_app_t *app)
{
    if (state == NULL || app == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PORT_APP_SESSION_MAX; i++) {
        port_app_session_t *session = &state->app_sessions[i];
        if (session->used && session->app == app) {
            return session;
        }
    }
    return NULL;
}

static port_app_session_t *port_app_alloc(port_shell_state_t *state,
                                          const solar_os_app_t *app)
{
    if (state == NULL || app == NULL) {
        return NULL;
    }
    portENTER_CRITICAL(&port_shells_lock);
    for (size_t i = 0; i < PORT_APP_SESSION_MAX; i++) {
        port_app_session_t *session = &state->app_sessions[i];
        if (session->used) {
            continue;
        }
        memset(session, 0, sizeof(*session));
        session->used = true;
        session->id = port_app_session_id(state, i);
        session->app = app;
        session->app_class = app->app_class;
        strlcpy(session->title,
                app->name != NULL ? app->name : "app",
                sizeof(session->title));
        portEXIT_CRITICAL(&port_shells_lock);
        return session;
    }
    portEXIT_CRITICAL(&port_shells_lock);
    return NULL;
}

static void port_app_owner(const port_app_session_t *session,
                           char *owner,
                           size_t owner_len)
{
    if (owner == NULL || owner_len == 0U) {
        return;
    }
    if (session == NULL) {
        strlcpy(owner, "session:?", owner_len);
        return;
    }
    snprintf(owner, owner_len, "session:%u", (unsigned)session->id);
}

static void port_app_update_title(port_shell_state_t *state,
                                  port_app_session_t *session)
{
    if (state == NULL || session == NULL || session->app == NULL) {
        return;
    }
    char title[sizeof(session->title)] = {0};
    if (session->app->title != NULL) {
        session->app->title(&state->ctx,
                            title,
                            sizeof(title));
    }
    if (title[0] == '\0') {
        strlcpy(title,
                session->app->name != NULL ? session->app->name : "app",
                sizeof(title));
    }
    portENTER_CRITICAL(&port_shells_lock);
    strlcpy(session->title, title, sizeof(session->title));
    portEXIT_CRITICAL(&port_shells_lock);
}

static void port_app_release(port_app_session_t *session)
{
    char owner[SOLAR_OS_APP_OWNER_MAX];

    if (session == NULL || session->app == NULL) {
        return;
    }

    port_app_owner(session, owner, sizeof(owner));
    solar_os_app_registry_release(session->app, owner);
}

static void port_shell_reset_terminal_state(port_shell_state_t *state)
{
    solar_os_shell_io_t *io =
        state != NULL && state->session != NULL ?
            solar_os_shell_session_io(state->session) : NULL;
    if (io == NULL) {
        return;
    }

    (void)solar_os_shell_io_set_bold(io, false);
    (void)solar_os_shell_io_set_italic(io, false);
    (void)solar_os_shell_io_set_underline(io, false);
    (void)solar_os_shell_io_set_inverse(io, false);
    (void)solar_os_shell_io_set_cursor_visible(io, true);
}

static void port_shell_show_prompt(port_shell_state_t *state,
                                   bool allow_terminal_preserve)
{
    if (state == NULL || state->session == NULL) {
        return;
    }
    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    solar_os_context_set_app_class(&state->ctx,
                                   solar_os_shell_app()->app_class);
    port_shell_reset_terminal_state(state);
    const bool preserve_terminal =
        solar_os_context_take_terminal_preserve(&state->ctx);
    if (io != NULL && (!allow_terminal_preserve || !preserve_terminal)) {
        solar_os_shell_io_clear(io);
    } else if (io != NULL && solar_os_shell_io_cursor_col(io) != 0U) {
        solar_os_shell_io_newline(io);
    }
    solar_os_shell_session_prompt(&state->ctx, state->session);
}

static void port_shell_clear_for_transition(port_shell_state_t *state)
{
    if (state == NULL || state->session == NULL) {
        return;
    }
    (void)solar_os_context_take_terminal_preserve(&state->ctx);
    port_shell_reset_terminal_state(state);
    (void)solar_os_shell_io_clear(
        solar_os_shell_session_io(state->session));
}

static esp_err_t port_app_resume(port_shell_state_t *state,
                                 port_app_session_t *session)
{
    if (state == NULL || session == NULL || !session->used ||
        session->app == NULL || state->active_app_session != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&port_shells_lock);
    state->active_app_session = session;
    state->last_app_session_id = session->id;
    session->suspended = false;
    portEXIT_CRITICAL(&port_shells_lock);
    solar_os_shell_session_set_foreground_app(state->session, session->app);
    solar_os_context_set_app_class(&state->ctx, session->app_class);
    if (session->started && session->app->resume != NULL) {
        port_shell_reset_terminal_state(state);
        (void)solar_os_shell_io_clear(
            solar_os_shell_session_io(state->session));
        session->app->resume(&state->ctx);
    }
    session->app_class = solar_os_context_app_class(&state->ctx);
    port_app_update_title(state, session);
    return ESP_OK;
}

static esp_err_t port_app_suspend(port_shell_state_t *state,
                                  bool show_prompt)
{
    port_app_session_t *session =
        state != NULL ? state->active_app_session : NULL;
    if (session == NULL || session->app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((session->app->flags & SOLAR_OS_APP_FLAG_RESUMABLE) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (session->app->suspend != NULL) {
        session->app->suspend(&state->ctx);
    }
    port_app_update_title(state, session);
    portENTER_CRITICAL(&port_shells_lock);
    session->suspended = true;
    state->last_app_session_id = session->id;
    state->active_app_session = NULL;
    portEXIT_CRITICAL(&port_shells_lock);
    solar_os_shell_session_set_foreground_app(state->session, NULL);
    (void)solar_os_context_take_exit_request(&state->ctx);
    if (show_prompt) {
        port_shell_show_prompt(state, false);
    }
    return ESP_OK;
}

static esp_err_t port_app_start(port_shell_state_t *state,
                                const solar_os_app_t *app,
                                port_app_session_t *parent,
                                port_app_session_t **started_session)
{
    if (started_session != NULL) {
        *started_session = NULL;
    }
    if (state == NULL || app == NULL || state->active_app_session != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    port_app_session_t *session = port_app_find(state, app);
    if (session != NULL) {
        portENTER_CRITICAL(&port_shells_lock);
        session->has_return_session = parent != NULL;
        session->return_session_id = parent != NULL ? parent->id : 0U;
        portEXIT_CRITICAL(&port_shells_lock);
        const esp_err_t err = port_app_resume(state, session);
        if (err == ESP_OK && started_session != NULL) {
            *started_session = session;
        }
        return err;
    }

    session = port_app_alloc(state, app);
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char owner[SOLAR_OS_APP_OWNER_MAX];
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
    port_app_owner(session, owner, sizeof(owner));
    esp_err_t err = solar_os_app_registry_claim(app,
                                                owner,
                                                busy_owner,
                                                sizeof(busy_owner));
    if (err != ESP_OK) {
        portENTER_CRITICAL(&port_shells_lock);
        memset(session, 0, sizeof(*session));
        portEXIT_CRITICAL(&port_shells_lock);
        return err;
    }

    portENTER_CRITICAL(&port_shells_lock);
    session->has_return_session = parent != NULL;
    session->return_session_id = parent != NULL ? parent->id : 0U;
    portEXIT_CRITICAL(&port_shells_lock);
    portENTER_CRITICAL(&port_shells_lock);
    state->active_app_session = session;
    state->last_app_session_id = session->id;
    portEXIT_CRITICAL(&port_shells_lock);
    solar_os_shell_session_set_foreground_app(state->session, app);
    (void)solar_os_context_take_terminal_preserve(&state->ctx);
    port_shell_reset_terminal_state(state);
    err = solar_os_app_start(app, &state->ctx);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&port_shells_lock);
        state->active_app_session = NULL;
        portEXIT_CRITICAL(&port_shells_lock);
        solar_os_shell_session_set_foreground_app(state->session, NULL);
        port_app_release(session);
        portENTER_CRITICAL(&port_shells_lock);
        memset(session, 0, sizeof(*session));
        portEXIT_CRITICAL(&port_shells_lock);
        return err;
    }

    portENTER_CRITICAL(&port_shells_lock);
    session->started = true;
    session->app_class = solar_os_context_app_class(&state->ctx);
    portEXIT_CRITICAL(&port_shells_lock);
    if (solar_os_context_take_exit_request(&state->ctx)) {
        return port_app_close(state,
                              session->id,
                              true,
                              true,
                              true);
    }
    port_app_update_title(state, session);
    if (started_session != NULL) {
        *started_session = session;
    }
    return ESP_OK;
}

static esp_err_t port_app_foreground(port_shell_state_t *state,
                                     uint8_t session_id)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    port_app_session_t *target = port_app_by_id(state, session_id);
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (target == state->active_app_session) {
        return ESP_OK;
    }

    if (state->active_app_session != NULL) {
        const esp_err_t suspend_err = port_app_suspend(state, false);
        if (suspend_err != ESP_OK) {
            return suspend_err;
        }
    }
    return port_app_resume(state, target);
}

static esp_err_t port_app_close(port_shell_state_t *state,
                                uint8_t session_id,
                                bool return_to_parent,
                                bool show_prompt_when_idle,
                                bool allow_terminal_preserve)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    port_app_session_t *session = port_app_by_id(state, session_id);
    if (session == NULL || session->app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    port_app_session_t *previous_active = state->active_app_session;
    const bool was_active = previous_active == session;
    if (!was_active && previous_active != NULL) {
        const esp_err_t suspend_err = port_app_suspend(state, false);
        if (suspend_err != ESP_OK) {
            return suspend_err;
        }
    } else if (was_active) {
        portENTER_CRITICAL(&port_shells_lock);
        state->active_app_session = NULL;
        portEXIT_CRITICAL(&port_shells_lock);
        solar_os_shell_session_set_foreground_app(state->session, NULL);
    }

    port_app_session_t *parent =
        session->has_return_session ?
            port_app_by_id(state, session->return_session_id) : NULL;
    const solar_os_app_t *app = session->app;
    solar_os_app_stop(app, &state->ctx);
    int exit_code = 0;
    const bool has_exit_result =
        solar_os_context_take_exit_result(&state->ctx, &exit_code);
    char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX] = {0};
    const bool has_message = solar_os_context_take_status_message(
        &state->ctx, message, sizeof(message));
    if (has_exit_result || has_message) {
        solar_os_shell_session_set_exit_result(state->session,
                                               has_exit_result ? exit_code : 0,
                                               has_message ? message : NULL);
    }
    port_app_release(session);
    portENTER_CRITICAL(&port_shells_lock);
    memset(session, 0, sizeof(*session));
    portEXIT_CRITICAL(&port_shells_lock);
    (void)solar_os_context_take_exit_request(&state->ctx);

    for (size_t i = 0; i < PORT_APP_SESSION_MAX; i++) {
        port_app_session_t *other = &state->app_sessions[i];
        if (other->used &&
            other->has_return_session &&
            other->return_session_id == session_id) {
            portENTER_CRITICAL(&port_shells_lock);
            other->has_return_session = false;
            other->return_session_id = 0U;
            portEXIT_CRITICAL(&port_shells_lock);
        }
    }

    if (!was_active && previous_active != NULL && previous_active->used) {
        return port_app_resume(state, previous_active);
    }
    if (!was_active) {
        if (show_prompt_when_idle) {
            port_shell_show_prompt(state, allow_terminal_preserve);
        } else {
            port_shell_clear_for_transition(state);
        }
        return ESP_OK;
    }
    if (return_to_parent && parent != NULL && parent->used) {
        return port_app_resume(state, parent);
    }
    if (show_prompt_when_idle) {
        port_shell_show_prompt(state, allow_terminal_preserve);
    } else {
        port_shell_clear_for_transition(state);
    }
    return ESP_OK;
}

static esp_err_t port_app_launch(port_shell_state_t *state,
                                 const solar_os_app_t *app,
                                 solar_os_launch_policy_t policy)
{
    if (state == NULL || app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    port_app_session_t *parent = state->active_app_session;
    if (parent != NULL) {
        if (policy != SOLAR_OS_LAUNCH_CHILD_RETURN) {
            const esp_err_t close_err =
                port_app_close(state, parent->id, false, false, false);
            if (close_err != ESP_OK) {
                return close_err;
            }
            parent = NULL;
        } else {
            const esp_err_t suspend_err = port_app_suspend(state, false);
            if (suspend_err != ESP_OK) {
                return suspend_err;
            }
        }
    }

    const esp_err_t start_err = port_app_start(state, app, parent, NULL);
    if (start_err != ESP_OK && parent != NULL && parent->used) {
        port_app_report_launch_failure(state, app, start_err);
        solar_os_shell_io_flush(
            solar_os_shell_session_io(state->session));
        (void)port_app_resume(state, parent);
        return ESP_OK;
    }
    return start_err;
}

static bool port_shell_emit_char(char ch, void *user)
{
    port_shell_state_t *state = (port_shell_state_t *)user;

    if (state == NULL || state->session == NULL || port_shell_should_stop(state)) {
        return false;
    }

    if ((uint8_t)ch == PORT_APP_SUSPEND_CHAR &&
        state->active_app_session != NULL) {
        const esp_err_t err = port_app_suspend(state, true);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(
                solar_os_shell_session_io(state->session),
                "this application cannot be suspended; use Ctrl+] to close it");
            solar_os_shell_io_flush(solar_os_shell_session_io(state->session));
        }
        return !port_shell_should_stop(state);
    }

    solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_CHAR,
        .data.ch = ch,
    };

    const solar_os_app_t *foreground_app = port_shell_foreground_app(state);
    if (foreground_app != NULL && foreground_app->event != NULL) {
        (void)foreground_app->event(&state->ctx, &event);
    } else {
        (void)solar_os_shell_session_event(&state->ctx, state->session, &event);
    }

    if (solar_os_context_take_sleep_request(&state->ctx)) {
        solar_os_shell_io_writeln(solar_os_shell_session_io(state->session),
                                  "sleep is only available from the display shell");
    }
    if (solar_os_context_take_suspend_request(&state->ctx)) {
        solar_os_shell_io_writeln(solar_os_shell_session_io(state->session),
                                  "suspend is only available from the display shell");
    }
    port_shell_process_requests(state);
    return !port_shell_should_stop(state);
}

static void port_shell_send_tick(port_shell_state_t *state, uint32_t now_ms)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    const solar_os_app_t *foreground_app = port_shell_foreground_app(state);
    const solar_os_app_t *tick_app = foreground_app != NULL ?
        foreground_app : solar_os_shell_app();
    if (state->tick_app != tick_app) {
        state->tick_app = tick_app;
        solar_os_tick_stats_reset(&state->tick_stats);
    }
    if (!solar_os_tick_due(&state->tick_stats,
                           solar_os_app_tick_interval_ms(
                               tick_app,
                               PORT_SHELL_TICK_MS),
                           tick_app->tick_deadline_ms,
                           PORT_SHELL_TICK_MS,
                           SOLAR_OS_TICK_DEADLINE_DEFAULT_MS,
                           now_ms)) {
        return;
    }

    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };
    const int64_t started_us = solar_os_tick_begin();
    if (foreground_app != NULL && foreground_app->event != NULL) {
        (void)foreground_app->event(&state->ctx, &event);
    } else {
        (void)solar_os_shell_session_event(&state->ctx, state->session, &event);
    }
    const bool missed = solar_os_tick_end(&state->tick_stats, started_us);
    if (state->active_app_session != NULL) {
        portENTER_CRITICAL(&port_shells_lock);
        state->active_app_session->tick_stats = state->tick_stats;
        portEXIT_CRITICAL(&port_shells_lock);
    }
    if (missed &&
        solar_os_tick_should_log_miss(&state->tick_stats)) {
        SOLAR_OS_LOGW(TAG,
                      "tick miss: #%u %s %" PRIu32 "us>%" PRIu32 "ms n=%" PRIu32,
                      (unsigned)state->id,
                      tick_app->name != NULL ? tick_app->name : "?",
                      state->tick_stats.last_duration_us,
                      state->tick_stats.deadline_ms,
                      state->tick_stats.deadline_miss_count);
    }
}

static uint32_t port_shell_read_timeout_ms(port_shell_state_t *state)
{
    const solar_os_app_t *foreground_app =
        port_shell_foreground_app(state);
    const solar_os_app_t *tick_app = foreground_app != NULL ?
        foreground_app : solar_os_shell_app();
    const uint32_t tick_interval_ms =
        solar_os_app_tick_interval_ms(tick_app,
                                      PORT_SHELL_TICK_MS);

    return tick_interval_ms < PORT_SHELL_READ_TIMEOUT_MS ?
        tick_interval_ms : PORT_SHELL_READ_TIMEOUT_MS;
}

static void port_shell_apply_dimensions(port_shell_state_t *state)
{
    uint16_t cols = 0;
    uint16_t rows = 0;

    if (state == NULL || state->session == NULL) {
        return;
    }

    portENTER_CRITICAL(&port_shells_lock);
    if (state->used && state->dimensions_pending) {
        cols = state->pending_cols;
        rows = state->pending_rows;
        state->dimensions_pending = false;
    }
    portEXIT_CRITICAL(&port_shells_lock);

    if (cols != 0 && rows != 0) {
        solar_os_shell_io_set_dimensions(solar_os_shell_session_io(state->session),
                                         cols,
                                         rows);
    }
}

static void port_shell_return_to_shell(port_shell_state_t *state)
{
    if (state == NULL ||
        state->session == NULL ||
        state->active_app_session == NULL) {
        return;
    }
    const uint8_t active_id = state->active_app_session->id;
    (void)port_app_close(state, active_id, true, true, true);
}

static void port_shell_process_requests(port_shell_state_t *state)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    if (solar_os_context_take_exit_request(&state->ctx)) {
        if (port_shell_foreground_app(state) != NULL) {
            port_shell_return_to_shell(state);
        } else {
            portENTER_CRITICAL(&port_shells_lock);
            if (state->used) {
                state->stop_requested = true;
            }
            portEXIT_CRITICAL(&port_shells_lock);
        }
        return;
    }

    const solar_os_app_t *requested_app = solar_os_context_take_launch_request(&state->ctx);
    if (requested_app == NULL) {
        return;
    }
    const solar_os_launch_policy_t policy =
        solar_os_context_take_launch_policy(&state->ctx);
    const esp_err_t launch_err =
        port_app_launch(state, requested_app, policy);
    if (launch_err != ESP_OK) {
        port_app_report_launch_failure(state, requested_app, launch_err);
        if (state->active_app_session == NULL) {
            solar_os_shell_session_prompt(&state->ctx, state->session);
        }
    }
}

static void port_shell_cleanup(port_shell_state_t *state, uint32_t generation)
{
    if (state == NULL) {
        return;
    }
    portENTER_CRITICAL(&port_shells_lock);
    if (state->used && state->generation == generation) {
        state->stop_requested = true;
    }
    portEXIT_CRITICAL(&port_shells_lock);

    if (state->session != NULL) {
        portENTER_CRITICAL(&port_shells_lock);
        state->active_app_session = NULL;
        portEXIT_CRITICAL(&port_shells_lock);
        solar_os_shell_session_set_foreground_app(state->session, NULL);
        for (size_t i = 0; i < PORT_APP_SESSION_MAX; i++) {
            port_app_session_t *app_session = &state->app_sessions[i];
            if (!app_session->used || app_session->app == NULL) {
                continue;
            }
            solar_os_app_stop(app_session->app, &state->ctx);
            port_app_release(app_session);
            portENTER_CRITICAL(&port_shells_lock);
            memset(app_session, 0, sizeof(*app_session));
            portEXIT_CRITICAL(&port_shells_lock);
        }

        solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
        if (io != NULL && solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_NONE) {
            solar_os_shell_io_set_cursor_visible(io, true);
            solar_os_shell_io_newline(io);
            solar_os_shell_io_writeln(io, "shell stopped");
            solar_os_shell_io_flush(io);
        }
        solar_os_context_detach_shell_session(&state->ctx, state->session);
        solar_os_shell_session_destroy(state->session);
        state->session = NULL;
    }

    if (state->app_operations != NULL) {
        port_app_operation_request_t *request = NULL;
        while (xQueueReceive(state->app_operations, &request, 0) == pdTRUE) {
            if (request != NULL) {
                request->result = ESP_ERR_INVALID_STATE;
                (void)xSemaphoreGive(request->complete);
            }
        }
        while (true) {
            portENTER_CRITICAL(&port_shells_lock);
            const uint32_t users = state->app_operation_users;
            portEXIT_CRITICAL(&port_shells_lock);
            if (users == 0U) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        solar_os_queue_delete(state->app_operations);
        state->app_operations = NULL;
    }

    if (solar_os_port_handle_valid(&state->port)) {
        (void)solar_os_port_release(&state->port);
    }

    portENTER_CRITICAL(&port_shells_lock);
    if (state->used && state->generation == generation) {
        memset(state, 0, sizeof(*state));
        state->generation = generation;
        state->port = (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
    }
    portEXIT_CRITICAL(&port_shells_lock);
}

static void port_shell_run(port_shell_state_t *state)
{
    uint8_t buffer[PORT_SHELL_READ_BUF];
    uint32_t last_input_ms = port_shell_now_ms();
    uint32_t generation = 0;

    portENTER_CRITICAL(&port_shells_lock);
    if (!state->used) {
        portEXIT_CRITICAL(&port_shells_lock);
        return;
    }
    generation = state->generation;
    state->task = xTaskGetCurrentTaskHandle();
    state->running = true;
    portEXIT_CRITICAL(&port_shells_lock);

    solar_os_vt100_input_init(&state->input);
    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    if (state->requested_terminal_profile == SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO) {
        const bool detected = port_shell_probe_terminal_identity(state);
        solar_os_shell_io_set_terminal_profile(
            io,
            detected ?
                SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100 :
                SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB);
        SOLAR_OS_LOGI(TAG,
                      "terminal profile on %s: %s%s",
                      state->port_name,
                      solar_os_shell_terminal_profile_name(solar_os_shell_io_terminal_profile(io)),
                      detected ? " (auto)" : " (auto fallback)");
    } else {
        solar_os_shell_io_set_terminal_profile(io, state->requested_terminal_profile);
    }
    portENTER_CRITICAL(&port_shells_lock);
    state->terminal_profile = solar_os_shell_io_terminal_profile(io);
    portEXIT_CRITICAL(&port_shells_lock);

    if (state->configured_size) {
        solar_os_shell_io_set_dimensions(io, state->configured_cols, state->configured_rows);
    } else if (solar_os_shell_io_terminal_profile(io) == SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100) {
        port_shell_probe_terminal_size(state);
    }

    port_shell_reset_terminal_state(state);
    esp_err_t err = solar_os_shell_session_start(&state->ctx,
                                                 state->session,
                                                 solar_os_shell_session_io(state->session),
                                                 false,
                                                 state->run_startup);
    if (err != ESP_OK) {
        state->last_error = err;
        SOLAR_OS_LOGW(TAG, "session start failed on %s: %s", state->port_name, esp_err_to_name(err));
        port_shell_cleanup(state, generation);
        return;
    }

    SOLAR_OS_LOGI(TAG,
                  "session %u shell started on %s",
                  (unsigned)state->id,
                  state->port_name);

    while (!port_shell_should_stop(state)) {
        port_shell_apply_dimensions(state);
        port_shell_process_app_operations(state);

        size_t read_len = 0;
        err = solar_os_port_read(&state->port,
                                                 buffer,
                                                 sizeof(buffer),
                                                 port_shell_read_timeout_ms(state),
                                                 &read_len);
        const uint32_t now_ms = port_shell_now_ms();
        if (err == ESP_OK && read_len > 0) {
            (void)solar_os_vt100_input_feed(&state->input,
                                            buffer,
                                            read_len,
                                            port_shell_emit_char,
                                            state);
            last_input_ms = now_ms;
        } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            state->last_error = err;
        }

        if (solar_os_vt100_input_pending(&state->input) &&
            (uint32_t)(now_ms - last_input_ms) >= PORT_SHELL_ESC_FLUSH_MS) {
            (void)solar_os_vt100_input_flush(&state->input, port_shell_emit_char, state);
        }
        port_shell_process_requests(state);
        port_shell_process_app_operations(state);

        port_shell_send_tick(state, now_ms);
        port_shell_process_requests(state);
        port_shell_process_app_operations(state);
    }

    SOLAR_OS_LOGI(TAG,
                  "session %u shell stopped on %s stack_high_water=%u",
                  (unsigned)state->id,
                  state->port_name,
                  (unsigned)uxTaskGetStackHighWaterMark(NULL));
    port_shell_cleanup(state, generation);
}

static void port_shell_task(void *arg)
{
    port_shell_run((port_shell_state_t *)arg);
    solar_os_task_delete_internal(NULL);
}

static void port_shell_reserved_worker(void *arg)
{
    (void)arg;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        portENTER_CRITICAL(&port_shells_lock);
        port_shell_state_t *state = port_shell_reserved_state;
        portEXIT_CRITICAL(&port_shells_lock);

        if (state != NULL) {
            port_shell_run(state);
        }

        portENTER_CRITICAL(&port_shells_lock);
        if (port_shell_reserved_state == state) {
            port_shell_reserved_state = NULL;
        }
        portEXIT_CRITICAL(&port_shells_lock);
    }
}

esp_err_t solar_os_port_shell_init(bool reserve_worker)
{
    if (!reserve_worker) {
        SOLAR_OS_LOGI(TAG, "port shell workers allocated on demand");
        return ESP_OK;
    }

    portENTER_CRITICAL(&port_shells_lock);
    if (port_shell_reserved_task != NULL) {
        portEXIT_CRITICAL(&port_shells_lock);
        return ESP_OK;
    }
    if (port_shell_reserved_initializing) {
        portEXIT_CRITICAL(&port_shells_lock);
        return ESP_ERR_INVALID_STATE;
    }
    port_shell_reserved_initializing = true;
    portEXIT_CRITICAL(&port_shells_lock);

    TaskHandle_t task = NULL;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        port_shell_reserved_worker,
        "port_shell_rsv",
        PORT_SHELL_TASK_STACK,
        NULL,
        PORT_SHELL_TASK_PRIORITY,
        &task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_SYSTEM);

    portENTER_CRITICAL(&port_shells_lock);
    port_shell_reserved_initializing = false;
    if (created == pdPASS) {
        port_shell_reserved_task = task;
    }
    portEXIT_CRITICAL(&port_shells_lock);

    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    SOLAR_OS_LOGI(TAG,
                  "reserved one %u-byte internal shell stack",
                  (unsigned)PORT_SHELL_TASK_STACK);
    return ESP_OK;
}

static esp_err_t port_shell_validate_port(const char *name)
{
    solar_os_port_info_t info;

    const esp_err_t err = solar_os_port_get_info(name, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (info.claimed) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((info.capabilities & (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) !=
        (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static port_shell_state_t *port_shell_by_id_locked(uint8_t session_id)
{
    if (session_id < SOLAR_OS_PORT_SHELL_SESSION_ID_BASE) {
        return NULL;
    }

    const size_t index = (size_t)(session_id - SOLAR_OS_PORT_SHELL_SESSION_ID_BASE);
    if (index >= PORT_SHELL_MAX || !port_shells[index].used) {
        return NULL;
    }
    return &port_shells[index];
}

static port_shell_state_t *port_shell_for_app_id_locked(uint8_t session_id)
{
    const uint8_t app_limit =
        (uint8_t)(SOLAR_OS_PORT_APP_SESSION_ID_BASE +
                  PORT_SHELL_MAX * PORT_APP_SESSION_MAX);
    if (session_id < SOLAR_OS_PORT_APP_SESSION_ID_BASE ||
        session_id >= app_limit) {
        return NULL;
    }
    const size_t shell_index =
        (size_t)(session_id - SOLAR_OS_PORT_APP_SESSION_ID_BASE) /
        PORT_APP_SESSION_MAX;
    return port_shells[shell_index].used ? &port_shells[shell_index] : NULL;
}

static port_shell_state_t *port_shell_for_context_locked(
    const solar_os_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (port_shells[i].used && &port_shells[i].ctx == ctx) {
            return &port_shells[i];
        }
    }
    return NULL;
}

static esp_err_t port_app_submit(port_shell_state_t *state,
                                 port_app_operation_type_t type,
                                 uint8_t session_id)
{
    if (state == NULL || state->app_operations == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (state->task == xTaskGetCurrentTaskHandle()) {
        return type == PORT_APP_OPERATION_FOREGROUND ?
            port_app_foreground(state, session_id) :
            port_app_close(state, session_id, false, false, false);
    }

    port_app_operation_request_t request = {
        .type = type,
        .session_id = session_id,
        .complete = xSemaphoreCreateBinary(),
        .result = ESP_ERR_INVALID_STATE,
    };
    if (request.complete == NULL) {
        return ESP_ERR_NO_MEM;
    }

    port_app_operation_request_t *queued_request = &request;
    portENTER_CRITICAL(&port_shells_lock);
    QueueHandle_t queue =
        state->used && !state->stop_requested ? state->app_operations : NULL;
    const BaseType_t queued =
        queue != NULL ? xQueueSend(queue, &queued_request, 0) : pdFALSE;
    portEXIT_CRITICAL(&port_shells_lock);
    if (queued != pdTRUE) {
        vSemaphoreDelete(request.complete);
        return queue != NULL ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
    }

    (void)xSemaphoreTake(request.complete, portMAX_DELAY);
    const esp_err_t result = request.result;
    vSemaphoreDelete(request.complete);
    return result;
}

static void port_shell_process_app_operations(port_shell_state_t *state)
{
    if (state == NULL || state->app_operations == NULL) {
        return;
    }
    port_app_operation_request_t *request = NULL;
    while (xQueueReceive(state->app_operations, &request, 0) == pdTRUE) {
        if (request == NULL) {
            continue;
        }
        request->result =
            request->type == PORT_APP_OPERATION_FOREGROUND ?
                port_app_foreground(state, request->session_id) :
                port_app_close(state,
                               request->session_id,
                               false,
                               true,
                               false);
        (void)xSemaphoreGive(request->complete);
    }
}

static port_shell_state_t *port_shell_alloc_locked(void)
{
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (port_shells[i].used) {
            continue;
        }
        port_shell_state_t *state = &port_shells[i];
        const uint32_t generation = state->generation + 1U;
        memset(state, 0, sizeof(*state));
        state->generation = generation != 0 ? generation : 1U;
        state->used = true;
        state->id = (uint8_t)(SOLAR_OS_PORT_SHELL_SESSION_ID_BASE + i);
        state->port = (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
        state->last_error = ESP_OK;
        return state;
    }
    return NULL;
}

bool solar_os_port_shell_is_session_id(uint8_t session_id)
{
    portENTER_CRITICAL(&port_shells_lock);
    const bool found = port_shell_by_id_locked(session_id) != NULL;
    portEXIT_CRITICAL(&port_shells_lock);
    return found;
}

bool solar_os_port_shell_context_owns_session(const solar_os_context_t *ctx,
                                              uint8_t session_id)
{
    portENTER_CRITICAL(&port_shells_lock);
    const port_shell_state_t *owner =
        port_shell_by_id_locked(session_id);
    const port_shell_state_t *caller =
        port_shell_for_context_locked(ctx);
    const bool matches = owner != NULL && owner == caller;
    portEXIT_CRITICAL(&port_shells_lock);
    return matches;
}

bool solar_os_port_shell_is_app_session_id(uint8_t session_id)
{
    const uint8_t app_limit =
        (uint8_t)(SOLAR_OS_PORT_APP_SESSION_ID_BASE +
                  PORT_SHELL_MAX * PORT_APP_SESSION_MAX);
    return session_id >= SOLAR_OS_PORT_APP_SESSION_ID_BASE &&
        session_id < app_limit;
}

bool solar_os_port_shell_context_owns_app_session(
    const solar_os_context_t *ctx,
    uint8_t session_id)
{
    portENTER_CRITICAL(&port_shells_lock);
    const port_shell_state_t *owner =
        port_shell_for_app_id_locked(session_id);
    const port_shell_state_t *caller =
        port_shell_for_context_locked(ctx);
    const bool matches = owner != NULL && owner == caller;
    portEXIT_CRITICAL(&port_shells_lock);
    return matches;
}

static void port_app_release_operation_user(port_shell_state_t *state)
{
    portENTER_CRITICAL(&port_shells_lock);
    if (state != NULL && state->app_operation_users > 0U) {
        state->app_operation_users--;
    }
    portEXIT_CRITICAL(&port_shells_lock);
}

esp_err_t solar_os_port_shell_foreground_app_session(
    solar_os_context_t *caller,
    uint8_t session_id)
{
    (void)caller;
    portENTER_CRITICAL(&port_shells_lock);
    port_shell_state_t *state =
        port_shell_for_app_id_locked(session_id);
    if (state != NULL && !state->stop_requested) {
        state->app_operation_users++;
    } else {
        state = NULL;
    }
    portEXIT_CRITICAL(&port_shells_lock);
    if (state == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t result =
        port_app_submit(state, PORT_APP_OPERATION_FOREGROUND, session_id);
    port_app_release_operation_user(state);
    return result;
}

esp_err_t solar_os_port_shell_foreground_last_app(
    solar_os_context_t *caller,
    uint8_t *session_id)
{
    if (session_id != NULL) {
        *session_id = 0U;
    }
    portENTER_CRITICAL(&port_shells_lock);
    port_shell_state_t *state =
        port_shell_for_context_locked(caller);
    uint8_t target_id = 0U;
    if (state != NULL && !state->stop_requested) {
        state->app_operation_users++;
        target_id = state->last_app_session_id;
    } else {
        state = NULL;
    }
    portEXIT_CRITICAL(&port_shells_lock);
    if (state == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (target_id == 0U) {
        port_app_release_operation_user(state);
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t result =
        port_app_submit(state, PORT_APP_OPERATION_FOREGROUND, target_id);
    port_app_release_operation_user(state);
    if (result == ESP_OK && session_id != NULL) {
        *session_id = target_id;
    }
    return result;
}

esp_err_t solar_os_port_shell_close_app_session(uint8_t session_id)
{
    portENTER_CRITICAL(&port_shells_lock);
    port_shell_state_t *state =
        port_shell_for_app_id_locked(session_id);
    if (state != NULL && !state->stop_requested) {
        state->app_operation_users++;
    } else {
        state = NULL;
    }
    portEXIT_CRITICAL(&port_shells_lock);
    if (state == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t result =
        port_app_submit(state, PORT_APP_OPERATION_CLOSE, session_id);
    port_app_release_operation_user(state);
    return result;
}

size_t solar_os_port_shell_session_count(void)
{
    size_t count = 0;

    portENTER_CRITICAL(&port_shells_lock);
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (port_shells[i].used) {
            count++;
        }
    }
    portEXIT_CRITICAL(&port_shells_lock);
    return count;
}

bool solar_os_port_shell_get_session_id(size_t index, uint8_t *session_id)
{
    size_t current = 0;

    if (session_id == NULL) {
        return false;
    }

    portENTER_CRITICAL(&port_shells_lock);
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (!port_shells[i].used) {
            continue;
        }
        if (current == index) {
            *session_id = port_shells[i].id;
            portEXIT_CRITICAL(&port_shells_lock);
            return true;
        }
        current++;
    }
    portEXIT_CRITICAL(&port_shells_lock);
    return false;
}

size_t solar_os_port_shell_app_session_count(void)
{
    size_t count = 0U;
    portENTER_CRITICAL(&port_shells_lock);
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        for (size_t j = 0; j < PORT_APP_SESSION_MAX; j++) {
            if (port_shells[i].used &&
                port_shells[i].app_sessions[j].used) {
                count++;
            }
        }
    }
    portEXIT_CRITICAL(&port_shells_lock);
    return count;
}

bool solar_os_port_shell_get_app_session_id(size_t index,
                                            uint8_t *session_id)
{
    if (session_id == NULL) {
        return false;
    }
    size_t current = 0U;
    portENTER_CRITICAL(&port_shells_lock);
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        for (size_t j = 0; j < PORT_APP_SESSION_MAX; j++) {
            const port_app_session_t *session =
                &port_shells[i].app_sessions[j];
            if (!port_shells[i].used || !session->used) {
                continue;
            }
            if (current == index) {
                *session_id = session->id;
                portEXIT_CRITICAL(&port_shells_lock);
                return true;
            }
            current++;
        }
    }
    portEXIT_CRITICAL(&port_shells_lock);
    return false;
}

esp_err_t solar_os_port_shell_start_with_options(solar_os_context_t *ctx,
                                                 const char *port_name,
                                                 const solar_os_port_shell_options_t *options,
                                                 bool run_startup,
                                                 uint8_t *session_id)
{
    solar_os_port_handle_t port = SOLAR_OS_PORT_HANDLE_INIT;
    solar_os_shell_session_t *session = NULL;
    solar_os_shell_terminal_profile_t requested_profile =
        SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO;
    solar_os_shell_charset_t charset = SOLAR_OS_SHELL_CHARSET_UTF8;
    bool configured_size = false;
    uint16_t cols = PORT_SHELL_DEFAULT_COLS;
    uint16_t rows = PORT_SHELL_DEFAULT_ROWS;

    if (ctx == NULL || port_name == NULL || port_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (options != NULL) {
        requested_profile = options->terminal_profile;
        if (!port_shell_terminal_profile_is_valid(requested_profile)) {
            return ESP_ERR_INVALID_ARG;
        }
        charset = options->charset;
        if (!port_shell_charset_is_valid(charset)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (options->cols != 0 || options->rows != 0) {
            if (!port_shell_dimensions_valid(options->cols, options->rows)) {
                return ESP_ERR_INVALID_ARG;
            }
            configured_size = true;
            cols = options->cols;
            rows = options->rows;
        }
    }
    esp_err_t err = port_shell_validate_port(port_name);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&port_shells_lock);
    port_shell_state_t *state = port_shell_alloc_locked();
    const uint32_t generation = state != NULL ? state->generation : 0;
    const uint8_t allocated_id = state != NULL ? state->id : 0;
    portEXIT_CRITICAL(&port_shells_lock);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char owner[SOLAR_OS_PORT_OWNER_MAX];
    port_shell_owner(state, owner, sizeof(owner));
    err = solar_os_port_claim(port_name, owner, &port);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&port_shells_lock);
        if (state->generation == generation) {
            state->used = false;
        }
        portEXIT_CRITICAL(&port_shells_lock);
        return err;
    }

    session = solar_os_shell_session_create();
    if (session == NULL) {
        (void)solar_os_port_release(&port);
        portENTER_CRITICAL(&port_shells_lock);
        if (state->generation == generation) {
            state->used = false;
        }
        portEXIT_CRITICAL(&port_shells_lock);
        return ESP_ERR_NO_MEM;
    }
    QueueHandle_t app_operations =
        solar_os_queue_create(PORT_APP_OPERATION_QUEUE_LEN,
                              sizeof(port_app_operation_request_t *));
    if (app_operations == NULL) {
        solar_os_shell_session_destroy(session);
        (void)solar_os_port_release(&port);
        portENTER_CRITICAL(&port_shells_lock);
        if (state->generation == generation) {
            state->used = false;
        }
        portEXIT_CRITICAL(&port_shells_lock);
        return ESP_ERR_NO_MEM;
    }

    memset(&state->ctx, 0, sizeof(state->ctx));
    solar_os_context_init(&state->ctx,
                          solar_os_context_terminal(ctx),
                          solar_os_context_gfx(ctx));
    solar_os_context_copy_session_handlers(&state->ctx, ctx);
    solar_os_shell_io_init_port(solar_os_shell_session_io(session),
                                &port,
                                cols,
                                rows);
    solar_os_shell_io_set_terminal_profile(solar_os_shell_session_io(session),
                                           requested_profile == SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO ?
                                               SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100 :
                                               requested_profile);
    solar_os_shell_io_set_charset(solar_os_shell_session_io(session), charset);

    portENTER_CRITICAL(&port_shells_lock);
    if (state->generation != generation || !state->used || state->stop_requested) {
        if (state->generation == generation) {
            state->used = false;
        }
        portEXIT_CRITICAL(&port_shells_lock);
        solar_os_queue_delete(app_operations);
        solar_os_shell_session_destroy(session);
        (void)solar_os_port_release(&port);
        return ESP_ERR_INVALID_STATE;
    }
    state->port = port;
    state->session = session;
    state->app_operations = app_operations;
    state->run_startup = run_startup;
    state->requested_terminal_profile = requested_profile;
    state->terminal_profile = requested_profile == SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO ?
        SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100 : requested_profile;
    state->configured_size = configured_size;
    state->configured_cols = cols;
    state->configured_rows = rows;
    state->last_error = ESP_OK;
    strlcpy(state->port_name, port_name, sizeof(state->port_name));
    portEXIT_CRITICAL(&port_shells_lock);

    TaskHandle_t created_task = NULL;
    bool using_reserved_task = false;
    portENTER_CRITICAL(&port_shells_lock);
    if (port_shell_reserved_task != NULL && port_shell_reserved_state == NULL) {
        port_shell_reserved_state = state;
        created_task = port_shell_reserved_task;
        state->task = created_task;
        using_reserved_task = true;
    }
    portEXIT_CRITICAL(&port_shells_lock);

    if (!using_reserved_task &&
        solar_os_task_create_pinned_internal(port_shell_task,
                                             "port_shell",
                                             PORT_SHELL_TASK_STACK,
                                             state,
                                             PORT_SHELL_TASK_PRIORITY,
                                             &created_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        portENTER_CRITICAL(&port_shells_lock);
        if (state->generation == generation) {
            const uint32_t failed_generation = state->generation;
            memset(state, 0, sizeof(*state));
            state->generation = failed_generation;
            state->port = (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
        }
        portEXIT_CRITICAL(&port_shells_lock);
        solar_os_queue_delete(app_operations);
        solar_os_shell_session_destroy(session);
        (void)solar_os_port_release(&port);
        return ESP_ERR_NO_MEM;
    }

    if (using_reserved_task) {
        xTaskNotifyGive(created_task);
    }
    portENTER_CRITICAL(&port_shells_lock);
    if (state->used && state->generation == generation && state->task == NULL) {
        state->task = created_task;
    }
    portEXIT_CRITICAL(&port_shells_lock);

    if (session_id != NULL) {
        *session_id = allocated_id;
    }
    return ESP_OK;
}

esp_err_t solar_os_port_shell_start(solar_os_context_t *ctx,
                                    const char *port_name,
                                    bool run_startup,
                                    uint8_t *session_id)
{
    return solar_os_port_shell_start_with_options(ctx,
                                                  port_name,
                                                  NULL,
                                                  run_startup,
                                                  session_id);
}

esp_err_t solar_os_port_shell_stop(uint8_t session_id)
{
    portENTER_CRITICAL(&port_shells_lock);
    port_shell_state_t *state = port_shell_by_id_locked(session_id);
    if (state == NULL) {
        portEXIT_CRITICAL(&port_shells_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t generation = state->generation;
    state->stop_requested = true;
    TaskHandle_t task = state->task;
    portEXIT_CRITICAL(&port_shells_lock);
    if (task != xTaskGetCurrentTaskHandle()) {
        for (uint32_t i = 0; i < 20; i++) {
            portENTER_CRITICAL(&port_shells_lock);
            const bool finished = !state->used || state->generation != generation;
            portEXIT_CRITICAL(&port_shells_lock);
            if (finished) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_port_shell_set_dimensions(uint8_t session_id,
                                             uint16_t cols,
                                             uint16_t rows)
{
    if (!port_shell_dimensions_valid(cols, rows)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&port_shells_lock);
    port_shell_state_t *state = port_shell_by_id_locked(session_id);
    if (state == NULL || state->stop_requested) {
        portEXIT_CRITICAL(&port_shells_lock);
        return ESP_ERR_NOT_FOUND;
    }
    state->pending_cols = cols;
    state->pending_rows = rows;
    state->dimensions_pending = true;
    portEXIT_CRITICAL(&port_shells_lock);
    return ESP_OK;
}

void solar_os_port_shell_print_list(solar_os_shell_io_t *io)
{
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        return;
    }

    typedef struct {
        bool used;
        bool active;
        bool suspended;
        uint8_t id;
        char title[48];
        char app_name[16];
        solar_os_tick_stats_t tick_stats;
    } port_app_list_entry_t;
    typedef struct {
        bool used;
        bool running;
        bool stop_requested;
        uint8_t id;
        solar_os_shell_terminal_profile_t terminal_profile;
        solar_os_tick_stats_t tick_stats;
        char port_name[SOLAR_OS_PORT_NAME_MAX];
        port_app_list_entry_t apps[PORT_APP_SESSION_MAX];
    } port_shell_list_entry_t;
    port_shell_list_entry_t entries[PORT_SHELL_MAX] = {0};

    portENTER_CRITICAL(&port_shells_lock);
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        entries[i].used = port_shells[i].used;
        entries[i].running = port_shells[i].running;
        entries[i].stop_requested = port_shells[i].stop_requested;
        entries[i].id = port_shells[i].id;
        entries[i].terminal_profile = port_shells[i].terminal_profile;
        entries[i].tick_stats = port_shells[i].tick_stats;
        strlcpy(entries[i].port_name, port_shells[i].port_name, sizeof(entries[i].port_name));
        for (size_t j = 0; j < PORT_APP_SESSION_MAX; j++) {
            const port_app_session_t *session = &port_shells[i].app_sessions[j];
            port_app_list_entry_t *app = &entries[i].apps[j];
            app->used = session->used;
            app->active = session == port_shells[i].active_app_session;
            app->suspended = session->suspended;
            app->id = session->id;
            app->tick_stats = session->tick_stats;
            strlcpy(app->title, session->title, sizeof(app->title));
            strlcpy(app->app_name,
                    session->app != NULL && session->app->name != NULL ?
                        session->app->name : "?",
                    sizeof(app->app_name));
        }
    }
    portEXIT_CRITICAL(&port_shells_lock);

    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        const port_shell_list_entry_t *entry = &entries[i];
        if (!entry->used) {
            continue;
        }
        const char *state_name = entry->stop_requested ? "stopping" :
            (entry->running ? "active" : "starting");
        char title[SOLAR_OS_PORT_NAME_MAX + 8];
        snprintf(title,
                 sizeof(title),
                 "%s/%s",
                 entry->port_name[0] != '\0' ? entry->port_name : "?",
                 solar_os_shell_terminal_profile_name(entry->terminal_profile));
        solar_os_shell_io_printf(io,
                                 "%-3u %-12.12s %-8s %-9.9s %-8.8s "
                                 "%" PRIu32 "/%" PRIu32 "ms %" PRIu32
                                 "/%" PRIu32 "us n=%" PRIu32 " !%" PRIu32 "\n",
                                 (unsigned)entry->id,
                                 title,
                                 "shell",
                                 state_name,
                                 entry->port_name,
                                 entry->tick_stats.interval_ms,
                                 entry->tick_stats.deadline_ms,
                                 entry->tick_stats.last_duration_us,
                                 entry->tick_stats.max_duration_us,
                                 entry->tick_stats.dispatch_count,
                                 entry->tick_stats.deadline_miss_count);
        for (size_t j = 0; j < PORT_APP_SESSION_MAX; j++) {
            const port_app_list_entry_t *app = &entry->apps[j];
            if (!app->used) {
                continue;
            }
            const char *app_state = app->active ? "active" :
                app->suspended ? "suspended" : "ready";
            solar_os_shell_io_printf(
                io,
                "%-3u %-12.12s %-8.8s %-9.9s %-8.8s "
                "%" PRIu32 "/%" PRIu32 "ms %" PRIu32
                "/%" PRIu32 "us n=%" PRIu32 " !%" PRIu32 "\n",
                (unsigned)app->id,
                app->title,
                app->app_name,
                app_state,
                entry->port_name,
                app->tick_stats.interval_ms,
                app->tick_stats.deadline_ms,
                app->tick_stats.last_duration_us,
                app->tick_stats.max_duration_us,
                app->tick_stats.dispatch_count,
                app->tick_stats.deadline_miss_count);
        }
    }
}
