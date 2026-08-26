#include "solar_os_json_scan.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define JSON_SCAN_NESTING_MAX 16U

static const char *json_scan_skip_ws(const char *p)
{
    while (p != NULL && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *json_scan_string(const char *p,
                                    char *out,
                                    size_t out_len,
                                    bool *truncated)
{
    if (p == NULL || *p != '"' || out == NULL || out_len == 0U) {
        return NULL;
    }

    p++;
    size_t used = 0U;
    out[0] = '\0';
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[used] = '\0';
            return p;
        }
        if (ch < 0x20U) {
            return NULL;
        }
        if (ch == '\\') {
            ch = (unsigned char)*p++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u':
                for (size_t i = 0U; i < 4U; i++) {
                    if (*p == '\0' || !isxdigit((unsigned char)*p)) {
                        return NULL;
                    }
                    p++;
                }
                ch = '?';
                break;
            default:
                return NULL;
            }
        }

        if (used + 1U < out_len) {
            out[used++] = (char)ch;
        } else if (truncated != NULL) {
            *truncated = true;
        }
    }
    return NULL;
}

static const char *json_scan_skip_value(const char *p)
{
    p = json_scan_skip_ws(p);
    if (p == NULL || *p == '\0') {
        return NULL;
    }
    if (*p == '"') {
        char scratch[1];
        return json_scan_string(p, scratch, sizeof(scratch), NULL);
    }
    if (*p == '{' || *p == '[') {
        char closing[JSON_SCAN_NESTING_MAX];
        size_t depth = 0U;
        closing[depth++] = *p++ == '{' ? '}' : ']';
        while (*p != '\0') {
            if (*p == '"') {
                char scratch[1];
                p = json_scan_string(p, scratch, sizeof(scratch), NULL);
                if (p == NULL) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                if (depth >= JSON_SCAN_NESTING_MAX) {
                    return NULL;
                }
                closing[depth++] = *p == '{' ? '}' : ']';
            } else if (*p == '}' || *p == ']') {
                if (depth == 0U || *p != closing[depth - 1U]) {
                    return NULL;
                }
                depth--;
                if (depth == 0U) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }

    const char *start = p;
    while (*p != '\0' && *p != ',' && *p != '}') {
        p++;
    }
    return p != start ? p : NULL;
}

static const char *json_scan_member_value(const char *json,
                                          const char *key)
{
    if (json == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    const char *p = json_scan_skip_ws(json);
    if (p == NULL || *p != '{') {
        return NULL;
    }
    p++;

    while (*p != '\0') {
        p = json_scan_skip_ws(p);
        if (p == NULL || *p == '}') {
            return NULL;
        }

        char member[64];
        bool member_truncated = false;
        p = json_scan_string(p, member, sizeof(member), &member_truncated);
        if (p == NULL) {
            return NULL;
        }
        p = json_scan_skip_ws(p);
        if (p == NULL || *p != ':') {
            return NULL;
        }
        p = json_scan_skip_ws(p + 1);
        if (p == NULL) {
            return NULL;
        }
        if (!member_truncated && strcmp(member, key) == 0) {
            return p;
        }

        p = json_scan_skip_value(p);
        p = json_scan_skip_ws(p);
        if (p == NULL) {
            return NULL;
        }
        if (*p == ',') {
            p++;
        } else if (*p == '}') {
            return NULL;
        } else {
            return NULL;
        }
    }
    return NULL;
}

bool solar_os_json_scan_object_string(const char *json,
                                      const char *key,
                                      char *out,
                                      size_t out_len,
                                      bool *truncated)
{
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    const char *value = json_scan_member_value(json, key);
    if (value == NULL || *value != '"') {
        return false;
    }
    const char *end = json_scan_string(value, out, out_len, truncated);
    end = json_scan_skip_ws(end);
    return end != NULL && (*end == ',' || *end == '}');
}

bool solar_os_json_scan_object_uint64(const char *json,
                                      const char *key,
                                      uint64_t *out)
{
    if (out == NULL) {
        return false;
    }
    const char *p = json_scan_member_value(json, key);
    if (p == NULL || !isdigit((unsigned char)*p)) {
        return false;
    }

    uint64_t value = 0U;
    do {
        const uint8_t digit = (uint8_t)(*p++ - '0');
        if (value > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    } while (isdigit((unsigned char)*p));

    p = json_scan_skip_ws(p);
    if (p == NULL || (*p != ',' && *p != '}')) {
        return false;
    }
    *out = value;
    return true;
}

esp_err_t solar_os_json_escape_string(const char *source,
                                      char *out,
                                      size_t out_len)
{
    if (source == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t written = 0U;
    out[0] = '\0';
    for (const unsigned char *p = (const unsigned char *)source;
         *p != '\0';
         p++) {
        char escaped[7];
        const char *chunk = escaped;

        switch (*p) {
        case '"':
            chunk = "\\\"";
            break;
        case '\\':
            chunk = "\\\\";
            break;
        case '\b':
            chunk = "\\b";
            break;
        case '\f':
            chunk = "\\f";
            break;
        case '\n':
            chunk = "\\n";
            break;
        case '\r':
            chunk = "\\r";
            break;
        case '\t':
            chunk = "\\t";
            break;
        default:
            if (*p < 0x20U) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p);
            } else {
                escaped[0] = (char)*p;
                escaped[1] = '\0';
            }
            break;
        }

        const size_t chunk_len = strlen(chunk);
        if (written + chunk_len >= out_len) {
            out[out_len - 1U] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out + written, chunk, chunk_len);
        written += chunk_len;
        out[written] = '\0';
    }
    return ESP_OK;
}
