#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_http_client.h"

#define SOLAR_OS_HTTP_STREAM_SESSION_MAX_HANDLES 2U
#define SOLAR_OS_HTTP_STREAM_GLOBAL_MAX_HANDLES 4U
#define SOLAR_OS_HTTP_STREAM_EVENT_DATA_MAX 1024U
#define SOLAR_OS_HTTP_STREAM_HEADER_NAME_MAX 64U
#define SOLAR_OS_HTTP_STREAM_HEADER_VALUE_MAX 512U
#define SOLAR_OS_HTTP_STREAM_MAX_TIMEOUT_MS 60000U

typedef struct solar_os_http_stream_session solar_os_http_stream_session_t;
typedef bool (*solar_os_http_stream_cancel_fn)(void *user);

typedef enum {
    SOLAR_OS_HTTP_STREAM_EVENT_RESPONSE = 1,
    SOLAR_OS_HTTP_STREAM_EVENT_HEADER,
    SOLAR_OS_HTTP_STREAM_EVENT_DATA,
    SOLAR_OS_HTTP_STREAM_EVENT_COMPLETE,
    SOLAR_OS_HTTP_STREAM_EVENT_ERROR,
} solar_os_http_stream_event_type_t;

typedef struct {
    solar_os_http_stream_event_type_t type;
    int status_code;
    int64_t content_length;
    uint64_t bytes_received;
    uint32_t duration_ms;
    esp_err_t error;
    bool cancelled;
    bool deadline_exceeded;
    bool truncated;
    size_t data_len;
    char header_name[SOLAR_OS_HTTP_STREAM_HEADER_NAME_MAX];
    char header_value[SOLAR_OS_HTTP_STREAM_HEADER_VALUE_MAX];
    uint8_t data[SOLAR_OS_HTTP_STREAM_EVENT_DATA_MAX];
} solar_os_http_stream_event_t;

/*
 * A session belongs to one script runtime. stream_open copies every option and
 * referenced buffer before it starts a worker, so interpreter objects can be
 * released immediately. The session replaces event and cancellation callbacks
 * in the request options with its owned worker callbacks. Events are bounded
 * and preserve transport order.
 * If the consumer does not drain the queue, the worker stops with
 * ESP_ERR_NO_MEM rather than dropping bytes and corrupting the stream.
 */
esp_err_t solar_os_http_stream_session_create(
    solar_os_http_stream_cancel_fn should_cancel,
    void *cancel_user,
    solar_os_http_stream_session_t **out_session);
void solar_os_http_stream_session_destroy(solar_os_http_stream_session_t *session);

esp_err_t solar_os_http_stream_open(
    solar_os_http_stream_session_t *session,
    const solar_os_http_request_options_t *options,
    uint32_t *out_handle);
esp_err_t solar_os_http_stream_read(
    solar_os_http_stream_session_t *session,
    uint32_t handle,
    uint32_t timeout_ms,
    solar_os_http_stream_event_t *event);
esp_err_t solar_os_http_stream_close(
    solar_os_http_stream_session_t *session,
    uint32_t handle);
void solar_os_http_stream_close_all(solar_os_http_stream_session_t *session);

const char *solar_os_http_stream_event_type_name(
    solar_os_http_stream_event_type_t type);
