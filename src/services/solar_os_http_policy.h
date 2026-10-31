#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_HTTP_URL_MAX 2048U
typedef struct {
    char host[256];
    uint16_t port;
    bool tls;
} solar_os_http_origin_t;

bool solar_os_http_origin_parse(const char *url, solar_os_http_origin_t *origin);
bool solar_os_http_same_origin(const solar_os_http_origin_t *a, const solar_os_http_origin_t *b);
bool solar_os_http_redirect_status(int status);
/* Resolve relative HTTP references. Redirect targets cannot contain userinfo
 * or downgrade HTTPS. Output never aliases either input. */
bool solar_os_http_redirect_url(const char *base, const char *location, char *out, size_t capacity);
