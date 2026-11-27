#include "solar_os_http_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_log.h"
#include "solar_os_sync.h"
#include "solar_os_task.h"

#define HTTP_SERVER_ROUTE_MAX 10
#define HTTP_SERVER_ASYNC_MAX 2
#define HTTP_SERVER_STACK_SIZE 6144
#define HTTP_SERVER_STOP_WAIT_MS 3000U

static const char *TAG = "http_server";

typedef struct {
    bool active;
    size_t refs;
    char owner[SOLAR_OS_HTTP_ROUTE_OWNER_MAX];
    char uri[SOLAR_OS_HTTP_ROUTE_URI_MAX];
    httpd_method_t method;
    bool prefix;
    bool asynchronous;
    solar_os_http_auth_t auth;
    solar_os_http_route_handler_t handler;
    void *user;
} http_route_slot_t;

typedef struct {
    httpd_req_t *request;
    size_t route_index;
} http_async_slot_t;

static EXT_RAM_BSS_ATTR http_route_slot_t route_slots[HTTP_SERVER_ROUTE_MAX];
static http_async_slot_t async_slots[HTTP_SERVER_ASYNC_MAX];
static httpd_handle_t http_server;
static bool http_server_ready;
static solar_os_lifecycle_t http_server_lifecycle;
static uint16_t http_server_listen_port = 80;
static char bearer_token[SOLAR_OS_HTTP_BEARER_TOKEN_MAX];
static portMUX_TYPE http_server_lock = portMUX_INITIALIZER_UNLOCKED;

static bool any_active_routes_locked(void);

static size_t request_path_len(const char *uri)
{
    if (uri == NULL) {
        return 0;
    }
    const char *query = strchr(uri, '?');
    return query != NULL ? (size_t)(query - uri) : strlen(uri);
}

static bool route_matches(const http_route_slot_t *slot,
                          httpd_method_t method,
                          const char *uri,
                          size_t uri_len)
{
    if (slot == NULL || !slot->active || slot->handler == NULL || slot->method != method) {
        return false;
    }

    const size_t route_len = strlen(slot->uri);
    if (slot->prefix) {
        return route_len <= uri_len && memcmp(slot->uri, uri, route_len) == 0;
    }
    return route_len == uri_len && memcmp(slot->uri, uri, uri_len) == 0;
}

static int find_route_locked(httpd_method_t method, const char *uri, size_t uri_len)
{
    int best = -1;
    size_t best_len = 0;
    bool best_exact = false;

    for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
        const http_route_slot_t *slot = &route_slots[i];
        if (!route_matches(slot, method, uri, uri_len)) {
            continue;
        }

        const size_t route_len = strlen(slot->uri);
        const bool exact = !slot->prefix;
        if (best < 0 ||
            (exact && !best_exact) ||
            (exact == best_exact && route_len > best_len)) {
            best = (int)i;
            best_len = route_len;
            best_exact = exact;
        }
    }
    return best;
}

static bool constant_time_equal(const char *left, const char *right, size_t len)
{
    unsigned diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned)(uint8_t)left[i] ^ (unsigned)(uint8_t)right[i];
    }
    return diff == 0;
}

static bool request_is_authorized(httpd_req_t *req, solar_os_http_auth_t auth)
{
    if (auth == SOLAR_OS_HTTP_AUTH_PUBLIC) {
        return true;
    }

    char token[SOLAR_OS_HTTP_BEARER_TOKEN_MAX];
    portENTER_CRITICAL(&http_server_lock);
    strlcpy(token, bearer_token, sizeof(token));
    portEXIT_CRITICAL(&http_server_lock);

    char expected[sizeof(token) + 7U];
    const int expected_len = snprintf(expected, sizeof(expected), "Bearer %s", token);
    if (expected_len <= 7 || (size_t)expected_len >= sizeof(expected)) {
        memset(token, 0, sizeof(token));
        return false;
    }

    const size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len != (size_t)expected_len) {
        memset(expected, 0, sizeof(expected));
        memset(token, 0, sizeof(token));
        return false;
    }

    char header[sizeof(expected)];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        memset(expected, 0, sizeof(expected));
        memset(token, 0, sizeof(token));
        return false;
    }
    const bool authorized = constant_time_equal(header, expected, (size_t)expected_len);
    memset(header, 0, sizeof(header));
    memset(expected, 0, sizeof(expected));
    memset(token, 0, sizeof(token));
    return authorized;
}

static bool any_route_refs_locked(void)
{
    for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
        if (route_slots[i].refs != 0U) {
            return true;
        }
    }
    return false;
}

/* Start and stop share a task mutex. Never wait for the server task while
 * holding a route reference on that task. */
static esp_err_t stop_idle_server_locked(void)
{
    portENTER_CRITICAL(&http_server_lock);
    httpd_handle_t server = (any_active_routes_locked() || any_route_refs_locked()) ?
        NULL : http_server;
    portEXIT_CRITICAL(&http_server_lock);
    if (!server) return ESP_OK;

    const esp_err_t err = httpd_stop(server);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&http_server_lock);
        http_server = NULL;
        http_server_ready = false;
        bearer_token[0] = '\0';
        portEXIT_CRITICAL(&http_server_lock);
        SOLAR_OS_LOGI(TAG, "stopped");
    }
    return err;
}

static esp_err_t stop_idle_server(void)
{
    portENTER_CRITICAL(&http_server_lock);
    const bool idle = http_server && !any_active_routes_locked() && !any_route_refs_locked();
    portEXIT_CRITICAL(&http_server_lock);
    if (!idle) return ESP_OK;

    if (!solar_os_lifecycle_enter(&http_server_lifecycle)) return ESP_ERR_NO_MEM;
    const esp_err_t err = stop_idle_server_locked();
    solar_os_lifecycle_leave(&http_server_lifecycle);
    return err;
}

static void clear_inactive_routes_locked(void)
{
    for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
        if (!route_slots[i].active && route_slots[i].refs == 0U) {
            memset(&route_slots[i], 0, sizeof(route_slots[i]));
        }
    }
}

static void release_route_ref(size_t route_index)
{
    portENTER_CRITICAL(&http_server_lock);
    if (route_index < HTTP_SERVER_ROUTE_MAX &&
        route_slots[route_index].refs > 0U) {
        route_slots[route_index].refs--;
    }
    portEXIT_CRITICAL(&http_server_lock);
}

static bool track_async_request(httpd_req_t *request, size_t route_index)
{
    bool tracked = false;
    portENTER_CRITICAL(&http_server_lock);
    for (size_t i = 0; i < HTTP_SERVER_ASYNC_MAX; i++) {
        if (async_slots[i].request == NULL) {
            async_slots[i].request = request;
            async_slots[i].route_index = route_index;
            route_slots[route_index].refs++;
            tracked = true;
            break;
        }
    }
    portEXIT_CRITICAL(&http_server_lock);
    return tracked;
}

static bool untrack_async_request(httpd_req_t *request, size_t *route_index)
{
    bool found = false;
    portENTER_CRITICAL(&http_server_lock);
    for (size_t i = 0; i < HTTP_SERVER_ASYNC_MAX; i++) {
        if (async_slots[i].request == request) {
            *route_index = async_slots[i].route_index;
            memset(&async_slots[i], 0, sizeof(async_slots[i]));
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&http_server_lock);
    return found;
}

static esp_err_t dispatch_request(httpd_req_t *req)
{
    if (req == NULL || req->uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t uri_len = request_path_len(req->uri);
    solar_os_http_route_handler_t handler = NULL;
    void *user = NULL;
    solar_os_http_auth_t auth = SOLAR_OS_HTTP_AUTH_PUBLIC;
    bool asynchronous = false;
    int route_index = -1;

    portENTER_CRITICAL(&http_server_lock);
    route_index = find_route_locked(req->method, req->uri, uri_len);
    if (route_index >= 0) {
        http_route_slot_t *slot = &route_slots[route_index];
        slot->refs++;
        handler = slot->handler;
        user = slot->user;
        auth = slot->auth;
        asynchronous = slot->asynchronous;
    }
    portEXIT_CRITICAL(&http_server_lock);

    if (handler == NULL) {
        return httpd_resp_send_404(req);
    }

    esp_err_t ret = ESP_OK;
    if (!request_is_authorized(req, auth)) {
        (void)httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        ret = httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "authentication required");
    } else if (!asynchronous) {
        ret = handler(req, user);
    } else {
        httpd_req_t *async_req = NULL;
        ret = httpd_req_async_handler_begin(req, &async_req);
        if (ret != ESP_OK) {
            (void)httpd_resp_set_status(req, "503 Service Unavailable");
            ret = httpd_resp_sendstr(req, "asynchronous request unavailable");
        } else if (!track_async_request(async_req, (size_t)route_index)) {
            (void)httpd_resp_set_status(async_req, "503 Service Unavailable");
            (void)httpd_resp_sendstr(async_req, "asynchronous request capacity exhausted");
            (void)httpd_req_async_handler_complete(async_req);
            ret = ESP_OK;
        } else {
            const esp_err_t accept_error = handler(async_req, user);
            if (accept_error != ESP_OK) {
                size_t copy_route = 0U;
                if (untrack_async_request(async_req, &copy_route)) {
                    (void)httpd_req_async_handler_complete(async_req);
                    release_route_ref(copy_route);
                }
                ret = ESP_OK;
            }
        }
    }

    release_route_ref((size_t)route_index);
    return ret;
}

static void generate_bearer_token(char token[SOLAR_OS_HTTP_BEARER_TOKEN_MAX])
{
    /*
     * Keep the browser pairing code practical to enter while avoiding modulo
     * bias across the six-digit range.
     */
    const uint32_t range = 1000000U;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    uint32_t random = 0;
    do {
        random = esp_random();
    } while (random >= limit);
    snprintf(token, SOLAR_OS_HTTP_BEARER_TOKEN_MAX, "%06u", (unsigned)(random % range));
    random = 0;
}

static esp_err_t start_server_locked(void)
{
    if (http_server_ready) return ESP_OK;
    const esp_err_t stop_error = stop_idle_server_locked();
    if (stop_error != ESP_OK) return stop_error;

    solar_os_task_managed_admission_t admission;
    if (!solar_os_task_admit_managed("http-server",
                                     HTTP_SERVER_STACK_SIZE,
                                     SOLAR_OS_TASK_ROLE_BACKGROUND,
                                     false,
                                     &admission)) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = HTTP_SERVER_STACK_SIZE;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    solar_os_task_note_managed_result("http-server",
                                      HTTP_SERVER_STACK_SIZE,
                                      SOLAR_OS_TASK_ROLE_BACKGROUND,
                                      &admission,
                                      ret == ESP_OK);
    if (ret != ESP_OK) {
        return ret;
    }

    const httpd_uri_t get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = dispatch_request,
    };
    ret = httpd_register_uri_handler(server, &get);
    if (ret == ESP_OK) {
        const httpd_uri_t post = {
            .uri = "/*",
            .method = HTTP_POST,
            .handler = dispatch_request,
        };
        ret = httpd_register_uri_handler(server, &post);
    }
    if (ret != ESP_OK) {
        if (httpd_stop(server) != ESP_OK) {
            portENTER_CRITICAL(&http_server_lock);
            http_server = server;
            portEXIT_CRITICAL(&http_server_lock);
        }
        return ret;
    }

    char token[SOLAR_OS_HTTP_BEARER_TOKEN_MAX];
    generate_bearer_token(token);
    portENTER_CRITICAL(&http_server_lock);
    http_server = server;
    http_server_ready = true;
    http_server_listen_port = config.server_port;
    strlcpy(bearer_token, token, sizeof(bearer_token));
    portEXIT_CRITICAL(&http_server_lock);
    memset(token, 0, sizeof(token));

    SOLAR_OS_LOGI(TAG, "started on port %u", (unsigned)config.server_port);
    return ESP_OK;
}

static bool any_active_routes_locked(void)
{
    for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
        if (route_slots[i].active) {
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_http_server_register_route(const solar_os_http_route_t *route)
{
    if (route == NULL ||
        route->owner == NULL ||
        route->owner[0] == '\0' ||
        strnlen(route->owner, SOLAR_OS_HTTP_ROUTE_OWNER_MAX) >= SOLAR_OS_HTTP_ROUTE_OWNER_MAX ||
        route->uri == NULL ||
        route->uri[0] != '/' ||
        strnlen(route->uri, SOLAR_OS_HTTP_ROUTE_URI_MAX) >= SOLAR_OS_HTTP_ROUTE_URI_MAX ||
        route->handler == NULL ||
        (route->method != HTTP_GET && route->method != HTTP_POST) ||
        (unsigned)route->auth > SOLAR_OS_HTTP_AUTH_CONTROL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!solar_os_lifecycle_enter(&http_server_lifecycle)) return ESP_ERR_NO_MEM;

    int free_index = -1;
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&http_server_lock);
    for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
        const http_route_slot_t *slot = &route_slots[i];
        if (slot->active && slot->method == route->method && !strcmp(slot->uri, route->uri)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if (!slot->active && !slot->refs && free_index < 0) free_index = (int)i;
    }
    portEXIT_CRITICAL(&http_server_lock);

    if (ret == ESP_OK) {
        ret = free_index >= 0 ? start_server_locked() : ESP_ERR_NO_MEM;
        if (ret == ESP_OK) {
            portENTER_CRITICAL(&http_server_lock);
            http_route_slot_t *slot = &route_slots[free_index];
            *slot = (http_route_slot_t){
                .active = true,
                .method = route->method,
                .prefix = route->prefix,
                .asynchronous = route->asynchronous,
                .auth = route->auth,
                .handler = route->handler,
                .user = route->user,
            };
            strlcpy(slot->owner, route->owner, sizeof(slot->owner));
            strlcpy(slot->uri, route->uri, sizeof(slot->uri));
            portEXIT_CRITICAL(&http_server_lock);
        }
    }
    solar_os_lifecycle_leave(&http_server_lifecycle);
    return ret;
}

static bool route_owned_by(const http_route_slot_t *slot, const char *owner, const void *user)
{
    return !strcmp(slot->owner, owner) && (user == NULL || slot->user == user);
}

static esp_err_t unregister_routes(const char *owner, const void *user, bool wait)
{
    if (!owner || !*owner || strnlen(owner, SOLAR_OS_HTTP_ROUTE_OWNER_MAX) >= SOLAR_OS_HTTP_ROUTE_OWNER_MAX)
        return ESP_ERR_INVALID_ARG;

    bool found = false;
    portENTER_CRITICAL(&http_server_lock);
    for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
        http_route_slot_t *slot = &route_slots[i];
        if (route_owned_by(slot, owner, user)) {
            slot->active = false;
            found = true;
        }
    }
    portEXIT_CRITICAL(&http_server_lock);

    for (uint32_t waited = 0; waited < HTTP_SERVER_STOP_WAIT_MS; waited += 10U) {
        bool busy = false;
        portENTER_CRITICAL(&http_server_lock);
        for (size_t i = 0; i < HTTP_SERVER_ROUTE_MAX; i++) {
            http_route_slot_t *slot = &route_slots[i];
            if (slot->active || !route_owned_by(slot, owner, user)) continue;
            if (slot->refs) busy = true;
            else memset(slot, 0, sizeof(*slot));
        }
        portEXIT_CRITICAL(&http_server_lock);
        if (!busy) {
            const esp_err_t err = stop_idle_server();
            return err == ESP_OK && !found ? ESP_ERR_NOT_FOUND : err;
        }
        if (!wait) return ESP_ERR_INVALID_STATE;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t solar_os_http_server_unregister_owner(const char *owner)
{
    return unregister_routes(owner, NULL, true);
}

esp_err_t solar_os_http_server_unregister_owned(const char *owner, const void *user)
{
    return user ? unregister_routes(owner, user, false) : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_http_server_complete_async(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t route_index = 0U;
    if (!untrack_async_request(req, &route_index)) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t complete_error = httpd_req_async_handler_complete(req);
    release_route_ref(route_index);
    portENTER_CRITICAL(&http_server_lock);
    clear_inactive_routes_locked();
    portEXIT_CRITICAL(&http_server_lock);
    const esp_err_t stop_error = stop_idle_server();
    return complete_error == ESP_OK ? stop_error : complete_error;
}

bool solar_os_http_server_get_bearer_token(char *token, size_t token_len)
{
    if (token == NULL || token_len < SOLAR_OS_HTTP_BEARER_TOKEN_MAX) {
        return false;
    }
    portENTER_CRITICAL(&http_server_lock);
    const bool available = bearer_token[0] != '\0';
    if (available) {
        strlcpy(token, bearer_token, token_len);
    } else {
        token[0] = '\0';
    }
    portEXIT_CRITICAL(&http_server_lock);
    return available;
}

uint16_t solar_os_http_server_port(void)
{
    portENTER_CRITICAL(&http_server_lock);
    const uint16_t port = http_server_listen_port;
    portEXIT_CRITICAL(&http_server_lock);
    return port;
}
