#include "solar_os_port_shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_app_registry.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_vt100.h"

#define PORT_SHELL_MAX 4
#define PORT_SHELL_TASK_STACK 16384
#define PORT_SHELL_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
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

static const char *TAG = "solar_os_port_shell";

typedef struct {
    bool used;
    bool running;
    volatile bool stop_requested;
    uint8_t id;
    TaskHandle_t task;
    solar_os_port_handle_t port;
    solar_os_shell_session_t *session;
    solar_os_context_t ctx;
    solar_os_vt100_input_t input;
    bool run_startup;
    solar_os_shell_terminal_profile_t requested_terminal_profile;
    bool configured_size;
    uint16_t configured_cols;
    uint16_t configured_rows;
    char port_name[SOLAR_OS_PORT_NAME_MAX];
    esp_err_t last_error;
} port_shell_state_t;

static port_shell_state_t port_shells[PORT_SHELL_MAX];

static void port_shell_process_requests(port_shell_state_t *state);

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
    return state != NULL ? solar_os_shell_session_foreground_app(state->session) : NULL;
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

static void port_shell_release_foreground_app(port_shell_state_t *state,
                                              const solar_os_app_t *app)
{
    char owner[SOLAR_OS_APP_OWNER_MAX];

    if (state == NULL || app == NULL) {
        return;
    }

    port_shell_owner(state, owner, sizeof(owner));
    solar_os_app_registry_release(app, owner);
}

static bool port_shell_emit_char(char ch, void *user)
{
    port_shell_state_t *state = (port_shell_state_t *)user;

    if (state == NULL || state->session == NULL || state->stop_requested) {
        return false;
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
    port_shell_process_requests(state);
    return !state->stop_requested;
}

static void port_shell_send_tick(port_shell_state_t *state, uint32_t now_ms)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };
    const solar_os_app_t *foreground_app = port_shell_foreground_app(state);
    if (foreground_app != NULL && foreground_app->event != NULL) {
        (void)foreground_app->event(&state->ctx, &event);
    } else {
        (void)solar_os_shell_session_event(&state->ctx, state->session, &event);
    }
}

static void port_shell_return_to_shell(port_shell_state_t *state)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    const solar_os_app_t *foreground_app = port_shell_foreground_app(state);

    if (foreground_app != NULL && foreground_app->stop != NULL) {
        foreground_app->stop(&state->ctx);
    }
    port_shell_release_foreground_app(state, foreground_app);
    solar_os_shell_session_set_foreground_app(state->session, NULL);
    (void)solar_os_context_take_exit_request(&state->ctx);

    const bool preserve_terminal = solar_os_context_take_terminal_preserve(&state->ctx);
    if (io != NULL && !preserve_terminal) {
        solar_os_shell_io_clear(io);
    }
    solar_os_shell_session_prompt(&state->ctx, state->session);
}

static void port_shell_process_requests(port_shell_state_t *state)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    if (solar_os_context_take_exit_request(&state->ctx)) {
        if (port_shell_foreground_app(state) != NULL) {
            port_shell_return_to_shell(state);
        }
        return;
    }

    const solar_os_app_t *requested_app = solar_os_context_take_launch_request(&state->ctx);
    if (requested_app == NULL) {
        return;
    }
    (void)solar_os_context_take_launch_policy(&state->ctx);

    if (port_shell_foreground_app(state) != NULL) {
        solar_os_shell_io_writeln(solar_os_shell_session_io(state->session),
                                  "another foreground app is already running");
        solar_os_shell_session_prompt(&state->ctx, state->session);
        return;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
    port_shell_owner(state, owner, sizeof(owner));
    esp_err_t claim_err = solar_os_app_registry_claim(requested_app,
                                                      owner,
                                                      busy_owner,
                                                      sizeof(busy_owner));
    if (claim_err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(solar_os_shell_session_io(state->session),
                                 "%s: already running on %s\n",
                                 requested_app->name != NULL ? requested_app->name : "app",
                                 busy_owner[0] != '\0' ? busy_owner : "another session");
        solar_os_shell_session_prompt(&state->ctx, state->session);
        return;
    }
    if (claim_err != ESP_OK) {
        solar_os_shell_io_printf(solar_os_shell_session_io(state->session),
                                 "%s: launch failed: %s\n",
                                 requested_app->name != NULL ? requested_app->name : "app",
                                 esp_err_to_name(claim_err));
        solar_os_shell_session_prompt(&state->ctx, state->session);
        return;
    }

    solar_os_shell_session_set_foreground_app(state->session, requested_app);
    const esp_err_t start_err = requested_app->start != NULL ?
        requested_app->start(&state->ctx) :
        ESP_OK;
    if (start_err != ESP_OK) {
        solar_os_shell_io_printf(solar_os_shell_session_io(state->session),
                                 "%s: launch failed: %s\n",
                                 requested_app->name != NULL ? requested_app->name : "app",
                                 esp_err_to_name(start_err));
        port_shell_release_foreground_app(state, requested_app);
        solar_os_shell_session_set_foreground_app(state->session, NULL);
        solar_os_shell_session_prompt(&state->ctx, state->session);
    }
}

static void port_shell_cleanup(port_shell_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (state->session != NULL) {
        const solar_os_app_t *foreground_app = port_shell_foreground_app(state);
        if (foreground_app != NULL && foreground_app->stop != NULL) {
            foreground_app->stop(&state->ctx);
        }
        port_shell_release_foreground_app(state, foreground_app);
        solar_os_shell_session_set_foreground_app(state->session, NULL);

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

    if (solar_os_port_handle_valid(&state->port)) {
        (void)solar_os_port_release(&state->port);
    }

    state->running = false;
    state->stop_requested = false;
    state->run_startup = false;
    state->task = NULL;
    state->port_name[0] = '\0';
    state->used = false;
}

static void port_shell_task(void *arg)
{
    port_shell_state_t *state = (port_shell_state_t *)arg;
    uint8_t buffer[PORT_SHELL_READ_BUF];
    uint32_t last_tick_ms = port_shell_now_ms();
    uint32_t last_input_ms = last_tick_ms;

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

    if (state->configured_size) {
        solar_os_shell_io_set_dimensions(io, state->configured_cols, state->configured_rows);
    } else if (solar_os_shell_io_terminal_profile(io) == SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100) {
        port_shell_probe_terminal_size(state);
    }

    esp_err_t err = solar_os_shell_session_start(&state->ctx,
                                                 state->session,
                                                 solar_os_shell_session_io(state->session),
                                                 false,
                                                 state->run_startup);
    if (err != ESP_OK) {
        state->last_error = err;
        SOLAR_OS_LOGW(TAG, "session start failed on %s: %s", state->port_name, esp_err_to_name(err));
        port_shell_cleanup(state);
        vTaskDelete(NULL);
        return;
    }

    SOLAR_OS_LOGI(TAG,
                  "session %u shell started on %s",
                  (unsigned)state->id,
                  state->port_name);

    while (!state->stop_requested) {
        size_t read_len = 0;
        err = solar_os_port_read(&state->port,
                                                 buffer,
                                                 sizeof(buffer),
                                                 PORT_SHELL_READ_TIMEOUT_MS,
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

        if ((uint32_t)(now_ms - last_tick_ms) >= PORT_SHELL_TICK_MS) {
            last_tick_ms = now_ms;
            port_shell_send_tick(state, now_ms);
            port_shell_process_requests(state);
        }
    }

    SOLAR_OS_LOGI(TAG,
                  "session %u shell stopped on %s",
                  (unsigned)state->id,
                  state->port_name);
    port_shell_cleanup(state);
    vTaskDelete(NULL);
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

static port_shell_state_t *port_shell_by_id(uint8_t session_id)
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

static port_shell_state_t *port_shell_alloc(void)
{
    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (port_shells[i].used) {
            continue;
        }
        port_shell_state_t *state = &port_shells[i];
        memset(state, 0, sizeof(*state));
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
    return port_shell_by_id(session_id) != NULL;
}

size_t solar_os_port_shell_session_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (port_shells[i].used) {
            count++;
        }
    }
    return count;
}

bool solar_os_port_shell_get_session_id(size_t index, uint8_t *session_id)
{
    size_t current = 0;

    if (session_id == NULL) {
        return false;
    }

    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        if (!port_shells[i].used) {
            continue;
        }
        if (current == index) {
            *session_id = port_shells[i].id;
            return true;
        }
        current++;
    }
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
        if (options->cols != 0 || options->rows != 0) {
            if (options->cols < PORT_SHELL_SIZE_PROBE_MIN_COLS ||
                options->rows < PORT_SHELL_SIZE_PROBE_MIN_ROWS ||
                options->cols > PORT_SHELL_SIZE_PROBE_MAX_COLS ||
                options->rows > PORT_SHELL_SIZE_PROBE_MAX_ROWS) {
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

    port_shell_state_t *state = port_shell_alloc();
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char owner[SOLAR_OS_PORT_OWNER_MAX];
    port_shell_owner(state, owner, sizeof(owner));
    err = solar_os_port_claim(port_name, owner, &port);
    if (err != ESP_OK) {
        state->used = false;
        return err;
    }

    session = solar_os_shell_session_create();
    if (session == NULL) {
        (void)solar_os_port_release(&port);
        state->used = false;
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

    state->port = port;
    state->session = session;
    state->stop_requested = false;
    state->run_startup = run_startup;
    state->requested_terminal_profile = requested_profile;
    state->configured_size = configured_size;
    state->configured_cols = cols;
    state->configured_rows = rows;
    state->running = true;
    state->last_error = ESP_OK;
    strlcpy(state->port_name, port_name, sizeof(state->port_name));

    if (xTaskCreate(port_shell_task,
                    "port_shell",
                    PORT_SHELL_TASK_STACK,
                    state,
                    PORT_SHELL_TASK_PRIORITY,
                    &state->task) != pdPASS) {
        state->task = NULL;
        state->running = false;
        state->session = NULL;
        state->port_name[0] = '\0';
        state->used = false;
        solar_os_shell_session_destroy(session);
        (void)solar_os_port_release(&port);
        return ESP_ERR_NO_MEM;
    }

    if (session_id != NULL) {
        *session_id = state->id;
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
    port_shell_state_t *state = port_shell_by_id(session_id);
    if (state == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!state->running && state->task == NULL) {
        return ESP_OK;
    }

    state->stop_requested = true;
    if (state->task != NULL && state->task != xTaskGetCurrentTaskHandle()) {
        for (uint32_t i = 0; i < 20 && state->task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    return ESP_OK;
}

void solar_os_port_shell_print_list(solar_os_shell_io_t *io)
{
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        return;
    }

    for (size_t i = 0; i < PORT_SHELL_MAX; i++) {
        const port_shell_state_t *state = &port_shells[i];
        if (!state->used) {
            continue;
        }
        solar_os_shell_io_t *session_io = solar_os_shell_session_io(state->session);
        solar_os_shell_io_printf(io,
                                 "%-3u %-11s %-9s shell on %s term=%s\n",
                                 (unsigned)state->id,
                                 state->running ? "active" : "stopping",
                                 "shell",
                                 state->port_name[0] != '\0' ? state->port_name : "?",
                                 solar_os_shell_terminal_profile_name(
                                     solar_os_shell_io_terminal_profile(session_io)));
    }
}
