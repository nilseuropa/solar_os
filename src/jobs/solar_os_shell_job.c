#include "solar_os_shell_job.h"

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

#define SHELL_JOB_TASK_STACK 8192
#define SHELL_JOB_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define SHELL_JOB_READ_BUF 64
#define SHELL_JOB_READ_TIMEOUT_MS 50U
#define SHELL_JOB_ESC_FLUSH_MS 40U
#define SHELL_JOB_TICK_MS 100U
#define SHELL_JOB_DEFAULT_COLS 80
#define SHELL_JOB_DEFAULT_ROWS 24
#define SHELL_JOB_SIZE_PROBE_TIMEOUT_MS 200U
#define SHELL_JOB_SIZE_PROBE_READ_MS 25U
#define SHELL_JOB_SIZE_PROBE_MIN_COLS 20U
#define SHELL_JOB_SIZE_PROBE_MIN_ROWS 8U
#define SHELL_JOB_SIZE_PROBE_MAX_COLS 300U
#define SHELL_JOB_SIZE_PROBE_MAX_ROWS 120U

static const char *TAG = "solar_os_shell_job";

typedef struct {
    bool running;
    volatile bool stop_requested;
    TaskHandle_t task;
    solar_os_port_handle_t port;
    solar_os_shell_session_t *session;
    solar_os_context_t ctx;
    solar_os_vt100_input_t input;
    char port_name[SOLAR_OS_PORT_NAME_MAX];
    esp_err_t last_error;
} shell_job_state_t;

static shell_job_state_t shell_job = {
    .port = SOLAR_OS_PORT_HANDLE_INIT,
    .last_error = ESP_OK,
};

static void shell_job_process_requests(shell_job_state_t *state);

static uint32_t shell_job_now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void shell_job_owner(const shell_job_state_t *state, char *owner, size_t owner_len)
{
    if (owner == NULL || owner_len == 0) {
        return;
    }

    snprintf(owner,
             owner_len,
             "shell:%s",
             state != NULL && state->port_name[0] != '\0' ? state->port_name : "?");
}

static const solar_os_app_t *shell_job_foreground_app(shell_job_state_t *state)
{
    return state != NULL ? solar_os_shell_session_foreground_app(state->session) : NULL;
}

static bool shell_job_parse_size_report(const uint8_t *data,
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
        if (parsed_cols < SHELL_JOB_SIZE_PROBE_MIN_COLS ||
            parsed_rows < SHELL_JOB_SIZE_PROBE_MIN_ROWS ||
            parsed_cols > SHELL_JOB_SIZE_PROBE_MAX_COLS ||
            parsed_rows > SHELL_JOB_SIZE_PROBE_MAX_ROWS) {
            continue;
        }

        *rows = (uint16_t)parsed_rows;
        *cols = (uint16_t)parsed_cols;
        return true;
    }

    return false;
}

static void shell_job_probe_terminal_size(shell_job_state_t *state)
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

    const char probe[] = "\x1b[?25h\x1b[999;999H\x1b[6n";
    (void)solar_os_shell_io_write_raw(io, probe, sizeof(probe) - 1U);

    const uint32_t start_ms = shell_job_now_ms();
    while ((uint32_t)(shell_job_now_ms() - start_ms) < SHELL_JOB_SIZE_PROBE_TIMEOUT_MS &&
           response_len < sizeof(response)) {
        size_t read_len = 0;
        const esp_err_t err = solar_os_port_read(&state->port,
                                                 response + response_len,
                                                 sizeof(response) - response_len,
                                                 SHELL_JOB_SIZE_PROBE_READ_MS,
                                                 &read_len);
        if (err == ESP_OK && read_len > 0) {
            response_len += read_len;
            if (shell_job_parse_size_report(response, response_len, &rows, &cols)) {
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

static void shell_job_release_foreground_app(shell_job_state_t *state, const solar_os_app_t *app)
{
    char owner[SOLAR_OS_APP_OWNER_MAX];

    if (state == NULL || app == NULL) {
        return;
    }

    shell_job_owner(state, owner, sizeof(owner));
    solar_os_app_registry_release(app, owner);
}

static bool shell_job_emit_char(char ch, void *user)
{
    shell_job_state_t *state = (shell_job_state_t *)user;

    if (state == NULL || state->session == NULL || state->stop_requested) {
        return false;
    }

    solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_CHAR,
        .data.ch = ch,
    };

    const solar_os_app_t *foreground_app = shell_job_foreground_app(state);
    if (foreground_app != NULL && foreground_app->event != NULL) {
        (void)foreground_app->event(&state->ctx, &event);
    } else {
        (void)solar_os_shell_session_event(&state->ctx, state->session, &event);
    }

    if (solar_os_context_take_sleep_request(&state->ctx)) {
        solar_os_shell_io_writeln(solar_os_shell_session_io(state->session),
                                  "sleep is only available from the display shell");
    }
    shell_job_process_requests(state);
    return !state->stop_requested;
}

static void shell_job_send_tick(shell_job_state_t *state, uint32_t now_ms)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };
    const solar_os_app_t *foreground_app = shell_job_foreground_app(state);
    if (foreground_app != NULL && foreground_app->event != NULL) {
        (void)foreground_app->event(&state->ctx, &event);
    } else {
        (void)solar_os_shell_session_event(&state->ctx, state->session, &event);
    }
}

static void shell_job_return_to_shell(shell_job_state_t *state)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    solar_os_shell_io_t *io = solar_os_shell_session_io(state->session);
    const solar_os_app_t *foreground_app = shell_job_foreground_app(state);

    if (foreground_app != NULL && foreground_app->stop != NULL) {
        foreground_app->stop(&state->ctx);
    }
    shell_job_release_foreground_app(state, foreground_app);
    solar_os_shell_session_set_foreground_app(state->session, NULL);
    (void)solar_os_context_take_exit_request(&state->ctx);

    const bool preserve_terminal = solar_os_context_take_terminal_preserve(&state->ctx);
    if (io != NULL && !preserve_terminal) {
        solar_os_shell_io_clear(io);
    }
    solar_os_shell_session_prompt(&state->ctx, state->session);
}

static void shell_job_process_requests(shell_job_state_t *state)
{
    if (state == NULL || state->session == NULL) {
        return;
    }

    if (solar_os_context_take_exit_request(&state->ctx)) {
        if (shell_job_foreground_app(state) != NULL) {
            shell_job_return_to_shell(state);
        }
        return;
    }

    const solar_os_app_t *requested_app = solar_os_context_take_launch_request(&state->ctx);
    if (requested_app == NULL) {
        return;
    }

    if (shell_job_foreground_app(state) != NULL) {
        solar_os_shell_io_writeln(solar_os_shell_session_io(state->session),
                                  "another foreground app is already running");
        solar_os_shell_session_prompt(&state->ctx, state->session);
        return;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
    shell_job_owner(state, owner, sizeof(owner));
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
        shell_job_release_foreground_app(state, requested_app);
        solar_os_shell_session_set_foreground_app(state->session, NULL);
        solar_os_shell_session_prompt(&state->ctx, state->session);
    }
}

static void shell_job_cleanup(shell_job_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (state->session != NULL) {
        const solar_os_app_t *foreground_app = shell_job_foreground_app(state);
        if (foreground_app != NULL && foreground_app->stop != NULL) {
            foreground_app->stop(&state->ctx);
        }
        shell_job_release_foreground_app(state, foreground_app);
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
    state->task = NULL;
    state->port_name[0] = '\0';
}

static void shell_job_task(void *arg)
{
    shell_job_state_t *state = (shell_job_state_t *)arg;
    uint8_t buffer[SHELL_JOB_READ_BUF];
    uint32_t last_tick_ms = shell_job_now_ms();
    uint32_t last_input_ms = last_tick_ms;

    solar_os_vt100_input_init(&state->input);
    shell_job_probe_terminal_size(state);

    esp_err_t err = solar_os_shell_session_start(&state->ctx,
                                                 state->session,
                                                 solar_os_shell_session_io(state->session),
                                                 false,
                                                 false);
    if (err != ESP_OK) {
        state->last_error = err;
        SOLAR_OS_LOGW(TAG, "session start failed on %s: %s", state->port_name, esp_err_to_name(err));
        shell_job_cleanup(state);
        vTaskDelete(NULL);
        return;
    }

    SOLAR_OS_LOGI(TAG, "shell session started on %s", state->port_name);

    while (!state->stop_requested) {
        size_t read_len = 0;
        err = solar_os_port_read(&state->port,
                                 buffer,
                                 sizeof(buffer),
                                 SHELL_JOB_READ_TIMEOUT_MS,
                                 &read_len);
        const uint32_t now_ms = shell_job_now_ms();
        if (err == ESP_OK && read_len > 0) {
            (void)solar_os_vt100_input_feed(&state->input,
                                            buffer,
                                            read_len,
                                            shell_job_emit_char,
                                            state);
            last_input_ms = now_ms;
        } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            state->last_error = err;
        }

        if (solar_os_vt100_input_pending(&state->input) &&
            (uint32_t)(now_ms - last_input_ms) >= SHELL_JOB_ESC_FLUSH_MS) {
            (void)solar_os_vt100_input_flush(&state->input, shell_job_emit_char, state);
        }
        shell_job_process_requests(state);

        if ((uint32_t)(now_ms - last_tick_ms) >= SHELL_JOB_TICK_MS) {
            last_tick_ms = now_ms;
            shell_job_send_tick(state, now_ms);
            shell_job_process_requests(state);
        }
    }

    SOLAR_OS_LOGI(TAG, "shell session stopped on %s", state->port_name);
    shell_job_cleanup(state);
    vTaskDelete(NULL);
}

static esp_err_t shell_job_validate_port(const char *name)
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

static esp_err_t shell_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    const char *port_name = NULL;
    solar_os_port_handle_t port = SOLAR_OS_PORT_HANDLE_INIT;
    solar_os_shell_session_t *session = NULL;

    if (ctx == NULL || argc != 2 || argv == NULL || argv[1] == NULL || argv[1][0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (shell_job.running || shell_job.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    port_name = argv[1];
    esp_err_t err = shell_job_validate_port(port_name);
    if (err != ESP_OK) {
        return err;
    }

    err = solar_os_port_claim(port_name, "shell", &port);
    if (err != ESP_OK) {
        return err;
    }

    session = solar_os_shell_session_create();
    if (session == NULL) {
        (void)solar_os_port_release(&port);
        return ESP_ERR_NO_MEM;
    }

    memset(&shell_job.ctx, 0, sizeof(shell_job.ctx));
    solar_os_context_init(&shell_job.ctx,
                          solar_os_context_terminal(ctx),
                          solar_os_context_gfx(ctx));
    solar_os_context_copy_session_handlers(&shell_job.ctx, ctx);
    solar_os_shell_io_init_port(solar_os_shell_session_io(session),
                                &port,
                                SHELL_JOB_DEFAULT_COLS,
                                SHELL_JOB_DEFAULT_ROWS);

    shell_job.port = port;
    shell_job.session = session;
    shell_job.stop_requested = false;
    shell_job.running = true;
    shell_job.last_error = ESP_OK;
    strlcpy(shell_job.port_name, port_name, sizeof(shell_job.port_name));

    if (xTaskCreate(shell_job_task,
                    "shell_job",
                    SHELL_JOB_TASK_STACK,
                    &shell_job,
                    SHELL_JOB_TASK_PRIORITY,
                    &shell_job.task) != pdPASS) {
        shell_job.task = NULL;
        shell_job.running = false;
        shell_job.session = NULL;
        shell_job.port_name[0] = '\0';
        solar_os_shell_session_destroy(session);
        (void)solar_os_port_release(&port);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void shell_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (!shell_job.running && shell_job.task == NULL) {
        return;
    }

    shell_job.stop_requested = true;
    if (shell_job.task != NULL && shell_job.task != xTaskGetCurrentTaskHandle()) {
        for (uint32_t i = 0; i < 20 && shell_job.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
}

const solar_os_job_t solar_os_shell_job = {
    .name = "shell",
    .summary = "VT100 shell on a byte-stream port",
    .start = shell_job_start,
    .stop = shell_job_stop,
    .event = NULL,
};
