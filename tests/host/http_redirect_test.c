#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "solar_os_http_client.h"
#include "solar_os_http_policy.h"
#define HTTP_METHOD_GET 100
#define HTTP_AUTH_TYPE_NONE 0
#define SOLAR_OS_MEMORY_EXTERNAL_PREFERRED 0
typedef struct {
    bool authorization, cookie, proxy, username, password;
    int method;
    char url[SOLAR_OS_HTTP_URL_MAX], host[280];
} client_t;
typedef client_t *esp_http_client_handle_t;
struct solar_os_http_request {
    solar_os_http_request_options_t options;
    char *redirect_location, *effective_url;
};
static void *solar_os_memory_alloc(size_t size, int cls, const char *tag)
{ (void)cls; (void)tag; return malloc(size); }
static void solar_os_memory_free(void *ptr) { free(ptr); }
static esp_err_t esp_http_client_delete_header(client_t *c, const char *name)
{
    if (!strcasecmp(name, "Authorization")) c->authorization = false;
    else if (!strcasecmp(name, "Cookie")) c->cookie = false;
    else if (!strcasecmp(name, "Proxy-Authorization")) c->proxy = false;
    else return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
static esp_err_t esp_http_client_set_username(client_t *c, const char *name)
{ assert(!name); c->username = false; return ESP_OK; }
static esp_err_t esp_http_client_set_password(client_t *c, const char *name)
{ assert(!name); c->password = false; return ESP_OK; }
static esp_err_t esp_http_client_set_authtype(client_t *c, int type)
{ (void)c; assert(type == HTTP_AUTH_TYPE_NONE); return ESP_OK; }
static esp_err_t esp_http_client_set_url(client_t *c, const char *url)
{ assert(strlen(url) < sizeof(c->url)); strcpy(c->url, url); return ESP_OK; }
static esp_err_t esp_http_client_set_header(client_t *c, const char *name, const char *value)
{ assert(!strcmp(name, "Host")); assert(strlen(value) < sizeof(c->host)); strcpy(c->host, value); return ESP_OK; }
static esp_err_t esp_http_client_set_method(client_t *c, int method)
{ c->method = method; return ESP_OK; }
#include "http_follow_redirect.inc"

int main(void)
{
    static const int statuses[] = {301, 302, 303, 307, 308};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        client_t client = {.authorization = true, .cookie = true, .proxy = true, .username = true, .password = true};
        solar_os_http_request_t request = {
            .options = {.url = "https://a.example/start", .method = SOLAR_OS_HTTP_METHOD_POST, .body = "secret", .body_len = 6},
            .redirect_location = "/same",
        };
        assert(solar_os_http_follow_redirect(&request, &client, 307) == ESP_OK);
        assert(client.authorization && client.cookie && client.username);
        request.redirect_location = "https://b.example:443/next";
        assert(solar_os_http_follow_redirect(&request, &client, statuses[i]) == ESP_OK);
        assert(!client.authorization && !client.cookie && !client.proxy && !client.username && !client.password);
        assert(!strcmp(client.host, "b.example:443"));
        const bool rewrite = statuses[i] == 301 || statuses[i] == 302 || statuses[i] == 303;
        assert(request.options.method == (rewrite ? SOLAR_OS_HTTP_METHOD_GET : SOLAR_OS_HTTP_METHOD_POST));
        assert(request.options.body_len == (rewrite ? 0U : 6U));
        /* A return hop never restores credentials discarded earlier. */
        request.redirect_location = "https://a.example/back";
        assert(solar_os_http_follow_redirect(&request, &client, 307) == ESP_OK);
        assert(!client.authorization && !client.cookie);
        request.redirect_location = "http://a.example/insecure";
        assert(solar_os_http_follow_redirect(&request, &client, 307) != ESP_OK);
        assert(!strcmp(client.url, "https://a.example/back"));
        free(request.effective_url);
    }
}
