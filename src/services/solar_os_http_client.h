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
} solar_os_http_event_type_t;

typedef struct {
    solar_os_http_event_type_t type;
    int status_code;
    const char *header_name;
    const char *header_value;
    const uint8_t *data;
    size_t data_len;
} solar_os_http_event_t;

typedef esp_err_t (*solar_os_http_event_fn)(const solar_os_http_event_t *event,
                                            void *user_data);

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
    /* End-to-end request deadline; zero disables the deadline. */
    uint32_t deadline_ms;
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

/*
 * Requests are one-shot objects. The options and all memory referenced by them
 * must remain valid until perform returns. perform is blocking and is intended
 * to run in a caller-owned worker task; event_handler receives response data as
 * it arrives and must not retain event pointers.
 */
esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *options,
                                       solar_os_http_request_t **out_request);
esp_err_t solar_os_http_request_perform(solar_os_http_request_t *request,
                                        solar_os_http_response_t *response);

/* Safe to call from another task while perform is blocked. */
esp_err_t solar_os_http_request_cancel(solar_os_http_request_t *request);

/* Returns ESP_ERR_INVALID_STATE while perform is active. */
esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *request);
