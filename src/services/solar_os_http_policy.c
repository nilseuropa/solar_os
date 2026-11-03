#include "solar_os_http_policy.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "http_parser.h"

bool solar_os_http_origin_parse(const char *url, solar_os_http_origin_t *origin)
{
    if (!url || !origin) return false;
    const size_t length = strnlen(url, SOLAR_OS_HTTP_URL_MAX);
    if (!length || length >= SOLAR_OS_HTTP_URL_MAX) return false;
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(url, length, 0, &parsed) ||
        !(parsed.field_set & (1U << UF_SCHEMA)) || !(parsed.field_set & (1U << UF_HOST))) return false;
    const size_t scheme = parsed.field_data[UF_SCHEMA].len;
    const bool tls = scheme == 5 && !strncasecmp(url, "https", 5);
    if (!tls && !(scheme == 4 && !strncasecmp(url, "http", 4))) return false;
    size_t host_length = parsed.field_data[UF_HOST].len;
    if (!host_length || host_length >= sizeof(origin->host)) return false;
    memcpy(origin->host, url + parsed.field_data[UF_HOST].off, host_length);
    origin->host[host_length] = 0;
    origin->tls = tls;
    origin->port = parsed.field_set & (1U << UF_PORT) ? parsed.port : (tls ? 443U : 80U);
    return origin->port != 0U;
}

bool solar_os_http_same_origin(const solar_os_http_origin_t *a, const solar_os_http_origin_t *b)
{
    return a->tls == b->tls && a->port == b->port && !strcasecmp(a->host, b->host);
}

bool solar_os_http_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static void normalize_path(char *url, const struct http_parser_url *parsed)
{
    if (!(parsed->field_set & (1U << UF_PATH))) return;
    char *path = url + parsed->field_data[UF_PATH].off;
    char *end = path + parsed->field_data[UF_PATH].len;
    char *read = path, *write = path;
    while (read < end) {
        const size_t remaining = (size_t)(end - read);
        if (remaining >= 3 && !memcmp(read, "/./", 3)) read += 2;
        else if (remaining == 2 && !memcmp(read, "/.", 2)) {
            read += 2; *write++ = '/';
        } else if ((remaining >= 4 && !memcmp(read, "/../", 4)) ||
                   (remaining == 3 && !memcmp(read, "/..", 3))) {
            read += 3;
            while (write > path && write[-1] != '/') --write;
            if (write > path) --write;
            if (read == end) *write++ = '/';
        } else {
            *write++ = *read++;
            while (read < end && *read != '/') *write++ = *read++;
        }
    }
    memmove(write, end, strlen(end) + 1U);
}

bool solar_os_http_redirect_url(const char *base, const char *location, char *out, size_t capacity)
{
    solar_os_http_origin_t before, after;
    if (!location || !*location || !out || !capacity || !solar_os_http_origin_parse(base, &before)) return false;
    if (strnlen(location, SOLAR_OS_HTTP_URL_MAX) >= SOLAR_OS_HTTP_URL_MAX) return false;
    const char *authority = strstr(base, "://") + 3;
    const char *authority_end = authority + strcspn(authority, "/?#");
    const char *end = base + strcspn(base, "#");
    const char *prefix_end = authority_end;
    const char *separator = "";
    if (!strncmp(location, "//", 2)) {
        prefix_end = authority - 2; /* Keep scheme and colon. */
    } else if (strstr(location, "://")) {
        prefix_end = base;
    } else if (*location == '?') {
        prefix_end = base + strcspn(base, "?#");
        if (prefix_end == authority_end) separator = "/";
    } else if (*location == '#') {
        prefix_end = end;
    } else if (*location != '/') {
        const char *path_end = base + strcspn(base, "?#");
        prefix_end = authority_end;
        for (const char *p = authority_end; p < path_end; ++p)
            if (*p == '/') prefix_end = p + 1;
        if (prefix_end == authority_end) separator = "/";
    }
    int written = snprintf(out, capacity, "%.*s%s%s", (int)(prefix_end - base), base, separator, location);
    if (written < 0 || (size_t)written >= capacity || !solar_os_http_origin_parse(out, &after)) return false;
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(out, strlen(out), 0, &parsed) ||
        (parsed.field_set & (1U << UF_USERINFO)) || (before.tls && !after.tls)) return false;
    normalize_path(out, &parsed);
    return true;
}
