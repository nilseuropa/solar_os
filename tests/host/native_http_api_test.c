#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_http.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"
#include "solar_os_http_stream.h"

struct solar_os_http_request { solar_os_http_request_options_t options; };
struct solar_os_http_session_context { solar_os_http_cancel_fn cancel; void *user; uint32_t id; };
typedef struct {
    solar_os_http_request_options_t options;
    solar_os_http_header_t *headers;
    uint8_t *option_storage;
    size_t option_storage_len;
    volatile bool stop_requested;
} solar_os_http_stream_t;
struct solar_os_http_stream_session {
    solar_os_http_stream_cancel_fn cancel;
    void *user;
    solar_os_http_stream_t *stream;
};
static const solar_os_native_http_api_v1_t *api;
static unsigned allocations, visits;
static bool fail_allocation, close_timeout;
static solar_os_http_cancel_fn saved_cancel;
static void *saved_user;
static solar_os_http_stream_session_t *active_streams;
static solar_os_native_handle_t active_context;
static bool block_request, waiting, release_wait;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    if (fail_allocation) return NULL;
    void *p = malloc(size); if (p) ++allocations; return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    void *p = solar_os_memory_alloc(n * size, cls, tag);
    if (p) memset(p, 0, n * size);
    return p;
}
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
static esp_err_t http_stream_event(const solar_os_http_event_t *event, void *user)
{ (void)event; (void)user; return ESP_OK; }
static bool http_stream_cancelled(void *user) { return ((solar_os_http_stream_t *)user)->stop_requested; }
#include "http_copy_options.inc"

esp_err_t solar_os_http_session_context_create(solar_os_http_cancel_fn cancel, void *user,
    solar_os_http_session_context_t **out)
{
    *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->cancel = saved_cancel = cancel; (*out)->user = saved_user = user; return ESP_OK;
}
void solar_os_http_session_context_destroy(solar_os_http_session_context_t *state)
{ assert(!state->id); free(state); }
esp_err_t solar_os_http_session_open(solar_os_http_session_context_t *state, const char *origin,
    const char *agent, uint32_t *out)
{ assert(!strcmp(origin, "https://peer") && !strcmp(agent, "agent")); state->id = *out = 7; return ESP_OK; }
esp_err_t solar_os_http_session_close(solar_os_http_session_context_t *state, uint32_t id)
{ assert(id == state->id); state->id = 0; return ESP_OK; }
esp_err_t solar_os_http_stream_session_create(solar_os_http_stream_cancel_fn cancel, void *user,
    solar_os_http_stream_session_t **out)
{
    *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->cancel = cancel; (*out)->user = user; active_streams = *out; return ESP_OK;
}
void solar_os_http_stream_session_destroy(solar_os_http_stream_session_t *state)
{ assert(!state->stream); active_streams = NULL; free(state); }
esp_err_t solar_os_http_stream_open(solar_os_http_stream_session_t *state,
    const solar_os_http_request_options_t *options, uint32_t *out)
{
    assert(!state->stream);
    solar_os_http_stream_t *value = calloc(1, sizeof(*value)); assert(value);
    int error = http_stream_copy_options(value, options);
    if (error) { solar_os_memory_free(value->headers); solar_os_memory_free(value->option_storage); free(value); return error; }
    state->stream = value; *out = 9; return ESP_OK;
}
esp_err_t solar_os_http_stream_close_wait(solar_os_http_stream_session_t *state, uint32_t id, uint32_t timeout)
{
    assert(id == 9 && timeout <= 60000);
    if (close_timeout) return ESP_ERR_TIMEOUT;
    assert(state->stream);
    solar_os_memory_free(state->stream->headers); solar_os_memory_free(state->stream->option_storage);
    free(state->stream); state->stream = NULL; return ESP_OK;
}
esp_err_t solar_os_http_stream_read(solar_os_http_stream_session_t *state, uint32_t id,
    uint32_t timeout, solar_os_http_stream_event_t *out)
{
    assert(state->stream && id == 9 && timeout == 42);
    *out = (solar_os_http_stream_event_t){.type = SOLAR_OS_HTTP_STREAM_EVENT_HEADER,
        .status_code = 201, .content_length = INT64_C(5000000000), .bytes_received = UINT64_C(6000000000),
        .duration_ms = 33, .truncated = true};
    strcpy(out->header_name, "content-type"); strcpy(out->header_value, "text/plain"); return ESP_OK;
}
esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *options,
    solar_os_http_request_t **out)
{ *out = malloc(sizeof(**out)); assert(*out); (*out)->options = *options; return ESP_OK; }
esp_err_t solar_os_http_request_perform(solar_os_http_request_t *request, solar_os_http_response_t *out)
{
    const solar_os_http_request_options_t *options = &request->options;
    *out = (solar_os_http_response_t){.status_code = 404, .content_length = 6, .bytes_received = 6, .duration_ms = 11};
    assert(options->method == SOLAR_OS_HTTP_METHOD_POST && options->header_count == 1);
    assert(!strcmp(options->headers[0].name, "X-Test") && !memcmp(options->body, "request", 7));
    pthread_mutex_lock(&lock); waiting = true; pthread_cond_broadcast(&condition);
    while (block_request && !release_wait) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    if (options->should_cancel(options->cancel_user_data)) { out->cancelled = true; return ESP_ERR_TIMEOUT; }
    solar_os_http_event_t event = {.type = SOLAR_OS_HTTP_EVENT_RESPONSE, .status_code = 404, .content_length = 6};
    return options->event_handler(&event, options->user_data);
}
esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *request) { free(request); return ESP_OK; }
bool solar_os_http_method_parse(const char *text, solar_os_http_method_t *out)
{ if (strcmp(text, "POST")) return false; *out = SOLAR_OS_HTTP_METHOD_POST; return true; }
esp_err_t solar_os_http_perform_buffered(const solar_os_http_request_options_t *options, size_t maximum,
    solar_os_http_buffered_response_t *out)
{
    assert(maximum == 3 && !strcmp(options->url, "https://peer/path"));
    out->response = (solar_os_http_response_t){.status_code = 404, .content_length = 6, .bytes_received = 6};
    out->body = (uint8_t *)strdup("err"); out->body_len = 3; out->body_truncated = true;
    out->headers = calloc(1, sizeof(*out->headers)); assert(out->headers);
    out->headers[0] = (solar_os_http_response_header_t){strdup("x-name"), strdup("value")}; out->header_count = 1;
    return options->should_cancel(options->cancel_user_data) ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_err_t solar_os_http_session_request(solar_os_http_session_context_t *state, uint32_t id,
    const solar_os_http_request_options_t *options, size_t maximum, solar_os_http_buffered_response_t *out)
{ assert(id == state->id); return solar_os_http_perform_buffered(options, maximum, out); }
void solar_os_http_buffered_response_clear(solar_os_http_buffered_response_t *out)
{
    for (size_t i = 0; i < out->header_count; ++i) { free(out->headers[i].name); free(out->headers[i].value); }
    free(out->headers); free(out->body); memset(out, 0, sizeof(*out));
}
static int event_visit(const solar_os_native_http_borrowed_event_v1_t *event, void *user)
{
    assert(user == &visits && event->type == SOLAR_OS_NATIVE_HTTP_RESPONSE && event->status == 404);
    assert(event->content_length == 6); ++visits;
    assert(api->close(active_context) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return SOLAR_OS_NATIVE_ERROR_PERMISSION;
}
static solar_os_native_http_options_v1_t options;
static void *perform_thread(void *unused)
{
    (void)unused; solar_os_native_http_result_v1_t result;
    assert(api->perform(active_context, &options, NULL, NULL, &result) == SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(result.flags & SOLAR_OS_NATIVE_HTTP_CANCELLED); return NULL;
}
int main(void)
{
    api = solar_os_native_http_get_service("http", 1, sizeof(*api)); assert(api);
    assert(!solar_os_native_http_get_service("http", 2, 8));
    assert(!api->create(&active_context) && active_context);
    solar_os_native_http_limits_v1_t limits; assert(!api->limits(&limits) && limits.streams_per_context == 2);
    uint32_t method; assert(!api->method_parse("POST", &method) && method == SOLAR_OS_NATIVE_HTTP_POST);
    char url[] = "https://peer/path", name[] = "X-Test", value[] = "yes", body[] = "request", agent[] = "agent";
    solar_os_native_http_header_v1_t header = {name, value};
    options = (solar_os_native_http_options_v1_t){.url = url, .headers = &header, .header_count = 1,
        .body = body, .body_size = 7, .user_agent = agent, .method = method};
    solar_os_native_http_result_v1_t result;
    assert(api->perform(active_context, &options, event_visit, &visits, &result) == SOLAR_OS_NATIVE_ERROR_PERMISSION);
    assert(visits == 1 && result.status == 404 && result.error == SOLAR_OS_NATIVE_ERROR_PERMISSION);
    pthread_t thread; block_request = true; waiting = false;
    assert(!pthread_create(&thread, NULL, perform_thread, NULL));
    pthread_mutex_lock(&lock); while (!waiting) pthread_cond_wait(&condition, &lock); pthread_mutex_unlock(&lock);
    assert(api->close(active_context) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!api->cancel(active_context, true));
    pthread_mutex_lock(&lock); release_wait = true; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL); block_request = false; assert(!api->cancel(active_context, false));
    uint32_t connection; assert(!api->connection_open(active_context, "https://peer", "agent", &connection));
    solar_os_native_handle_t response; assert(!api->request(active_context, connection, &options, 3, &response));
    assert(!api->response_info(response, &result) && result.status == 404 && result.body_size == 3 && result.header_count == 1);
    assert(result.flags & SOLAR_OS_NATIVE_HTTP_BODY_TRUNCATED);
    uint32_t namesize, valuesize; char n[10], v[10];
    assert(api->response_header(response, 0, NULL, 0, NULL, 0, &namesize, &valuesize) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(namesize == 7 && valuesize == 6);
    assert(!api->response_header(response, 0, n, sizeof(n), v, sizeof(v), &namesize, &valuesize));
    assert(!strcmp(n, "x-name") && !strcmp(v, "value"));
    uint32_t stream; assert(!api->stream_open(active_context, &options, &stream));
    memset(url, '?', strlen(url)); memset(name, '?', strlen(name)); memset(value, '?', strlen(value));
    memset(body, '?', strlen(body)); memset(agent, '?', strlen(agent));
    const solar_os_http_request_options_t *copy = &active_streams->stream->options;
    assert(!strcmp(copy->url, "https://peer/path") && !strcmp(copy->headers[0].name, "X-Test"));
    assert(!strcmp(copy->headers[0].value, "yes") && !memcmp(copy->body, "request", 7) && !strcmp(copy->user_agent, "agent"));
    solar_os_native_http_event_v1_t event;
    assert(!api->stream_read(active_context, stream, 42, &event));
    assert(event.type == SOLAR_OS_NATIVE_HTTP_HEADER && event.flags == SOLAR_OS_NATIVE_HTTP_HEADERS_TRUNCATED);
    assert(event.content_length == INT64_C(5000000000) && event.bytes_received == UINT64_C(6000000000));
    assert(!strcmp(event.header_name, "content-type") && !strcmp(event.header_value, "text/plain"));
    fail_allocation = true;
    assert(api->stream_read(active_context, stream, 42, &event) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    fail_allocation = false; close_timeout = true;
    assert(api->close(active_context) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(active_streams && active_streams->stream);
    assert(api->cancel(active_context, false) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->stream_open(active_context, &options, &stream) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    close_timeout = false; assert(!api->close(active_context)); assert(!active_streams);
    assert(saved_cancel(saved_user));
    char bytes[8]; size_t size;
    assert(!api->response_read(response, 1, bytes, sizeof(bytes), &size) && size == 2 && !memcmp(bytes, "rr", 2));
    assert(!api->response_close(response)); assert(api->response_close(response) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!allocations && !solar_os_native_handle_count());
    solar_os_native_http_origin_v1_t origin, other;
    assert(!api->origin_parse("https://peer/path", &origin) && origin.tls && origin.port == 443);
    other = origin; other.port = 70000; assert(!api->same_origin(&origin, &other));
    char redirect[128];
    assert(!api->redirect_url("https://peer/path", "../next", redirect, sizeof(redirect)));
    assert(!strcmp(redirect, "https://peer/next"));
    assert(api->redirect_url("https://peer/path", "http://peer/", redirect, sizeof(redirect)));
    puts("native HTTP values, copied options, cancellation and joined shutdown passed"); return 0;
}
