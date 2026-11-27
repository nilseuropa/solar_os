#include "solar_os_native_http_server.h"
#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_HTTP_SERVER
#include "solar_os.h"
#include "solar_os_http_server.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_native_runtime.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const solar_os_app_t *app;
    int (*on_request)(solar_os_native_handle_t request, void *user);
    void *user;
    uint32_t library_lease;
    char owner[SOLAR_OS_HTTP_ROUTE_OWNER_MAX];
    atomic_bool opening;
    bool asynchronous, registered;
} native_http_route_t;

typedef enum {
    HTTP_REPLY_UNSENT,
    HTTP_REPLY_CHUNKS,
    HTTP_REPLY_FINISHED,
} native_http_reply_state_t;

typedef struct {
    httpd_req_t *request;
    solar_os_native_handle_t id;
    size_t received;
    native_http_reply_state_t reply_state;
    bool asynchronous, accepted;
} native_http_request_t;

static __thread native_http_request_t *callback_request;

static int request_close(void *opaque)
{
    native_http_request_t *state = opaque;
    if (state->accepted && state->request) {
        const esp_err_t result = solar_os_http_server_complete_async(state->request);
        state->request = NULL;
        if (result != ESP_OK) return solar_os_native_result(result);
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int request_complete(solar_os_native_handle_t request)
{
    return solar_os_native_handle_close_kind(request, SOLAR_OS_NATIVE_HANDLE_HTTP_SERVER_REQUEST);
}

static esp_err_t dispatch_request(httpd_req_t *request, void *opaque)
{
    native_http_route_t *route = opaque;
    native_http_request_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.http.request");
    if (!state) return ESP_ERR_NO_MEM;
    state->request = request;
    state->asynchronous = route->asynchronous;
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_HTTP_SERVER_REQUEST;
    solar_os_native_handle_t id;
    int result = solar_os_native_handle_create_exclusive(kind, state, request_close, route->app, &id);
    if (result != SOLAR_OS_NATIVE_OK) {
        solar_os_memory_free(state);
        return solar_os_native_callback_result(result);
    }
    state->id = id;

    native_http_request_t *previous = callback_request;
    callback_request = state;
    result = route->on_request(id, route->user);
    callback_request = previous;
    if (result == SOLAR_OS_NATIVE_OK && state->asynchronous) {
        state->accepted = true;
        solar_os_native_handle_release_exclusive(id);
    } else {
        state->request = NULL;
        (void)solar_os_native_handle_close_exclusive(id, kind);
    }
    return solar_os_native_callback_result(result);
}

static int route_destroy(void *opaque)
{
    native_http_route_t *route = opaque;
    if (atomic_load(&route->opening)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (route->registered) {
        const esp_err_t result = solar_os_http_server_unregister_owned(route->owner, route);
        if (result == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) return solar_os_native_result(result);
        route->registered = false;
    }
    if (route->library_lease) {
        const int result = solar_os_native_library_release(route->library_lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    solar_os_memory_free(route);
    return SOLAR_OS_NATIVE_OK;
}

static int route_close(solar_os_native_handle_t route)
{
    return solar_os_native_handle_close_kind(route, SOLAR_OS_NATIVE_HANDLE_HTTP_SERVER_ROUTE);
}

static int route_register(const solar_os_native_http_route_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->label || !*in->label || !in->on_request || !in->uri ||
        in->method > SOLAR_OS_NATIVE_HTTP_SERVER_POST || in->auth > SOLAR_OS_NATIVE_HTTP_AUTH_CONTROL)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_app_t *app = in->library_lease ? NULL : solar_os_application_descriptor();
    if (!app && !in->library_lease) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;

    native_http_route_t *route = solar_os_memory_calloc(1, sizeof(*route),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.http.route");
    if (!route) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    route->app = app;
    route->asynchronous = in->asynchronous;
    route->on_request = in->on_request;
    route->user = in->user;
    atomic_init(&route->opening, true);
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_HTTP_SERVER_ROUTE;
    int result = app ?
        solar_os_native_handle_create_owned(kind, route, route_destroy, app, out) :
        solar_os_native_handle_create(kind, route, route_destroy, out);
    if (result != SOLAR_OS_NATIVE_OK) {
        solar_os_memory_free(route);
        return result;
    }
    if (in->library_lease) result = solar_os_native_library_retain(in->library_lease, &route->library_lease);
    if (!result) {
        snprintf(route->owner, sizeof(route->owner), "%08lx/%.14s", (unsigned long)*out, in->label);
        _Static_assert(SOLAR_OS_NATIVE_HTTP_AUTH_PUBLIC == SOLAR_OS_HTTP_AUTH_PUBLIC &&
                       SOLAR_OS_NATIVE_HTTP_AUTH_VIEW == SOLAR_OS_HTTP_AUTH_VIEW &&
                       SOLAR_OS_NATIVE_HTTP_AUTH_CONTROL == SOLAR_OS_HTTP_AUTH_CONTROL,
                       "HTTP authentication values");
        const solar_os_http_route_t descriptor = {
            .owner = route->owner,
            .uri = in->uri,
            .method = in->method == SOLAR_OS_NATIVE_HTTP_SERVER_GET ? HTTP_GET : HTTP_POST,
            .prefix = in->prefix,
            .asynchronous = in->asynchronous,
            .auth = in->auth,
            .handler = dispatch_request,
            .user = route,
        };
        result = solar_os_native_result(solar_os_http_server_register_route(&descriptor));
        route->registered = result == SOLAR_OS_NATIVE_OK;
    }
    atomic_store(&route->opening, false);
    if (result) {
        const int close_result = route_close(*out);
        if (close_result == SOLAR_OS_NATIVE_OK || close_result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) *out = 0;
    }
    return result;
}

static uint32_t server_port(void)
{
    return solar_os_http_server_port();
}

static int server_bearer_token(char *text, size_t capacity)
{
    if (!text || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *text = '\0';
    if (capacity < SOLAR_OS_NATIVE_HTTP_BEARER_TOKEN_CAPACITY) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_http_server_get_bearer_token(text, capacity) ? SOLAR_OS_NATIVE_OK :
        SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int request_enter(solar_os_native_handle_t id, native_http_request_t **out)
{
    if (callback_request && callback_request->id == id) {
        *out = callback_request;
        return SOLAR_OS_NATIVE_OK;
    }
    void *state;
    int result = solar_os_native_handle_acquire_exclusive(id, SOLAR_OS_NATIVE_HANDLE_HTTP_SERVER_REQUEST, &state);
    if (result) return result;
    native_http_request_t *request = state;
    if (!request->request) {
        solar_os_native_handle_release_exclusive(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *out = request;
    return SOLAR_OS_NATIVE_OK;
}

static void request_leave(const native_http_request_t *state)
{
    if (state != callback_request) solar_os_native_handle_release_exclusive(state->id);
}

static int request_info(solar_os_native_handle_t id, solar_os_native_http_request_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_http_request_info_v1_t){0};
    native_http_request_t *state;
    const int result = request_enter(id, &state);
    if (result) return result;
    *out = (solar_os_native_http_request_info_v1_t){
        .method = state->request->method == HTTP_GET ? SOLAR_OS_NATIVE_HTTP_SERVER_GET : SOLAR_OS_NATIVE_HTTP_SERVER_POST,
        .content_length = state->request->content_len,
        .received = state->received,
        .asynchronous = state->asynchronous,
    };
    request_leave(state);
    return SOLAR_OS_NATIVE_OK;
}

static int request_uri(solar_os_native_handle_t id, char *path, size_t capacity)
{
    if (!path || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *path = '\0';
    native_http_request_t *state;
    int result = request_enter(id, &state);
    if (result) return result;
    if (strlcpy(path, state->request->uri, capacity) >= capacity) {
        *path = '\0';
        result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    request_leave(state);
    return result;
}

static int request_header(solar_os_native_handle_t id, const char *name, char *value, size_t capacity)
{
    if (!name || !*name || !value || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *value = '\0';
    native_http_request_t *state;
    int result = request_enter(id, &state);
    if (result) return result;
    if (state->reply_state != HTTP_REPLY_UNSENT) {
        result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    } else {
        const size_t need = httpd_req_get_hdr_value_len(state->request, name);
        if (!need) result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        else if (need >= capacity) result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        else result = solar_os_native_result(httpd_req_get_hdr_value_str(state->request, name, value, capacity));
    }
    if (result) *value = '\0';
    request_leave(state);
    return result;
}

static int request_read(solar_os_native_handle_t id, void *data, size_t capacity, size_t *bytes)
{
    if (!bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *bytes = 0;
    if (!data || !capacity || capacity > INT_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_http_request_t *state;
    int result = request_enter(id, &state);
    if (result) return result;
    if (state->reply_state != HTTP_REPLY_UNSENT) {
        result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    } else {
        const int received = httpd_req_recv(state->request, data, capacity);
        if (received < 0) {
            result = received == HTTPD_SOCK_ERR_TIMEOUT ? SOLAR_OS_NATIVE_ERROR_TIMEOUT : SOLAR_OS_NATIVE_ERROR_IO;
        } else {
            *bytes = (size_t)received;
            state->received += *bytes;
        }
    }
    request_leave(state);
    return result;
}

static bool header_name_valid(const char *name)
{
    if (!name || !*name) return false;
    static const char separators[] = "()<>@,;:\\\"/[]?={} \t";
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 33 || *p > 126 || strchr(separators, *p)) return false;
    }
    return true;
}

static bool header_value_valid(const char *value)
{
    if (!value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p < 32 && *p != '\t') || *p == 127) return false;
    }
    return true;
}

static bool reply_valid(const solar_os_native_http_reply_v1_t *reply)
{
    if (!reply) return true;
    if (reply->status) {
        const char *status = reply->status;
        if (strlen(status) < 5 || status[0] < '1' || status[0] > '5' ||
            status[1] < '0' || status[1] > '9' || status[2] < '0' || status[2] > '9' ||
            status[3] != ' ' || !header_value_valid(status)) return false;
    }
    if (reply->content_type && (!*reply->content_type || !header_value_valid(reply->content_type)))
        return false;
    if ((reply->header_count && !reply->headers) || reply->header_count > 8U) return false;
    for (size_t i = 0; i < reply->header_count; i++) {
        const solar_os_native_http_reply_header_v1_t *header = &reply->headers[i];
        if (!header_name_valid(header->name) || !header_value_valid(header->value) ||
            !strcasecmp(header->name, "Content-Length") ||
            !strcasecmp(header->name, "Transfer-Encoding") ||
            !strcasecmp(header->name, "Content-Type")) return false;
    }
    return true;
}

static esp_err_t apply_reply(httpd_req_t *request, const solar_os_native_http_reply_v1_t *reply)
{
    if (!reply) return ESP_OK;
    esp_err_t result = reply->status ? httpd_resp_set_status(request, reply->status) : ESP_OK;
    if (result == ESP_OK && reply->content_type) result = httpd_resp_set_type(request, reply->content_type);
    for (size_t i = 0; result == ESP_OK && i < reply->header_count; i++) {
        result = httpd_resp_set_hdr(request, reply->headers[i].name, reply->headers[i].value);
    }
    return result;
}

static int request_reply(solar_os_native_handle_t id, const solar_os_native_http_reply_v1_t *reply,
                          const void *data, size_t bytes)
{
    if ((!data && bytes) || bytes > INT_MAX || !reply_valid(reply) || (reply && reply->chunked && !bytes))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_http_request_t *state;
    int result = request_enter(id, &state);
    if (result) return result;
    if (state->reply_state != HTTP_REPLY_UNSENT) {
        result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    } else {
        const bool chunked = reply && reply->chunked;
        esp_err_t error = apply_reply(state->request, reply);
        if (error == ESP_OK) {
            error = chunked ? httpd_resp_send_chunk(state->request, data, bytes) :
                httpd_resp_send(state->request, data, bytes);
        }
        state->reply_state = error == ESP_OK && chunked ? HTTP_REPLY_CHUNKS : HTTP_REPLY_FINISHED;
        result = solar_os_native_result(error);
    }
    request_leave(state);
    return result;
}

static int request_chunk(solar_os_native_handle_t id, const void *data, size_t bytes)
{
    if ((!data && bytes) || bytes > INT_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_http_request_t *state;
    int result = request_enter(id, &state);
    if (result) return result;
    if (state->reply_state != HTTP_REPLY_CHUNKS) {
        result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    } else {
        const esp_err_t error = httpd_resp_send_chunk(state->request, data, bytes);
        if (!bytes || error != ESP_OK) state->reply_state = HTTP_REPLY_FINISHED;
        result = solar_os_native_result(error);
    }
    request_leave(state);
    return result;
}

static const solar_os_native_http_server_api_v1_t http_server_api = {
    .abi_version = SOLAR_OS_NATIVE_HTTP_SERVER_ABI,
    .struct_size = sizeof(http_server_api),
    .route_register = route_register,
    .route_close = route_close,
    .port = server_port,
    .bearer_token = server_bearer_token,
    .request_info = request_info,
    .request_uri = request_uri,
    .request_header = request_header,
    .request_read = request_read,
    .request_reply = request_reply,
    .request_chunk = request_chunk,
    .request_complete = request_complete,
};
#endif

const void *solar_os_native_http_server_get_service(const char *name, uint32_t abi,
                                                     uint32_t minimum)
{
#include "generated/native_http_server_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
