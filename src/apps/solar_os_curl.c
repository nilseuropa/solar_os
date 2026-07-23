#include "solar_os_curl.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_http_client.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_wifi.h"

#define CURL_TASK_STACK 12288
#define CURL_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define CURL_EVENT_QUEUE_LEN 10
#define CURL_EVENT_DATA_MAX 256
#define CURL_EVENT_MESSAGE_MAX 128
#define CURL_LOCATION_MAX 256
#define CURL_TIMEOUT_MS 10000
#define CURL_TERMINAL_LIMIT (16U * 1024U)
#define CURL_FILE_PROGRESS_STEP (32U * 1024U)

typedef enum {
    CURL_EVENT_STATUS,
    CURL_EVENT_PROGRESS,
    CURL_EVENT_DATA,
    CURL_EVENT_ERROR,
    CURL_EVENT_DONE,
} curl_event_type_t;

typedef struct {
    curl_event_type_t type;
    int status_code;
    uint32_t bytes_read;
    uint32_t bytes_written;
    uint32_t content_length;
    bool content_length_known;
    bool output_truncated;
    bool success;
    size_t data_len;
    uint8_t data[CURL_EVENT_DATA_MAX];
    char message[CURL_EVENT_MESSAGE_MAX];
} curl_event_t;

typedef struct {
    bool follow_redirects;
    bool output_to_file;
    char url[SOLAR_OS_APP_ARG_LEN];
    char output_path[SOLAR_OS_STORAGE_PATH_MAX];
} curl_options_t;

typedef struct {
    curl_options_t options;
    QueueHandle_t events;
    TaskHandle_t task;
    solar_os_http_request_t *request;
    FILE *output_file;
    volatile bool stop_requested;
    volatile bool task_done;
    bool running;
    bool done;
    bool saw_error;
    bool transfer_error;
    bool output_truncated;
    uint32_t bytes_read;
    uint32_t bytes_written;
    uint32_t terminal_bytes_sent;
    uint32_t next_progress;
    int status_code;
    int64_t content_length;
    char location[CURL_LOCATION_MAX];
    char transfer_error_message[CURL_EVENT_MESSAGE_MAX];
} curl_app_state_t;

static const char *TAG = "solar_os_curl";
static curl_app_state_t curl_app;
static solar_os_shell_io_t curl_fallback_io;

static solar_os_shell_io_t *curl_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&curl_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &curl_fallback_io);
        io = &curl_fallback_io;
    }
    return io;
}

static solar_os_terminal_t *curl_terminal(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = curl_io(ctx);
    solar_os_terminal_t *term = solar_os_shell_io_terminal(io);
    return term != NULL ? term : solar_os_context_terminal(ctx);
}

static void curl_format_error(char *buffer, size_t len, const char *prefix, esp_err_t err)
{
    if (buffer == NULL || len == 0) {
        return;
    }
    snprintf(buffer, len, "%s: %s", prefix, esp_err_to_name(err));
}

static bool curl_send_event(const curl_event_t *event)
{
    if (event == NULL || curl_app.events == NULL) {
        return false;
    }

    while (!curl_app.stop_requested) {
        if (xQueueSend(curl_app.events, event, pdMS_TO_TICKS(100)) == pdPASS) {
            return true;
        }
    }
    return false;
}

static void curl_send_message(curl_event_type_t type, const char *message)
{
    curl_event_t event = {
        .type = type,
    };
    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    }
    (void)curl_send_event(&event);
}

static void curl_cleanup_resources(void)
{
    if (curl_app.events != NULL) {
        solar_os_queue_delete(curl_app.events);
        curl_app.events = NULL;
    }
    if (curl_app.output_file != NULL) {
        fclose(curl_app.output_file);
        curl_app.output_file = NULL;
    }
    if (curl_app.request != NULL) {
        (void)solar_os_http_request_destroy(curl_app.request);
        curl_app.request = NULL;
    }
}

static void curl_set_transfer_error(const char *message)
{
    curl_app.transfer_error = true;
    if (message != NULL) {
        strlcpy(curl_app.transfer_error_message,
                message,
                sizeof(curl_app.transfer_error_message));
    }
}

static bool curl_send_data_chunk(const uint8_t *data, size_t len)
{
    while (len > 0 && !curl_app.stop_requested) {
        curl_event_t event = {
            .type = CURL_EVENT_DATA,
        };
        event.data_len = len > sizeof(event.data) ? sizeof(event.data) : len;
        memcpy(event.data, data, event.data_len);
        if (!curl_send_event(&event)) {
            return false;
        }

        data += event.data_len;
        len -= event.data_len;
    }
    return !curl_app.stop_requested;
}

static esp_err_t curl_handle_data(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_OK;
    }
    if (curl_app.stop_requested) {
        return ESP_FAIL;
    }

    curl_app.bytes_read += (uint32_t)len;

    if (curl_app.options.output_to_file) {
        if (curl_app.output_file == NULL) {
            curl_set_transfer_error("output file is not open");
            return ESP_FAIL;
        }

        const size_t written = fwrite(data, 1, len, curl_app.output_file);
        curl_app.bytes_written += (uint32_t)written;
        if (written != len) {
            curl_set_transfer_error("write failed");
            return ESP_FAIL;
        }

        if (curl_app.bytes_written >= curl_app.next_progress) {
            curl_event_t event = {
                .type = CURL_EVENT_PROGRESS,
                .bytes_written = curl_app.bytes_written,
            };
            (void)curl_send_event(&event);
            curl_app.next_progress = curl_app.bytes_written + CURL_FILE_PROGRESS_STEP;
        }
        return ESP_OK;
    }

    if (curl_app.terminal_bytes_sent >= CURL_TERMINAL_LIMIT) {
        curl_app.output_truncated = true;
        return ESP_OK;
    }

    const uint32_t remaining = CURL_TERMINAL_LIMIT - curl_app.terminal_bytes_sent;
    const size_t send_len = len > remaining ? (size_t)remaining : len;
    if (send_len < len) {
        curl_app.output_truncated = true;
    }
    if (send_len == 0) {
        return ESP_OK;
    }

    if (!curl_send_data_chunk(data, send_len)) {
        return ESP_FAIL;
    }
    curl_app.terminal_bytes_sent += (uint32_t)send_len;
    return ESP_OK;
}

static esp_err_t curl_http_event(const solar_os_http_event_t *event, void *user_data)
{
    (void)user_data;
    if (event == NULL) {
        return ESP_OK;
    }

    if (curl_app.stop_requested) {
        return ESP_FAIL;
    }

    switch (event->type) {
    case SOLAR_OS_HTTP_EVENT_HEADER:
        if (event->header_name != NULL &&
            event->header_value != NULL &&
            strcasecmp(event->header_name, "Location") == 0) {
            strlcpy(curl_app.location, event->header_value, sizeof(curl_app.location));
        }
        break;
    case SOLAR_OS_HTTP_EVENT_DATA:
        return curl_handle_data(event->data, event->data_len);
    default:
        break;
    }
    return ESP_OK;
}

static bool curl_url_supported(const char *url)
{
    if (url == NULL) {
        return false;
    }
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static void curl_render_usage(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = curl_io(ctx);

    solar_os_shell_io_clear(io);
    solar_os_shell_io_write_bold(io, "curl");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, "usage: curl [-L] [-o file] URL");
    solar_os_shell_io_writeln(io, "examples:");
    solar_os_shell_io_writeln(io, "  curl http://example.com/");
    solar_os_shell_io_writeln(io, "  curl -L -o page.html https://example.com/");
    solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
    solar_os_shell_io_flush(io);
}

static bool curl_parse_args(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    memset(&curl_app.options, 0, sizeof(curl_app.options));

    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "-L") == 0) {
            curl_app.options.follow_redirects = true;
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            if (solar_os_storage_resolve_path(solar_os_context_argv(ctx, i + 1),
                                              curl_app.options.output_path,
                                              sizeof(curl_app.options.output_path)) != ESP_OK) {
                return false;
            }
            curl_app.options.output_to_file = true;
            i++;
            continue;
        }
        if (arg[0] == '-') {
            return false;
        }
        if (curl_app.options.url[0] != '\0') {
            return false;
        }
        strlcpy(curl_app.options.url, arg, sizeof(curl_app.options.url));
    }

    return curl_app.options.url[0] != '\0' && curl_url_supported(curl_app.options.url);
}

static esp_err_t curl_check_ready(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = curl_io(ctx);

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.started || !wifi.connected || !wifi.has_ip) {
        solar_os_shell_io_writeln(io, "curl: wifi not connected");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
        return ESP_ERR_INVALID_STATE;
    }

    if (curl_app.options.output_to_file && !solar_os_storage_is_mounted()) {
        solar_os_shell_io_writeln(io, "curl: storage required for -o");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void curl_send_done(bool success)
{
    curl_event_t event = {
        .type = CURL_EVENT_DONE,
        .status_code = curl_app.status_code,
        .bytes_read = curl_app.bytes_read,
        .bytes_written = curl_app.bytes_written,
        .success = success,
        .output_truncated = curl_app.output_truncated,
    };

    if (curl_app.content_length >= 0 && curl_app.content_length <= UINT32_MAX) {
        event.content_length_known = true;
        event.content_length = (uint32_t)curl_app.content_length;
    }
    if (curl_app.location[0] != '\0') {
        strlcpy(event.message, curl_app.location, sizeof(event.message));
    }
    (void)curl_send_event(&event);
}

static void curl_task(void *arg)
{
    (void)arg;

    bool success = false;

    curl_send_message(CURL_EVENT_STATUS, "connecting");
    SOLAR_OS_LOGI(TAG,
             "GET %s output=%s follow=%s",
             curl_app.options.url,
             curl_app.options.output_to_file ? curl_app.options.output_path : "terminal",
             curl_app.options.follow_redirects ? "yes" : "no");

    if (curl_app.options.output_to_file) {
        curl_app.output_file = fopen(curl_app.options.output_path, "wb");
        if (curl_app.output_file == NULL) {
            char message[CURL_EVENT_MESSAGE_MAX];
            snprintf(message, sizeof(message), "open failed: errno %d", errno);
            curl_send_message(CURL_EVENT_ERROR, message);
            SOLAR_OS_LOGE(TAG, "open %s failed: errno %d", curl_app.options.output_path, errno);
            goto done;
        }
    }

    const solar_os_http_request_options_t options = {
        .url = curl_app.options.url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .timeout_ms = CURL_TIMEOUT_MS,
        .follow_redirects = curl_app.options.follow_redirects,
        .event_handler = curl_http_event,
        .receive_buffer_size = 1024,
        .transmit_buffer_size = 512,
        .user_agent = "SolarOS-curl/0.1",
        .user_data = &curl_app,
    };

    esp_err_t err = solar_os_http_request_create(&options, &curl_app.request);
    if (err != ESP_OK) {
        curl_send_message(CURL_EVENT_ERROR, "HTTP client init failed");
        goto done;
    }

    solar_os_http_response_t response;
    err = solar_os_http_request_perform(curl_app.request, &response);
    curl_app.status_code = response.status_code;
    curl_app.content_length = response.content_length;

    if (curl_app.output_file != NULL && fflush(curl_app.output_file) != 0) {
        curl_set_transfer_error("flush failed");
    }

    if (curl_app.stop_requested) {
        curl_send_message(CURL_EVENT_ERROR, "cancelled");
    } else if (curl_app.transfer_error) {
        curl_send_message(CURL_EVENT_ERROR,
                          curl_app.transfer_error_message[0] != '\0' ?
                              curl_app.transfer_error_message :
                              "transfer failed");
    } else if (err != ESP_OK) {
        char message[CURL_EVENT_MESSAGE_MAX];
        curl_format_error(message, sizeof(message), "request failed", err);
        curl_send_message(CURL_EVENT_ERROR, message);
        SOLAR_OS_LOGE(TAG, "request failed: %s", esp_err_to_name(err));
    } else {
        success = true;
    }

done:
    if (curl_app.output_file != NULL) {
        fclose(curl_app.output_file);
        curl_app.output_file = NULL;
    }
    curl_send_done(success);
    SOLAR_OS_LOGI(TAG,
             "done status=%d bytes=%" PRIu32 " written=%" PRIu32 " success=%s",
             curl_app.status_code,
             curl_app.bytes_read,
             curl_app.bytes_written,
             success ? "yes" : "no");
    curl_app.task_done = true;
    solar_os_task_delete_internal(NULL);
}

static void curl_drain_events(solar_os_context_t *ctx)
{
    if (curl_app.events == NULL) {
        return;
    }

    solar_os_shell_io_t *io = curl_io(ctx);
    curl_event_t event;
    while (xQueueReceive(curl_app.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case CURL_EVENT_STATUS:
            solar_os_shell_io_printf(io, "curl: %s\n", event.message);
            break;
        case CURL_EVENT_PROGRESS:
            solar_os_shell_io_printf(io, "curl: %" PRIu32 " bytes\n", event.bytes_written);
            break;
        case CURL_EVENT_DATA:
            for (size_t i = 0; i < event.data_len; i++) {
                solar_os_shell_io_put_utf8_byte(io, event.data[i]);
            }
            break;
        case CURL_EVENT_ERROR:
            curl_app.saw_error = true;
            solar_os_shell_io_printf(io, "\ncurl: %s\n", event.message);
            break;
        case CURL_EVENT_DONE:
            curl_app.running = false;
            curl_app.done = true;
            solar_os_shell_io_put_char(io, '\n');
            solar_os_shell_io_printf(io,
                                     "curl: HTTP %d, %" PRIu32 " bytes",
                                     event.status_code,
                                     event.bytes_read);
            if (event.content_length_known) {
                solar_os_shell_io_printf(io, " of %" PRIu32, event.content_length);
            }
            solar_os_shell_io_put_char(io, '\n');
            if (curl_app.options.output_to_file) {
                solar_os_shell_io_printf(io,
                                         "curl: wrote %" PRIu32 " bytes to %s\n",
                                         event.bytes_written,
                                         curl_app.options.output_path);
            }
            if (!curl_app.options.follow_redirects &&
                event.status_code >= 300 &&
                event.status_code < 400 &&
                event.message[0] != '\0') {
                solar_os_shell_io_printf(io, "curl: redirect to %s\n", event.message);
            }
            if (event.output_truncated) {
                solar_os_shell_io_printf(io,
                                         "curl: output truncated at %u bytes; use -o\n",
                                         (unsigned)CURL_TERMINAL_LIMIT);
            }
            if (!event.success && !curl_app.saw_error) {
                solar_os_shell_io_writeln(io, "curl: failed");
            }
            solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t curl_start(solar_os_context_t *ctx)
{
    if (curl_app.task != NULL && !curl_app.task_done) {
        solar_os_shell_io_t *io = curl_io(ctx);
        solar_os_shell_io_clear(io);
        solar_os_shell_io_writeln(io, "curl: previous request is still stopping");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
        return ESP_OK;
    }

    curl_cleanup_resources();
    memset(&curl_app, 0, sizeof(curl_app));

    if (!curl_parse_args(ctx)) {
        curl_render_usage(ctx);
        return ESP_OK;
    }

    solar_os_shell_io_t *io = curl_io(ctx);
    solar_os_shell_io_clear(io);
    solar_os_shell_io_printf_bold(io, "curl %s\n", curl_app.options.url);
    if (curl_app.options.output_to_file) {
        solar_os_shell_io_printf(io, "output: %s\n", curl_app.options.output_path);
    } else {
        solar_os_shell_io_printf(io, "terminal output limit: %u bytes\n", (unsigned)CURL_TERMINAL_LIMIT);
    }
    if (curl_app.options.follow_redirects) {
        solar_os_shell_io_writeln(io, "redirects: follow");
    }
    solar_os_shell_io_flush(io);

    if (curl_check_ready(ctx) != ESP_OK) {
        return ESP_OK;
    }

    curl_app.events = solar_os_queue_create(CURL_EVENT_QUEUE_LEN,
                                             sizeof(curl_event_t));
    if (curl_app.events == NULL) {
        solar_os_shell_io_writeln(io, "curl: out of memory");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
        return ESP_OK;
    }

    curl_app.next_progress = CURL_FILE_PROGRESS_STEP;
    curl_app.status_code = -1;
    curl_app.content_length = -1;
    curl_app.running = true;

    const BaseType_t created = solar_os_task_create_pinned_internal(
        curl_task,
        "solar_os_curl",
        CURL_TASK_STACK,
        NULL,
        CURL_TASK_PRIORITY,
        &curl_app.task,
        tskNO_AFFINITY);
    if (created != pdPASS) {
        solar_os_queue_delete(curl_app.events);
        curl_app.events = NULL;
        curl_app.running = false;
        solar_os_shell_io_writeln(io, "curl: task create failed");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
        return ESP_OK;
    }

    return ESP_OK;
}

static void curl_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    curl_app.stop_requested = true;
    if (curl_app.request != NULL) {
        (void)solar_os_http_request_cancel(curl_app.request);
    }

    if (!solar_os_task_wait_done(curl_app.task,
                                 &curl_app.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "curl task did not stop within %u ms",
                 (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }

    curl_cleanup_resources();
}

static bool curl_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        curl_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        if (curl_app.running) {
            solar_os_shell_io_t *io = curl_io(ctx);
            solar_os_shell_io_writeln(io, "\ncurl: stopping");
            solar_os_shell_io_flush(io);
        }
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *term = curl_terminal(ctx);
        if (term != NULL) {
            solar_os_terminal_page_up(term);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *term = curl_terminal(ctx);
        if (term != NULL) {
            solar_os_terminal_page_down(term);
        }
        return true;
    }
    return true;
}

const solar_os_app_t solar_os_curl_app = {
    .name = "curl",
    .summary = "HTTP client",
    .start = curl_start,
    .stop = curl_stop,
    .event = curl_event,
};
