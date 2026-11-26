#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_HTTP_SERVICE "http"
#define SOLAR_OS_NATIVE_HTTP_ABI 1U
#define SOLAR_OS_NATIVE_HTTP_GET 0U
#define SOLAR_OS_NATIVE_HTTP_POST 1U
#define SOLAR_OS_NATIVE_HTTP_PUT 2U
#define SOLAR_OS_NATIVE_HTTP_PATCH 3U
#define SOLAR_OS_NATIVE_HTTP_DELETE 4U
#define SOLAR_OS_NATIVE_HTTP_HEAD 5U
#define SOLAR_OS_NATIVE_HTTP_FOLLOW_REDIRECTS 1U
#define SOLAR_OS_NATIVE_HTTP_CANCELLED 1U
#define SOLAR_OS_NATIVE_HTTP_DEADLINE 2U
#define SOLAR_OS_NATIVE_HTTP_BODY_TRUNCATED 4U
#define SOLAR_OS_NATIVE_HTTP_HEADERS_TRUNCATED 8U
#define SOLAR_OS_NATIVE_HTTP_RESPONSE 1U
#define SOLAR_OS_NATIVE_HTTP_HEADER 2U
#define SOLAR_OS_NATIVE_HTTP_DATA 3U
#define SOLAR_OS_NATIVE_HTTP_COMPLETE 4U
#define SOLAR_OS_NATIVE_HTTP_ERROR 5U
typedef struct { const char *name, *value; } solar_os_native_http_header_v1_t;
typedef struct {
    const char *url;
    const solar_os_native_http_header_v1_t *headers;
    const void *body;
    const char *user_agent;
    size_t body_size;
    uint32_t method, header_count, flags, max_redirects;
    uint32_t timeout_ms, read_poll_ms, deadline_ms, receive_buffer_size, transmit_buffer_size;
} solar_os_native_http_options_v1_t;
typedef struct {
    int64_t content_length;
    uint64_t bytes_received, body_size;
    int32_t status, error;
    uint32_t duration_ms, flags, header_count;
} solar_os_native_http_result_v1_t;
typedef struct {
    int64_t content_length;
    uint64_t bytes_received;
    int32_t status, error;
    uint32_t type, duration_ms, flags, data_size;
    char header_name[64], header_value[512];
    uint8_t data[1024];
} solar_os_native_http_event_v1_t;
typedef struct {
    const char *header_name, *header_value;
    const uint8_t *data;
    size_t data_size;
    int64_t content_length;
    int32_t status;
    uint32_t type;
} solar_os_native_http_borrowed_event_v1_t;
typedef int (*solar_os_native_http_event_fn)(const solar_os_native_http_borrowed_event_v1_t *event, void *user);
typedef struct {
    uint32_t streams_per_context, global_streams, connections_per_context, global_connections;
    uint32_t buffered_body_max, buffered_headers_max, buffered_header_bytes_max, stream_timeout_max_ms;
} solar_os_native_http_limits_v1_t;
typedef struct { char host[256]; uint32_t port, tls; } solar_os_native_http_origin_v1_t;

typedef struct {
    uint64_t bytes;
    uint8_t sha256[32];
    solar_os_native_http_result_v1_t response;
} solar_os_native_download_result_v1_t;
typedef void (*solar_os_native_download_progress_fn)(size_t received, size_t total, void *user);

/* A context owns its keep-alive connections, streams and atomic cancellation.
 * Serialize context operations except cancel(), which is cross-task safe.
 * Stream open copies every option/buffer before return. Buffered requests
 * borrow options only during the call; response handles own their results.
 * A failed request may still return a response handle with status/partial data.
 * Close every response handle independently of the context.
 *
 * Raw perform() borrows options and invokes its event callback synchronously
 * on the calling task, only until perform returns. Borrowed event members
 * expire on callback return. Return OK to continue or a native error to abort.
 * One-shot perform/request require a worker stack suitable for the OS HTTP/TLS
 * service (16 KiB); streams and persistent requests use host transport workers.
 *
 * Closing cancels and joins streams. A timeout preserves the context/channel
 * for cleanup retry; no new requests may be started on a closing context.
 * No interpreter callbacks or caller pointers survive a completed operation. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(solar_os_native_handle_t *context);
    int (*close)(solar_os_native_handle_t context);
    int (*cancel)(solar_os_native_handle_t context, bool requested);
    int (*limits)(solar_os_native_http_limits_v1_t *limits);
    int (*method_parse)(const char *text, uint32_t *method);
    int (*perform)(solar_os_native_handle_t context, const solar_os_native_http_options_v1_t *options,
        solar_os_native_http_event_fn event, void *user, solar_os_native_http_result_v1_t *result);
    int (*request)(solar_os_native_handle_t context, uint32_t connection,
        const solar_os_native_http_options_v1_t *options, size_t max_body_size, solar_os_native_handle_t *response);
    int (*response_close)(solar_os_native_handle_t response);
    int (*response_info)(solar_os_native_handle_t response, solar_os_native_http_result_v1_t *result);
    int (*response_read)(solar_os_native_handle_t response, uint64_t offset, void *bytes,
        size_t capacity, size_t *read_bytes);
    int (*response_header)(solar_os_native_handle_t response, uint32_t index,
        char *name, size_t name_capacity, char *value, size_t value_capacity,
        uint32_t *name_size, uint32_t *value_size);
    int (*connection_open)(solar_os_native_handle_t context, const char *origin,
        const char *user_agent, uint32_t *connection);
    int (*connection_close)(solar_os_native_handle_t context, uint32_t connection);
    int (*stream_open)(solar_os_native_handle_t context, const solar_os_native_http_options_v1_t *options,
        uint32_t *stream);
    int (*stream_read)(solar_os_native_handle_t context, uint32_t stream, uint32_t timeout_ms,
        solar_os_native_http_event_v1_t *event);
    int (*stream_close)(solar_os_native_handle_t context, uint32_t stream, uint32_t timeout_ms);
    int (*close_all)(solar_os_native_handle_t context, uint32_t timeout_ms);
    int (*origin_parse)(const char *url, solar_os_native_http_origin_v1_t *origin);
    bool (*same_origin)(const solar_os_native_http_origin_v1_t *a, const solar_os_native_http_origin_v1_t *b);
    bool (*redirect_status)(int32_t status);
    int (*redirect_url)(const char *base, const char *location, char *out, size_t capacity);
    /* Download to an exclusive caller-owned staging path. The service checks
     * the stored bytes against the response and optional expected SHA-256.
     * A failed transfer removes its staging file. Progress runs on the calling
     * task, and cancel() applies during transfer and file verification. */
    int (*download)(solar_os_native_handle_t context, const solar_os_native_http_options_v1_t *options,
        const char *staged_path, size_t max_size, size_t expected_size, const char *expected_sha256,
        solar_os_native_download_progress_fn progress, void *user, solar_os_native_download_result_v1_t *result);
} solar_os_native_http_api_v1_t;
