#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/task.h"
#include "solar_os_http_server.h"
#include "solar_os_log.h"
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_http_server.h"

static httpd_uri_handler_t registered_get;
static httpd_uri_handler_t registered_post;
static atomic_uint server_starts;
static unsigned server_stops;
static bool stop_fails, post_registration_fails, slow_start;
static unsigned async_begins;
static unsigned async_completes;
static unsigned service_unavailable;
static unsigned sync_calls;
static unsigned library_pins, allocations, sent, chunks, unauthorized;
static bool complete_fails, read_times_out, app_scope;
static const char *authorization;
static const solar_os_app_t app = {.name = "http-fixture"};
static httpd_req_t *accepted_async[2];
static size_t accepted_async_count;

esp_err_t solar_os_log_write(solar_os_log_level_t level,
                             const char *tag,
                             const char *format,
                             ...)
{
    (void)level;
    (void)tag;
    (void)format;
    return ESP_OK;
}

uint32_t esp_random(void)
{
    return 123456U;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    assert(handle != NULL);
    assert(config != NULL);
    *handle = (void *)(uintptr_t)(atomic_fetch_add(&server_starts, 1U) + 1U);
    if (slow_start) usleep(10000);
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    assert(handle != NULL);
    if (stop_fails) return ESP_FAIL;
    server_stops++;
    registered_get = NULL;
    registered_post = NULL;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                     const httpd_uri_t *uri)
{
    assert(handle != NULL);
    assert(uri != NULL);
    if (uri->method == HTTP_GET) {
        registered_get = uri->handler;
    } else if (uri->method == HTTP_POST) {
        if (post_registration_fails) return ESP_FAIL;
        registered_post = uri->handler;
    }
    return ESP_OK;
}

bool httpd_uri_match_wildcard(const char *template,
                              const char *uri,
                              size_t len)
{
    (void)template;
    (void)uri;
    (void)len;
    return true;
}

static const char *request_header(const char *name)
{
    if (!strcmp(name, "Authorization")) return authorization;
    if (!strcmp(name, "X-Client")) return "fixture";
    return NULL;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field)
{
    assert(req);
    const char *value = request_header(field);
    return value ? strlen(value) : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field,
                                      char *value, size_t capacity)
{
    assert(req);
    const char *text = request_header(field);
    if (!text) return ESP_ERR_NOT_FOUND;
    return strlcpy(value, text, capacity) < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

int httpd_req_recv(httpd_req_t *req, char *out, size_t capacity)
{
    assert(req && out);
    if (read_times_out) return HTTPD_SOCK_ERR_TIMEOUT;
    const char body[] = "hello";
    assert(req->content_len <= sizeof(body) - 1 && req->body_offset <= req->content_len);
    const size_t remaining = req->content_len - req->body_offset;
    const size_t bytes = remaining < capacity ? remaining : capacity;
    memcpy(out, body + req->body_offset, bytes);
    req->body_offset += bytes;
    return bytes;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    assert(req && !strcmp(type, "text/plain"));
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, ssize_t length)
{
    assert(req && length == 2 && !memcmp(data, "OK", 2));
    sent++;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *data, ssize_t length)
{
    assert(req && length >= 0 && (!length || data));
    chunks++;
    return ESP_OK;
}

esp_err_t httpd_resp_send_404(httpd_req_t *req)
{
    (void)req;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *req,
                             const char *field,
                             const char *value)
{
    (void)req;
    (void)field;
    (void)value;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req,
                              httpd_err_code_t error,
                              const char *message)
{
    (void)req;
    if (error == HTTPD_401_UNAUTHORIZED) unauthorized++;
    (void)message;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    if (strcmp(status, "503 Service Unavailable") == 0) {
        service_unavailable++;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *message)
{
    (void)req;
    (void)message;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_begin(httpd_req_t *req,
                                        httpd_req_t **out)
{
    assert(req != NULL);
    assert(out != NULL);
    *out = malloc(sizeof(**out));
    assert(*out != NULL);
    **out = *req;
    async_begins++;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_complete(httpd_req_t *req)
{
    assert(req != NULL);
    async_completes++;
    free(req);
    return complete_fails ? ESP_FAIL : ESP_OK;
}

static esp_err_t sync_handler(httpd_req_t *req, void *user)
{
    assert(req != NULL);
    assert(user == (void *)0x1234);
    sync_calls++;
    return ESP_OK;
}

static esp_err_t async_handler(httpd_req_t *req, void *user)
{
    (void)user;
    assert(req != NULL);
    assert(accepted_async_count < 2U);
    accepted_async[accepted_async_count++] = req;
    return ESP_OK;
}

static esp_err_t inert_handler(httpd_req_t *req, void *user)
{
    assert(req && user);
    return ESP_OK;
}

static pthread_barrier_t register_together;
static void *register_route(void *arg)
{
    (void)pthread_barrier_wait(&register_together);
    assert(!solar_os_http_server_register_route(arg));
    return NULL;
}

static void test_parallel_registration(void)
{
    const unsigned starts = server_starts, stops = server_stops;
    int left, right;
    solar_os_http_route_t routes[] = {
        {.owner = "shared", .uri = "/left", .method = HTTP_GET,
         .handler = inert_handler, .user = &left},
        {.owner = "shared", .uri = "/right", .method = HTTP_POST,
         .handler = inert_handler, .user = &right},
    };
    assert(!pthread_barrier_init(&register_together, NULL, 2));
    pthread_t threads[2];
    slow_start = true;
    for (size_t i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, register_route, &routes[i]));
    for (size_t i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    slow_start = false;
    assert(!pthread_barrier_destroy(&register_together));
    assert(server_starts == starts + 1 && server_stops == stops);

    assert(!solar_os_http_server_unregister_owned("shared", &left));
    httpd_req_t req = {.uri = "/left", .method = HTTP_GET};
    assert(registered_get(&req) == ESP_ERR_NOT_FOUND);
    req = (httpd_req_t){.uri = "/right", .method = HTTP_POST};
    assert(!registered_post(&req));
    assert(!solar_os_http_server_unregister_owned("shared", &right));
    assert(server_stops == stops + 1);
}

static void test_retry_failed_stop(void)
{
    const unsigned starts = server_starts, stops = server_stops;
    int owner;
    const solar_os_http_route_t route = {
        .owner = "failed", .uri = "/failed", .method = HTTP_GET,
        .handler = inert_handler, .user = &owner,
    };
    post_registration_fails = stop_fails = true;
    assert(solar_os_http_server_register_route(&route) == ESP_FAIL);
    assert(solar_os_http_server_register_route(&route) == ESP_FAIL);
    assert(server_starts == starts + 1 && server_stops == stops);
    assert(!solar_os_http_server_get_bearer_token((char[7]){0}, 7));
    post_registration_fails = stop_fails = false;
    assert(!solar_os_http_server_register_route(&route));
    assert(server_starts == starts + 2 && server_stops == stops + 1);

    stop_fails = true;
    assert(solar_os_http_server_unregister_owned("failed", &owner) == ESP_FAIL);
    stop_fails = false;
    assert(solar_os_http_server_unregister_owned("failed", &owner) == ESP_ERR_NOT_FOUND);
    assert(server_starts == starts + 2 && server_stops == stops + 2);
}

static void *complete_request(void *arg)
{
    assert(!solar_os_http_server_complete_async(arg));
    return NULL;
}

static esp_err_t early_complete_handler(httpd_req_t *req, void *user)
{
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, complete_request, req));
    assert(!pthread_join(worker, NULL));
    assert(solar_os_http_server_unregister_owned("early", user) == ESP_ERR_INVALID_STATE);
    /* A worker completed the copy. Dispatch still owns the callback ref and
     * will release it without completing the same copy again. */
    return ESP_FAIL;
}

static void test_early_async_completion(void)
{
    const unsigned completions = async_completes, stops = server_stops;
    int owner;
    const solar_os_http_route_t route = {
        .owner = "early", .uri = "/early", .method = HTTP_GET, .asynchronous = true,
        .handler = early_complete_handler, .user = &owner,
    };
    httpd_req_t req = {.uri = "/early", .method = HTTP_GET};
    assert(!solar_os_http_server_register_route(&route));
    assert(!registered_get(&req));
    assert(async_completes == completions + 1 && server_stops == stops);
    assert(!solar_os_http_server_unregister_owned("early", &owner));
    assert(server_stops == stops + 1);
}

const solar_os_app_t *solar_os_application_descriptor(void)
{
    return app_scope ? &app : NULL;
}

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    void *memory = calloc(count, size);
    if (memory) allocations++;
    return memory;
}

void solar_os_memory_free(void *memory)
{
    if (memory) {
        assert(allocations);
        allocations--;
        free(memory);
    }
}

int solar_os_native_library_retain(uint32_t lease, uint32_t *retained)
{
    if (lease != 5) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *retained = 6;
    library_pins++;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(uint32_t lease)
{
    assert(lease == 6 && library_pins);
    library_pins--;
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_http_server_api_v1_t *api;

typedef struct {
    solar_os_native_handle_t route, request;
    bool async, refuse;
    unsigned callbacks;
} native_http_fixture_t;

static void *request_on_other_task(void *user)
{
    const native_http_fixture_t *fixture = user;
    solar_os_native_http_request_info_v1_t info;
    assert(api->request_info(fixture->request, &info) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return NULL;
}

static int native_request(solar_os_native_handle_t request, void *user)
{
    native_http_fixture_t *fixture = user;
    fixture->callbacks++;
    fixture->request = request;
    solar_os_native_http_request_info_v1_t info;
    assert(!api->request_info(request, &info) && !info.received);
    assert(info.asynchronous == fixture->async);
    assert(api->request_complete(request) == SOLAR_OS_NATIVE_ERROR_BUSY);
    if (fixture->async) {
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, request_on_other_task, fixture));
        assert(!pthread_join(thread, NULL));
        return fixture->refuse ? SOLAR_OS_NATIVE_ERROR_FAILED : SOLAR_OS_NATIVE_OK;
    }

    assert(info.method == SOLAR_OS_NATIVE_HTTP_SERVER_POST && info.content_length == 5);
    char text[32];
    assert(api->request_uri(request, text, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !text[0]);
    assert(!api->request_uri(request, text, sizeof(text)) && !strcmp(text, "/native?arg=1"));
    assert(!api->request_header(request, "X-Client", text, sizeof(text)) && !strcmp(text, "fixture"));
    size_t bytes;
    read_times_out = true;
    assert(api->request_read(request, text, 2, &bytes) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !bytes);
    read_times_out = false;
    assert(!api->request_read(request, text, 2, &bytes) && bytes == 2 && !memcmp(text, "he", bytes));
    assert(!api->request_info(request, &info) && info.received == 2);
    assert(!api->request_read(request, text, sizeof(text), &bytes) && bytes == 3 && !memcmp(text, "llo", bytes));

    const solar_os_native_http_reply_header_v1_t invalid = {.name = "X-Test", .value = "bad\r"};
    const solar_os_native_http_reply_v1_t reply = {
        .status = "202 Accepted", .content_type = "text/plain",
        .headers = &invalid, .header_count = 1,
    };
    assert(api->request_reply(request, &reply, "OK", 2) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->request_reply(request, NULL, "OK", 2));
    assert(api->request_header(request, "X-Client", text, sizeof(text)) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return SOLAR_OS_NATIVE_OK;
}

static void test_native_requests(void)
{
    api = solar_os_native_http_server_get_service("http.server", 1, sizeof(*api));
    assert(api && api->port() == 80);
    char token[7];
    assert(api->bearer_token(token, sizeof(token)) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    native_http_fixture_t fixture = {0};
    solar_os_native_http_route_v1_t route = {
        .label = "native", .uri = "/native",
        .method = SOLAR_OS_NATIVE_HTTP_SERVER_POST, .auth = SOLAR_OS_NATIVE_HTTP_AUTH_CONTROL,
        .on_request = native_request, .user = &fixture,
    };
    assert(api->route_register(&route, &fixture.route) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    app_scope = true;
    assert(!api->route_register(&route, &fixture.route) && !library_pins);
    assert(!api->bearer_token(token, sizeof(token)) && !strcmp(token, "123456"));
    httpd_req_t raw = {.uri = "/native?arg=1", .method = HTTP_POST, .content_len = 5};
    authorization = "Bearer 654321";
    assert(!registered_post(&raw) && unauthorized == 1 && !fixture.callbacks);
    authorization = "Bearer 123456";
    assert(!registered_post(&raw) && fixture.callbacks == 1 && sent == 1);
    solar_os_native_http_request_info_v1_t info;
    assert(api->request_info(fixture.request, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!solar_os_native_handle_close_owner(&app) && !allocations);
    app_scope = false;
    authorization = NULL;

    fixture = (native_http_fixture_t){.async = true, .refuse = true};
    route.asynchronous = true;
    route.method = SOLAR_OS_NATIVE_HTTP_SERVER_GET;
    route.auth = SOLAR_OS_NATIVE_HTTP_AUTH_PUBLIC;
    route.library_lease = 5;
    assert(!api->route_register(&route, &fixture.route) && library_pins == 1);
    solar_os_native_handle_t duplicate;
    assert(api->route_register(&route, &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!duplicate && library_pins == 1);
    raw = (httpd_req_t){.uri = "/native", .method = HTTP_GET};
    assert(!registered_get(&raw) && fixture.callbacks == 1);
    assert(api->request_info(fixture.request, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);

    fixture.refuse = false;
    assert(!registered_get(&raw) && fixture.callbacks == 2);
    assert(!api->request_info(fixture.request, &info) && info.asynchronous);
    assert(api->route_close(fixture.route) == SOLAR_OS_NATIVE_ERROR_BUSY && library_pins == 1);
    assert(registered_get(&raw) == ESP_ERR_NOT_FOUND);
    const solar_os_native_http_reply_header_v1_t header = {.name = "X-Test", .value = "ok"};
    const solar_os_native_http_reply_v1_t reply = {
        .status = "200 OK", .content_type = "text/plain",
        .headers = &header, .header_count = 1, .chunked = true,
    };
    assert(!api->request_reply(fixture.request, &reply, "1", 1));
    assert(!api->request_chunk(fixture.request, "2", 1));
    assert(!api->request_chunk(fixture.request, NULL, 0) && chunks == 3);
    assert(api->request_chunk(fixture.request, "3", 1) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);

    complete_fails = true;
    const unsigned completed = async_completes;
    assert(api->request_complete(fixture.request) == SOLAR_OS_NATIVE_ERROR_FAILED);
    complete_fails = false;
    assert(async_completes == completed + 1);
    assert(!api->request_complete(fixture.request) && async_completes == completed + 1);
    assert(!api->route_close(fixture.route) && !library_pins);
    assert(!solar_os_native_handle_count() && !allocations);
}

int main(void)
{
    const solar_os_http_route_t sync_route = {
        .owner = "sync",
        .uri = "/sync",
        .method = HTTP_GET,
        .auth = SOLAR_OS_HTTP_AUTH_PUBLIC,
        .handler = sync_handler,
        .user = (void *)0x1234,
    };
    assert(solar_os_http_server_register_route(&sync_route) == ESP_OK);
    assert(server_starts == 1U);
    assert(registered_get != NULL && registered_post != NULL);
    httpd_req_t request = {.uri = "/sync", .method = HTTP_GET};
    assert(registered_get(&request) == ESP_OK);
    assert(sync_calls == 1U);
    assert(solar_os_http_server_unregister_owner("sync") == ESP_OK);
    assert(server_stops == 1U);

    const solar_os_http_route_t async_route = {
        .owner = "async",
        .uri = "/stream",
        .method = HTTP_GET,
        .asynchronous = true,
        .auth = SOLAR_OS_HTTP_AUTH_PUBLIC,
        .handler = async_handler,
    };
    assert(solar_os_http_server_register_route(&async_route) == ESP_OK);
    assert(server_starts == 2U);

    request = (httpd_req_t) {.uri = "/stream", .method = HTTP_GET};
    assert(registered_get(&request) == ESP_OK);
    assert(registered_get(&request) == ESP_OK);
    assert(accepted_async_count == 2U);
    assert(async_begins == 2U);

    /* The fixed async capacity rejects a third request without calling the job. */
    assert(registered_get(&request) == ESP_OK);
    assert(accepted_async_count == 2U);
    assert(async_begins == 3U);
    assert(async_completes == 1U);
    assert(service_unavailable == 1U);

    /* In-flight references prevent route/server teardown. */
    assert(solar_os_http_server_unregister_owner("async") == ESP_ERR_TIMEOUT);
    assert(server_stops == 1U);
    assert(solar_os_http_server_complete_async(accepted_async[0]) == ESP_OK);
    assert(server_stops == 1U);
    assert(solar_os_http_server_complete_async(accepted_async[1]) == ESP_OK);
    assert(server_stops == 2U);
    assert(async_completes == 3U);
    assert(solar_os_http_server_complete_async(accepted_async[1]) ==
           ESP_ERR_INVALID_STATE);

    test_parallel_registration();
    test_retry_failed_stop();
    test_early_async_completion();
    test_native_requests();
    puts("http server tests: ok");
    return 0;
}
