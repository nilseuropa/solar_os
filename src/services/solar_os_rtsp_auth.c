#include "solar_os_rtsp_auth.h"
#include "solar_os_secure_wipe.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "mbedtls/base64.h"
#include "mbedtls/md5.h"

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "solar_os_sync.h"
#define RTSP_AUTH_NVS_NAMESPACE "rtsp_auth"
#define RTSP_AUTH_NVS_KEY "accounts"
#endif

typedef struct {
    char host[SOLAR_OS_RTSP_AUTH_HOST_MAX + 1U];
    uint16_t port;
    char user[SOLAR_OS_RTSP_AUTH_USER_MAX + 1U];
    char password[SOLAR_OS_RTSP_AUTH_PASSWORD_MAX + 1U];
} rtsp_auth_record_t;

static rtsp_auth_record_t auth_records[SOLAR_OS_RTSP_AUTH_CAPACITY];
#ifdef ESP_PLATFORM
static bool auth_loaded;
static solar_os_lifecycle_t account_store;
#endif

void solar_os_rtsp_auth_wipe(void *buffer, size_t length)
{
    solar_os_secure_wipe(buffer, length);
}

static void hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static esp_err_t md5_hex(const char *text, size_t capacity, int formatted, char out[33])
{
    if (formatted < 0 || (size_t)formatted >= capacity) return ESP_ERR_INVALID_SIZE;
    uint8_t digest[16];
    const bool ok = mbedtls_md5((const unsigned char *)text, (size_t)formatted, digest) == 0;
    if (ok) hex_encode(digest, sizeof(digest), out);
    solar_os_rtsp_auth_wipe(digest, sizeof(digest));
    return ok ? ESP_OK : ESP_FAIL;
}

static bool token_char(unsigned char c)
{
    return c && (isalnum(c) || strchr("!#$%&'*+-.^_`|~", c));
}

/* Commas delimit parameters/challenges only outside quoted strings. A scheme
 * is a whole token followed by whitespace, rather than a parameter's '='. */
static const char *challenge_fields(const char *www, const char *scheme, const char **end)
{
    const char *fields = NULL, *p = www;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *token = p, *after = p;
        while (token_char((unsigned char)*after)) after++;
        const char *value = after;
        while (*value == ' ' || *value == '\t') value++;
        if (after > token && value > after && *value != '=') {
            if (fields) {
                *end = token;
                return fields;
            }
            size_t len = (size_t)(after - token);
            if (len == strlen(scheme) && !strncasecmp(token, scheme, len)) fields = value;
        }
        bool quoted = false;
        while (*p) {
            if (quoted && *p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '"') quoted = !quoted;
            if (*p == ',' && !quoted) { p++; break; }
            p++;
        }
        if (quoted) return NULL;
    }
    *end = p;
    return fields;
}

static const char *param(const char *p, const char *end, const char *name, char *out, size_t cap)
{
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        if (p == end) break;
        const char *token = p;
        while (p < end && token_char((unsigned char)*p)) p++;
        size_t len = (size_t)(p - token);
        bool match = len == strlen(name) && !strncasecmp(token, name, len);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (!len || p == end || *p++ != '=') return NULL;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        bool quoted = p < end && *p == '"';
        if (quoted) p++;
        size_t used = 0;
        while (p < end && (quoted ? *p != '"' : token_char((unsigned char)*p))) {
            unsigned char c = (unsigned char)*p++;
            if (quoted && c == '\\') {
                if (p == end) return NULL;
                c = (unsigned char)*p++;
            }
            if (c < 32 || c == 127) return NULL;
            if (match) {
                if (used + 1 >= cap) return NULL;
                out[used++] = (char)c;
            }
        }
        if (quoted) {
            if (p == end || *p++ != '"') return NULL;
        } else if (!used && match) return NULL;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p != ',') return NULL;
        if (match) { out[used] = '\0'; return out; }
    }
    return NULL;
}

esp_err_t solar_os_rtsp_auth_challenge(const char *www, solar_os_rtsp_auth_challenge_t *c)
{
    if (!www || !c || !*www) return ESP_ERR_INVALID_ARG;
    memset(c, 0, sizeof(*c));
    const char *end;
    const char *fields = challenge_fields(www, "Digest", &end);
    if (fields) {
        char qop[64] = "", algorithm[32] = "", stale[8] = "";
        c->digest = true;
        if (!param(fields, end, "realm", c->realm, sizeof(c->realm)) ||
            !param(fields, end, "nonce", c->nonce, sizeof(c->nonce)))
            return ESP_ERR_INVALID_RESPONSE;
        (void)param(fields, end, "opaque", c->opaque, sizeof(c->opaque));
        if (param(fields, end, "algorithm", algorithm, sizeof(algorithm)) &&
            algorithm[0] && strcasecmp(algorithm, "MD5"))
            return ESP_ERR_NOT_SUPPORTED;
        if (param(fields, end, "qop", qop, sizeof(qop)) && qop[0]) {
            bool auth = false, other = false;
            for (char *tok = qop; *tok;) {
                while (*tok == ' ' || *tok == ',') tok++;
                if (!*tok) break;
                char *comma = strchr(tok, ',');
                size_t n = comma ? (size_t)(comma - tok) : strlen(tok);
                while (n && tok[n - 1] == ' ') n--;
                if (n == 4 && !strncasecmp(tok, "auth", 4)) auth = true;
                else other = true;
                tok = comma ? comma + 1 : tok + n;
                if (!comma) break;
            }
            if (!auth) return other ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_RESPONSE;
            c->qop_auth = true;
        }
        if (param(fields, end, "stale", stale, sizeof(stale)))
            c->stale = !strcasecmp(stale, "true");
        return ESP_OK;
    }
    if (challenge_fields(www, "Basic", &end)) return ESP_OK;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool append_text(char *header, size_t capacity, size_t *used, const char *text)
{
    size_t len = strlen(text);
    if (*used >= capacity || len >= capacity - *used) return false;
    memcpy(header + *used, text, len + 1);
    *used += len;
    return true;
}

static bool append_quoted(char *header, size_t capacity, size_t *used,
                          const char *name, const char *value)
{
    if (!append_text(header, capacity, used, name) ||
        !append_text(header, capacity, used, "=\"")) return false;
    for (; *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c < 32 || c == 127) return false;
        if ((c == '"' || c == '\\') && !append_text(header, capacity, used, "\\")) return false;
        char byte[] = {(char)c, '\0'};
        if (!append_text(header, capacity, used, byte)) return false;
    }
    return append_text(header, capacity, used, "\"");
}

esp_err_t solar_os_rtsp_authorization(const solar_os_rtsp_auth_challenge_t *c,
                                     const char *method, const char *uri,
                                     const char *user, const char *password,
                                     char *header, size_t capacity)
{
    if (!c || !method || !uri || !user || !password || !header) return ESP_ERR_INVALID_ARG;
    if (!capacity) return ESP_ERR_INVALID_SIZE;
    header[0] = '\0';
    if (!c->digest) {
        static const char prefix[] = "Basic ";
        unsigned char raw[SOLAR_OS_RTSP_AUTH_USER_MAX + SOLAR_OS_RTSP_AUTH_PASSWORD_MAX + 2];
        const int n = snprintf((char *)raw, sizeof(raw), "%s:%s", user, password);
        esp_err_t error = ESP_ERR_INVALID_SIZE;
        if (n > 0 && (size_t)n < sizeof(raw) && capacity >= sizeof(prefix)) {
            size_t written;
            if (mbedtls_base64_encode((unsigned char *)header + sizeof(prefix) - 1,
                    capacity - sizeof(prefix) + 1, &written, raw, (size_t)n) == 0) {
                memcpy(header, prefix, sizeof(prefix) - 1);
                error = ESP_OK;
            }
        }
        solar_os_rtsp_auth_wipe(raw, sizeof(raw));
        return error;
    }
    if (!memchr(c->realm, 0, sizeof(c->realm)) || !memchr(c->nonce, 0, sizeof(c->nonce)) ||
        !memchr(c->opaque, 0, sizeof(c->opaque)) || !memchr(c->cnonce, 0, sizeof(c->cnonce))) {
        return ESP_ERR_INVALID_ARG;
    }
    char ha1[33], ha2[33], response[33], material[320];
    int n = snprintf(material, sizeof(material), "%s:%s:%s", user, c->realm, password);
    esp_err_t error = md5_hex(material, sizeof(material), n, ha1);
    solar_os_rtsp_auth_wipe(material, sizeof(material));
    if (error == ESP_OK) {
        n = snprintf(material, sizeof(material), "%s:%s", method, uri);
        error = md5_hex(material, sizeof(material), n, ha2);
    }
    if (error == ESP_OK) {
        if (c->qop_auth) {
            n = snprintf(material, sizeof(material), "%s:%s:%08lx:%s:auth:%s",
                ha1, c->nonce, (unsigned long)(c->nc ? c->nc : 1), c->cnonce, ha2);
        } else {
            n = snprintf(material, sizeof(material), "%s:%s:%s", ha1, c->nonce, ha2);
        }
        error = md5_hex(material, sizeof(material), n, response);
    }
    solar_os_rtsp_auth_wipe(material, sizeof(material));
    solar_os_rtsp_auth_wipe(ha1, sizeof(ha1));
    if (error != ESP_OK) return error;
    size_t used = 0;
    bool ok = append_text(header, capacity, &used, "Digest ") &&
        append_quoted(header, capacity, &used, "username", user) &&
        append_text(header, capacity, &used, ", ") &&
        append_quoted(header, capacity, &used, "realm", c->realm) &&
        append_text(header, capacity, &used, ", ") &&
        append_quoted(header, capacity, &used, "nonce", c->nonce) &&
        append_text(header, capacity, &used, ", ") &&
        append_quoted(header, capacity, &used, "uri", uri) &&
        append_text(header, capacity, &used, ", algorithm=MD5, ") &&
        append_quoted(header, capacity, &used, "response", response);
    if (ok && c->qop_auth) {
        char counts[40];
        snprintf(counts, sizeof(counts), ", qop=auth, nc=%08lx, ", (unsigned long)(c->nc ? c->nc : 1));
        ok = append_text(header, capacity, &used, counts) &&
            append_quoted(header, capacity, &used, "cnonce", c->cnonce);
    }
    if (ok && c->opaque[0])
        ok = append_text(header, capacity, &used, ", ") &&
            append_quoted(header, capacity, &used, "opaque", c->opaque);
    solar_os_rtsp_auth_wipe(response, sizeof(response));
    if (!ok) { header[0] = '\0'; return ESP_ERR_INVALID_SIZE; }
    return ESP_OK;
}

static int find_account(const char *host, uint16_t port)
{
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; i++) {
        if (auth_records[i].port == port && !strcasecmp(auth_records[i].host, host)) return (int)i;
    }
    return -1;
}

#ifdef ESP_PLATFORM
static bool valid_accounts(void)
{
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; ++i) {
        const rtsp_auth_record_t *record = &auth_records[i];
        if (!record->host[0]) continue;
        if (!record->port || !record->user[0] ||
            !memchr(record->host, 0, sizeof(record->host)) ||
            !memchr(record->user, 0, sizeof(record->user)) ||
            !memchr(record->password, 0, sizeof(record->password))) return false;
    }
    return true;
}

static esp_err_t load_accounts(void)
{
    if (auth_loaded) return ESP_OK;
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(RTSP_AUTH_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        size_t length = sizeof(auth_records);
        error = nvs_get_blob(nvs, RTSP_AUTH_NVS_KEY, auth_records, &length);
        nvs_close(nvs);
        if (error == ESP_OK && (length != sizeof(auth_records) || !valid_accounts())) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (error != ESP_OK) {
        solar_os_rtsp_auth_wipe(auth_records, sizeof(auth_records));
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    }
    auth_loaded = error == ESP_OK;
    return error;
}

static esp_err_t account_store_open(bool load)
{
    if (!solar_os_lifecycle_enter(&account_store)) return ESP_ERR_NO_MEM;
    esp_err_t error = load ? load_accounts() : ESP_OK;
    if (error != ESP_OK) solar_os_lifecycle_leave(&account_store);
    return error;
}

static void account_store_close(void)
{
    solar_os_lifecycle_leave(&account_store);
}

static esp_err_t save_accounts(const rtsp_auth_record_t *updated)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(RTSP_AUTH_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(nvs, RTSP_AUTH_NVS_KEY, updated, sizeof(auth_records));
    if (error == ESP_OK) error = nvs_commit(nvs);
    nvs_close(nvs);
    if (error == ESP_OK) {
        memcpy(auth_records, updated, sizeof(auth_records));
        auth_loaded = true;
    }
    return error;
}
#else
static esp_err_t account_store_open(bool load) { (void)load; return ESP_OK; }
static void account_store_close(void) {}
static esp_err_t save_accounts(const rtsp_auth_record_t *updated)
{
    memcpy(auth_records, updated, sizeof(auth_records));
    return ESP_OK;
}
#endif

/* A null host clears the store, even when a damaged blob prevents reads. */
static esp_err_t edit_accounts(const char *host, uint16_t port, const rtsp_auth_record_t *record)
{
    esp_err_t error = account_store_open(host != NULL);
    if (error != ESP_OK) return error;
    rtsp_auth_record_t updated[SOLAR_OS_RTSP_AUTH_CAPACITY];
    if (!host) {
        memset(updated, 0, sizeof(updated));
    } else {
        int slot = find_account(host, port);
        if (slot < 0 && record) {
            for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; ++i) {
                if (!auth_records[i].host[0]) { slot = (int)i; break; }
            }
        }
        if (slot < 0) {
            account_store_close();
            return record ? ESP_ERR_NO_MEM : ESP_ERR_NOT_FOUND;
        }
        memcpy(updated, auth_records, sizeof(updated));
        if (record) updated[slot] = *record;
        else memset(&updated[slot], 0, sizeof(updated[slot]));
    }
    error = save_accounts(updated);
    solar_os_rtsp_auth_wipe(updated, sizeof(updated));
    account_store_close();
    return error;
}

esp_err_t solar_os_rtsp_auth_set(const char *host, uint16_t port,
                                const char *user, const char *password)
{
    if (!host || !*host || !user || !*user || !password || !port) return ESP_ERR_INVALID_ARG;
    const size_t host_length = strlen(host);
    const size_t user_length = strlen(user);
    const size_t password_length = strlen(password);
    if (host_length > SOLAR_OS_RTSP_AUTH_HOST_MAX || user_length > SOLAR_OS_RTSP_AUTH_USER_MAX ||
        password_length > SOLAR_OS_RTSP_AUTH_PASSWORD_MAX) return ESP_ERR_INVALID_SIZE;
    rtsp_auth_record_t record = {.port = port};
    memcpy(record.host, host, host_length + 1);
    memcpy(record.user, user, user_length + 1);
    memcpy(record.password, password, password_length + 1);
    const esp_err_t error = edit_accounts(host, port, &record);
    solar_os_rtsp_auth_wipe(&record, sizeof(record));
    return error;
}

esp_err_t solar_os_rtsp_auth_clear(const char *host, uint16_t port)
{
    return edit_accounts(host && *host ? host : NULL, port ? port : 554, NULL);
}

esp_err_t solar_os_rtsp_auth_lookup(const char *host, uint16_t port,
                                   char *user, size_t user_cap,
                                   char *password, size_t password_cap)
{
    if (!host || !*host || !port || !user || !password) return ESP_ERR_INVALID_ARG;
    if (user_cap) user[0] = 0;
    if (password_cap) password[0] = 0;
    esp_err_t error = account_store_open(true);
    if (error != ESP_OK) return error;
    const int slot = find_account(host, port);
    if (slot < 0) {
        error = ESP_ERR_NOT_FOUND;
    } else {
        const rtsp_auth_record_t *record = &auth_records[slot];
        const size_t user_bytes = strlen(record->user) + 1;
        const size_t password_bytes = strlen(record->password) + 1;
        if (user_bytes <= user_cap && password_bytes <= password_cap) {
            memcpy(user, record->user, user_bytes);
            memcpy(password, record->password, password_bytes);
        } else {
            error = ESP_ERR_INVALID_SIZE;
        }
    }
    account_store_close();
    return error;
}

esp_err_t solar_os_rtsp_auth_list(solar_os_rtsp_auth_account_t *records, size_t capacity, size_t *count)
{
    if (!count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    esp_err_t error = account_store_open(true);
    if (error != ESP_OK) return error;
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; ++i) {
        if (!auth_records[i].host[0]) continue;
        if (records) {
            if (*count == capacity) break;
            solar_os_rtsp_auth_account_t *record = &records[*count];
            memcpy(record->host, auth_records[i].host, sizeof(record->host));
            record->port = auth_records[i].port;
            memcpy(record->user, auth_records[i].user, sizeof(record->user));
        }
        ++(*count);
    }
    account_store_close();
    return ESP_OK;
}
