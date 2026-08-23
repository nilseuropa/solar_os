# HTTP Client Service

The `service_http_client` package provides the native HTTP transport boundary
for foreground apps, background jobs, and provider adapters. It owns ESP-IDF
HTTP/TLS setup and exposes a transport-neutral streaming API in
`solar_os_http_client.h`.

## Request Lifecycle

`solar_os_http_request_create()` creates a one-shot request from
`solar_os_http_request_options_t`. The option strings, headers, body, callback,
and callback context must remain valid until `solar_os_http_request_perform()`
returns. Perform is intentionally blocking: callers run it in their own worker
task so the cooperative scheduler is never used for network I/O.

Response headers, final response metadata, and body chunks are delivered
through the event callback. `SOLAR_OS_HTTP_EVENT_RESPONSE` reports the final
status and content length after redirect handling and before body chunks.
Returning an error stops the stream and returns that error from perform. The
final `solar_os_http_response_t` reports status, content length, streamed byte
count, duration, cancellation, and deadline expiry.
HTTP error statuses such as 404 or 429 are response metadata, not transport
errors; callers decide how to handle them.

Call `solar_os_http_request_cancel()` from another task to interrupt an active
request. Once perform has returned, call `solar_os_http_request_destroy()`.

`solar_os_http_stream.h` adds an owned asynchronous wrapper for script and UI
consumers. It copies all request options, runs the blocking request on an
internal-stack foreground worker, and exposes bounded `response`, `header`,
`data`, `complete`, and `error` events through generation-checked handles. Use
this wrapper when the consumer must continue its event loop while a long-lived
HTTP response is open.

Persistent clients use `solar_os_http_session_context_t`. A caller creates one
context, opens up to two origin-scoped handles, and makes synchronous bounded
requests with `solar_os_http_session_request()`. Four handles are allowed
globally. The retained ESP-IDF client reuses an HTTP/TLS connection only for
the exact scheme and authority. Redirect following is rejected, request
headers are cleared between calls, and runtime context destruction closes all
handles. When an idle connection has gone stale, the service retries GET or
HEAD once before any response starts; it never retries writes.

## Timing Controls

- `timeout_ms` limits each connect, write, header, or read operation. Zero uses
  the service default of 10 seconds.
- `deadline_ms` limits the complete request, including redirects. Zero means no
  end-to-end deadline.
- Before every blocking operation, the service reduces the transport timeout
  to the remaining deadline. Deadline expiry returns `ESP_ERR_TIMEOUT` and sets
  `deadline_exceeded` in the response.

Redirects are disabled by default. With `follow_redirects`, the service follows
up to `max_redirects`; zero selects the service default. TLS server validation
uses the ESP-IDF certificate bundle when it is enabled in the firmware.

## Minimal Streaming Request

```c
static esp_err_t on_http_event(const solar_os_http_event_t *event, void *context)
{
    if (event->type == SOLAR_OS_HTTP_EVENT_DATA) {
        return consume_chunk(context, event->data, event->data_len);
    }
    return ESP_OK;
}

solar_os_http_request_options_t options = {
    .url = url,
    .method = SOLAR_OS_HTTP_METHOD_POST,
    .headers = headers,
    .header_count = header_count,
    .body = json,
    .body_len = strlen(json),
    .timeout_ms = 10000,
    .deadline_ms = 30000,
    .follow_redirects = true,
    .event_handler = on_http_event,
    .user_data = consumer,
};

solar_os_http_request_t *request = NULL;
solar_os_http_response_t response;
esp_err_t err = solar_os_http_request_create(&options, &request);
if (err == ESP_OK) {
    err = solar_os_http_request_perform(request, &response);
    (void)solar_os_http_request_destroy(request);
}
```
