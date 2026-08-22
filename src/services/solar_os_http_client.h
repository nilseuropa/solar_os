#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct solar_os_http_request solar_os_http_request_t;

typedef enum {
    SOLAR_OS_HTTP_METHOD_GET = 0,
    SOLAR_OS_HTTP_METHOD_POST,
    SOLAR_OS_HTTP_METHOD_PUT,
    SOLAR_OS_HTTP_METHOD_PATCH,
    SOLAR_OS_HTTP_METHOD_DELETE,
    SOLAR_OS_HTTP_METHOD_HEAD,
} solar_os_http_method_t;

typedef struct {
    const char *name;
    const char *value;
} solar_os_http_header_t;

typedef enum {
    SOLAR_OS_HTTP_EVENT_HEADER = 0,
    SOLAR_OS_HTTP_EVENT_DATA,
    SOLAR_OS_HTTP_EVENT_RESPONSE,
} solar_os_http_event_type_t;

typedef struct {
    solar_os_http_event_type_t type;
    int status_code;
    const char *header_name;
    const char *header_value;
    const uint8_t *data;
    size_t data_len;
    int64_t content_length;
} solar_os_http_event_t;

typedef esp_err_t (*solar_os_http_event_fn)(const solar_os_http_event_t *event,
                                            void *user_data);
typedef bool (*solar_os_http_cancel_fn)(void *user_data);

typedef struct {
    const char *url;
    solar_os_http_method_t method;
    const solar_os_http_header_t *headers;
    size_t header_count;
    const void *body;
    size_t body_len;
    const char *user_agent;
    bool follow_redirects;
    /* Zero uses the service default when redirects are enabled. */
    uint8_t max_redirects;
    /* Per-operation transport timeout; zero uses the service default. */
    uint32_t timeout_ms;
    /* Optional body-read poll interval; timeouts retry after checking cancel. */
    uint32_t read_poll_ms;
    /* End-to-end request deadline; zero disables the deadline. */
    uint32_t deadline_ms;
    /* Optional caller-owned cancellation flag, valid until perform returns. */
    const volatile bool *cancel_flag;
    /* Optional cancellation callback, checked with cancel_flag. */
    solar_os_http_cancel_fn should_cancel;
    void *cancel_user_data;
    size_t receive_buffer_size;
    size_t transmit_buffer_size;
    solar_os_http_event_fn event_handler;
    void *user_data;
} solar_os_http_request_options_t;

typedef struct {
    /* HTTP status is reported independently of the transport return value. */
    int status_code;
    int64_t content_length;
    uint64_t bytes_received;
    uint32_t duration_ms;
    bool cancelled;
    bool deadline_exceeded;
} solar_os_http_response_t;

typedef struct {
    char *name;
    char *value;
} solar_os_http_response_header_t;

typedef struct {
    solar_os_http_response_t response;
    uint8_t *body;
    size_t body_len;
    bool body_truncated;
    solar_os_http_response_header_t *headers;
    size_t header_count;
    bool headers_truncated;
} solar_os_http_buffered_response_t;

#define SOLAR_OS_HTTP_BUFFERED_DEFAULT_MAX_BODY (64U * 1024U)
#define SOLAR_OS_HTTP_BUFFERED_MAX_BODY (512U * 1024U)
#define SOLAR_OS_HTTP_BUFFERED_MAX_HEADERS 24U
#define SOLAR_OS_HTTP_BUFFERED_MAX_HEADER_BYTES (8U * 1024U)

/*
 * Requests are one-shot objects. The options and all memory referenced by them
 * must remain valid until perform returns. This includes cancel_flag when set.
 * perform is blocking and is intended to run in a caller-owned worker task;
 * event_handler receives response data as it arrives and must not retain event
 * pointers.
 */
esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *options,
                                       solar_os_http_request_t **out_request);
esp_err_t solar_os_http_request_perform(solar_os_http_request_t *request,
                                        solar_os_http_response_t *response);

/* Safe to call from another task while perform is blocked. */
esp_err_t solar_os_http_request_cancel(solar_os_http_request_t *request);

/* Returns ESP_ERR_INVALID_STATE while perform is active. */
esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *request);

/*
 * High-level bounded response collection for synchronous service consumers.
 * HTTP status codes, including 4xx and 5xx, are returned in response.response;
 * only request, transport, cancellation, deadline, and allocation failures are
 * returned as esp_err_t. The body is cut at max_body_bytes and marked
 * body_truncated. Call clear for every initialized response, including errors.
 */
esp_err_t solar_os_http_perform_buffered(
    const solar_os_http_request_options_t *options,
    size_t max_body_bytes,
    solar_os_http_buffered_response_t *response);
void solar_os_http_buffered_response_clear(
    solar_os_http_buffered_response_t *response);

bool solar_os_http_method_parse(const char *text,
                                solar_os_http_method_t *method);
