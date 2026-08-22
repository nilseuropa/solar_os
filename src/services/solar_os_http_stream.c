#include "solar_os_http_stream.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"

#define HTTP_STREAM_QUEUE_LEN 8U
#define HTTP_STREAM_QUEUE_WAIT_MS 100U
#define HTTP_STREAM_READ_POLL_MS 50U
#define HTTP_STREAM_STOP_WAIT_MS 2000U
#define HTTP_STREAM_TASK_STACK (12U * 1024U)
#define HTTP_STREAM_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(HTTP_STREAM_TASK_STACK);

typedef struct solar_os_http_stream solar_os_http_stream_t;

struct solar_os_http_stream {
    QueueHandle_t events;
    TaskHandle_t task;
    solar_os_http_request_t *request;
    solar_os_http_request_options_t options;
    solar_os_http_header_t *headers;
    void *option_storage;
    size_t option_storage_len;
    solar_os_http_stream_cancel_fn should_cancel;
    void *cancel_user;
    solar_os_http_response_t response;
    esp_err_t result;
    volatile bool stop_requested;
    volatile bool task_done;
    bool detached;
    bool terminal_delivered;
};

struct solar_os_http_stream_session {
    solar_os_http_stream_cancel_fn should_cancel;
    void *cancel_user;
    solar_os_http_stream_t *streams[SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES];
    uint32_t generations[SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES];
};

static portMUX_TYPE http_stream_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t http_stream_global_count;

static void http_stream_wipe(void *buffer, size_t length)
{
    volatile uint8_t *bytes = buffer;
    while (bytes != NULL && length > 0) {
        *bytes++ = 0;
        length--;
    }
}

static bool http_stream_cancelled(void *user)
{
    const solar_os_http_stream_t *stream = user;
    return stream == NULL || stream->stop_requested ||
        (stream->should_cancel != NULL &&
         stream->should_cancel(stream->cancel_user));
}

static void http_stream_release_global(void)
{
    portENTER_CRITICAL(&http_stream_lock);
    if (http_stream_global_count > 0) {
        http_stream_global_count--;
    }
    portEXIT_CRITICAL(&http_stream_lock);
}

static void http_stream_destroy(solar_os_http_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    (void)solar_os_http_request_destroy(stream->request);
    solar_os_queue_delete(stream->events);
    solar_os_memory_free(stream->headers);
    http_stream_wipe(stream->option_storage, stream->option_storage_len);
    solar_os_memory_free(stream->option_storage);
    solar_os_memory_free(stream);
    http_stream_release_global();
}

static esp_err_t http_stream_copy_text(char *destination,
                                       size_t capacity,
                                       const char *source,
                                       bool *truncated)
{
    if (destination == NULL || capacity == 0 || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t source_len = strlen(source);
    const size_t copy_len = source_len < capacity - 1U ?
        source_len : capacity - 1U;
    memcpy(destination, source, copy_len);
    destination[copy_len] = '\0';
    if (truncated != NULL && copy_len < source_len) {
        *truncated = true;
    }
    return ESP_OK;
}

static esp_err_t http_stream_event(const solar_os_http_event_t *source,
                                   void *user)
{
    solar_os_http_stream_t *stream = user;
    if (stream == NULL || source == NULL || http_stream_cancelled(stream)) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_http_stream_event_t event = {
        .status_code = source->status_code,
        .content_length = source->content_length,
    };
    switch (source->type) {
    case SOLAR_OS_HTTP_EVENT_RESPONSE:
        event.type = SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE;
        break;
    case SOLAR_OS_HTTP_EVENT_HEADER:
        event.type = SOLAR_OS_HTTP_STREAM_EVENT_HEADER;
        if (source->header_name != NULL) {
            (void)http_stream_copy_text(event.header_name,
                                        sizeof(event.header_name),
                                        source->header_name,
                                        &event.truncated);
        }
        if (source->header_value != NULL) {
            (void)http_stream_copy_text(event.header_value,
                                        sizeof(event.header_value),
                                        source->header_value,
                                        &event.truncated);
        }
        break;
    case SOLAR_OS_HTTP_EVENT_DATA:
        event.type = SOLAR_OS_HTTP_STREAM_EVENT_DATA;
        if (source->data_len > sizeof(event.data)) {
            return ESP_ERR_INVALID_SIZE;
        }
        event.data_len = source->data_len;
        if (event.data_len > 0) {
            memcpy(event.data, source->data, event.data_len);
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return xQueueSend(stream->events,
                      &event,
                      pdMS_TO_TICKS(HTTP_STREAM_QUEUE_WAIT_MS)) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

static void http_stream_task(void *arg)
{
    solar_os_http_stream_t *stream = arg;
    stream->result = solar_os_http_request_create(&stream->options,
                                                   &stream->request);
    if (stream->result == ESP_OK) {
        stream->result = solar_os_http_request_perform(stream->request,
                                                       &stream->response);
    }

    bool detached = false;
    portENTER_CRITICAL(&http_stream_lock);
    stream->task_done = true;
    detached = stream->detached;
    portEXIT_CRITICAL(&http_stream_lock);
    if (detached) {
        http_stream_destroy(stream);
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t http_stream_copy_options(
    solar_os_http_stream_t *stream,
    const solar_os_http_request_options_t *source)
{
    if (stream == NULL || source == NULL || source->url == NULL ||
        source->url[0] == '\0' ||
        (source->header_count > 0 && source->headers == NULL) ||
        (source->body_len > 0 && source->body == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t storage_len = strlen(source->url) + 1U + source->body_len;
    if (source->user_agent != NULL) {
        storage_len += strlen(source->user_agent) + 1U;
    }
    for (size_t i = 0; i < source->header_count; i++) {
        if (source->headers[i].name == NULL || source->headers[i].value == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        storage_len += strlen(source->headers[i].name) + 1U;
        storage_len += strlen(source->headers[i].value) + 1U;
    }
    if (storage_len < source->body_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    stream->option_storage = solar_os_memory_alloc(
        storage_len,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.stream.options");
    if (stream->option_storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    stream->option_storage_len = storage_len;
    if (source->header_count > 0) {
        stream->headers = solar_os_memory_calloc(
            source->header_count,
            sizeof(*stream->headers),
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "http.stream.headers");
        if (stream->headers == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    uint8_t *cursor = stream->option_storage;
#define HTTP_STREAM_COPY_STRING(target, value) do {                            \
        const size_t copy_length = strlen(value) + 1U;                         \
        memcpy(cursor, value, copy_length);                                    \
        target = (const char *)cursor;                                         \
        cursor += copy_length;                                                 \
    } while (0)

    stream->options = *source;
    HTTP_STREAM_COPY_STRING(stream->options.url, source->url);
    if (source->user_agent != NULL) {
        HTTP_STREAM_COPY_STRING(stream->options.user_agent, source->user_agent);
    }
    if (source->body_len > 0) {
        memcpy(cursor, source->body, source->body_len);
        stream->options.body = cursor;
        cursor += source->body_len;
    }
    for (size_t i = 0; i < source->header_count; i++) {
        HTTP_STREAM_COPY_STRING(stream->headers[i].name, source->headers[i].name);
        HTTP_STREAM_COPY_STRING(stream->headers[i].value, source->headers[i].value);
    }
#undef HTTP_STREAM_COPY_STRING

    stream->options.headers = stream->headers;
    stream->options.event_handler = http_stream_event;
    stream->options.user_data = stream;
    stream->options.cancel_flag = &stream->stop_requested;
    stream->options.should_cancel = http_stream_cancelled;
    stream->options.cancel_user_data = stream;
    return ESP_OK;
}

static solar_os_http_stream_t *http_stream_find(
    solar_os_http_stream_session_t *session,
    uint32_t handle,
    size_t *out_index)
{
    if (session == NULL || handle == 0) {
        return NULL;
    }
    const uint32_t adjusted = handle - 1U;
    const size_t index = adjusted % SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES;
    const uint32_t generation =
        adjusted / SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES + 1U;
    if (session->generations[index] != generation) {
        return NULL;
    }
    if (out_index != NULL) {
        *out_index = index;
    }
    return session->streams[index];
}

esp_err_t solar_os_http_stream_session_create(
    solar_os_http_stream_cancel_fn should_cancel,
    void *cancel_user,
    solar_os_http_stream_session_t **out_session)
{
    if (out_session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_session = solar_os_memory_calloc(
        1,
        sizeof(**out_session),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.stream.session");
    if (*out_session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (*out_session)->should_cancel = should_cancel;
    (*out_session)->cancel_user = cancel_user;
    return ESP_OK;
}

void solar_os_http_stream_session_destroy(solar_os_http_stream_session_t *session)
{
    if (session == NULL) {
        return;
    }
    solar_os_http_stream_close_all(session);
    solar_os_memory_free(session);
}

esp_err_t solar_os_http_stream_open(
    solar_os_http_stream_session_t *session,
    const solar_os_http_request_options_t *options,
    uint32_t *out_handle)
{
    if (session == NULL || options == NULL || out_handle == NULL ||
        options->timeout_ms > SOLAR_OS_HTTP_STREAM_MAX_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = 0;

    size_t index = SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES;
    portENTER_CRITICAL(&http_stream_lock);
    if (http_stream_global_count < SOLAR_OS_HTTP_STREAM_GLOBAL_MAX_HANDLES) {
        for (size_t i = 0; i < SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES; i++) {
            if (session->streams[i] == NULL) {
                index = i;
                http_stream_global_count++;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&http_stream_lock);
    if (index == SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_http_stream_t *stream = solar_os_memory_calloc(
        1,
        sizeof(*stream),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.stream");
    if (stream == NULL) {
        http_stream_release_global();
        return ESP_ERR_NO_MEM;
    }
    stream->should_cancel = session->should_cancel;
    stream->cancel_user = session->cancel_user;
    stream->response.status_code = -1;
    stream->response.content_length = -1;
    stream->events = solar_os_queue_create(HTTP_STREAM_QUEUE_LEN,
                                            sizeof(solar_os_http_stream_event_t));
    esp_err_t err = stream->events != NULL ?
        http_stream_copy_options(stream, options) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        http_stream_destroy(stream);
        return err;
    }

    session->generations[index]++;
    if (session->generations[index] == 0 ||
        session->generations[index] >
            UINT32_MAX / SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES) {
        session->generations[index] = 1;
    }
    session->streams[index] = stream;
    const uint32_t handle = (session->generations[index] - 1U) *
        SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES + (uint32_t)index + 1U;

    const BaseType_t created = solar_os_task_create_pinned_internal(
        http_stream_task,
        "http_stream",
        HTTP_STREAM_TASK_STACK,
        stream,
        HTTP_STREAM_TASK_PRIORITY,
        &stream->task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        session->streams[index] = NULL;
        http_stream_destroy(stream);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t solar_os_http_stream_read(
    solar_os_http_stream_session_t *session,
    uint32_t handle,
    uint32_t timeout_ms,
    solar_os_http_stream_event_t *event)
{
    if (event == NULL || timeout_ms > SOLAR_OS_HTTP_STREAM_MAX_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_http_stream_t *stream = http_stream_find(session, handle, NULL);
    if (stream == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    do {
        TickType_t wait_ticks = 0;
        if (timeout_ms > 0) {
            const TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout_ticks) {
                break;
            }
            const TickType_t remaining = timeout_ticks - elapsed;
            wait_ticks = pdMS_TO_TICKS(HTTP_STREAM_READ_POLL_MS);
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
            if (wait_ticks > remaining) {
                wait_ticks = remaining;
            }
        }
        if (xQueueReceive(stream->events, event, wait_ticks) == pdPASS) {
            return ESP_OK;
        }
        if (stream->task_done) {
            if (stream->terminal_delivered) {
                return ESP_ERR_INVALID_STATE;
            }
            *event = (solar_os_http_stream_event_t){
                .type = stream->result == ESP_OK ?
                    SOLAR_OS_HTTP_STREAM_EVENT_COMPLETE :
                    SOLAR_OS_HTTP_STREAM_EVENT_ERROR,
                .status_code = stream->response.status_code,
                .content_length = stream->response.content_length,
                .bytes_received = stream->response.bytes_received,
                .duration_ms = stream->response.duration_ms,
                .error = stream->result,
                .cancelled = stream->response.cancelled,
                .deadline_exceeded = stream->response.deadline_exceeded,
            };
            stream->terminal_delivered = true;
            return ESP_OK;
        }
        if (timeout_ms == 0) {
            break;
        }
    } while (true);
    return ESP_ERR_TIMEOUT;
}

esp_err_t solar_os_http_stream_close(
    solar_os_http_stream_session_t *session,
    uint32_t handle)
{
    size_t index = 0;
    solar_os_http_stream_t *stream = http_stream_find(session, handle, &index);
    if (stream == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    session->streams[index] = NULL;
    stream->stop_requested = true;
    if (stream->request != NULL) {
        (void)solar_os_http_request_cancel(stream->request);
    }
    if (solar_os_task_wait_done(stream->task,
                                &stream->task_done,
                                HTTP_STREAM_STOP_WAIT_MS)) {
        http_stream_destroy(stream);
        return ESP_OK;
    }

    bool completed = false;
    portENTER_CRITICAL(&http_stream_lock);
    completed = stream->task_done;
    if (!completed) {
        stream->detached = true;
    }
    portEXIT_CRITICAL(&http_stream_lock);
    if (completed) {
        http_stream_destroy(stream);
    }
    return completed ? ESP_OK : ESP_ERR_TIMEOUT;
}

void solar_os_http_stream_close_all(solar_os_http_stream_session_t *session)
{
    if (session == NULL) {
        return;
    }
    for (size_t i = 0; i < SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES; i++) {
        solar_os_http_stream_t *stream = session->streams[i];
        if (stream == NULL) {
            continue;
        }
        const uint32_t handle = (session->generations[i] - 1U) *
            SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES + (uint32_t)i + 1U;
        (void)solar_os_http_stream_close(session, handle);
    }
}

const char *solar_os_http_stream_event_type_name(
    solar_os_http_stream_event_type_t type)
{
    switch (type) {
    case SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE:
        return "response";
    case SOLAR_OS_HTTP_STREAM_EVENT_HEADER:
        return "header";
    case SOLAR_OS_HTTP_STREAM_EVENT_DATA:
        return "data";
    case SOLAR_OS_HTTP_STREAM_EVENT_COMPLETE:
        return "complete";
    case SOLAR_OS_HTTP_STREAM_EVENT_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
