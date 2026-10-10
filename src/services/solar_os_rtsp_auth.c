#include "solar_os_rtsp_auth.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
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
static bool auth_loaded;

void solar_os_rtsp_auth_wipe(void *buffer, size_t length)
{
    volatile uint8_t *bytes = buffer;
    if (!bytes) return;
    while (length--) *bytes++ = 0;
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

static const char *strcasestr_local(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return hay;
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return hay;
    return NULL;
}

static uint32_t md5_rot(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

static void md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    static const uint32_t s[] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    static const uint32_t k[] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    uint64_t bits = (uint64_t)len * 8;
    size_t padded = len + 1;
    while ((padded % 64) != 56) padded++;
    padded += 8;
    for (size_t off = 0; off < padded; off += 64) {
        uint8_t block[64];
        memset(block, 0, sizeof(block));
        if (off < len) memcpy(block, data + off, len - off < 64 ? len - off : 64);
        if (off <= len && len < off + 64) block[len - off] = 0x80;
        if (off + 64 == padded) {
            for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bits >> (8 * i));
        }
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
                   ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = (uint32_t)i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5U * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3U * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7U * i) % 16; }
            uint32_t next = d;
            d = c; c = b;
            b = b + md5_rot(a + f + k[i] + w[g], s[i]);
            a = next;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    uint32_t words[] = {h0, h1, h2, h3};
    for (int i = 0; i < 4; i++) {
        out[i * 4] = (uint8_t)words[i];
        out[i * 4 + 1] = (uint8_t)(words[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(words[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(words[i] >> 24);
    }
}

static void md5_hex(const char *text, char out[33])
{
    uint8_t dig[16];
    md5((const uint8_t *)text, strlen(text), dig);
    hex_encode(dig, 16, out);
}

static size_t base64(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((len + 2) / 3) + 1;
    if (need > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        if (i + 1 < len) n |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) n |= in[i + 2];
        out[o++] = table[(n >> 18) & 63];
        out[o++] = table[(n >> 12) & 63];
        out[o++] = i + 1 < len ? table[(n >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? table[n & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static bool copy_token(char *dst, size_t cap, const char *src, size_t len)
{
    if (!dst || len >= cap) return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static const char *param(const char *header, const char *name, char *out, size_t cap, bool *quoted)
{
    size_t nlen = strlen(name);
    const char *p = header;
    if (quoted) *quoted = false;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!strncasecmp(p, name, nlen) && (p[nlen] == '=' || p[nlen] == ' ')) {
            p += nlen;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '=') return NULL;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                if (quoted) *quoted = true;
                p++;
                const char *end = p;
                while (*end && *end != '"') {
                    if (*end == '\\' && end[1]) end++;
                    end++;
                }
                size_t o = 0;
                for (const char *s = p; s < end && o + 1 < cap; s++) {
                    if (*s == '\\' && s + 1 < end) s++;
                    out[o++] = *s;
                }
                if (o + 1 >= cap) return NULL;
                out[o] = '\0';
                return out;
            }
            const char *end = p;
            while (*end && *end != ',' && *end != ' ' && *end != '\t') end++;
            return copy_token(out, cap, p, (size_t)(end - p)) ? out : NULL;
        }
        p = strchr(p, ',');
        if (p) p++;
    }
    return NULL;
}

esp_err_t solar_os_rtsp_auth_challenge(const char *www, solar_os_rtsp_auth_challenge_t *c)
{
    if (!www || !c || !*www) return ESP_ERR_INVALID_ARG;
    memset(c, 0, sizeof(*c));
    const char *digest = strcasestr_local(www, "Digest");
    const char *basic = strcasestr_local(www, "Basic");
    if (digest) {
        char qop[64] = "", algorithm[32] = "", stale[8] = "";
        const char *fields = digest + 6;
        c->digest = true;
        if (!param(fields, "realm", c->realm, sizeof(c->realm), NULL) ||
            !param(fields, "nonce", c->nonce, sizeof(c->nonce), NULL))
            return ESP_ERR_INVALID_RESPONSE;
        (void)param(fields, "opaque", c->opaque, sizeof(c->opaque), NULL);
        if (param(fields, "algorithm", algorithm, sizeof(algorithm), NULL) &&
            algorithm[0] && strcasecmp(algorithm, "MD5"))
            return ESP_ERR_NOT_SUPPORTED;
        if (param(fields, "qop", qop, sizeof(qop), NULL) && qop[0]) {
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
        if (param(fields, "stale", stale, sizeof(stale), NULL))
            c->stale = !strcasecmp(stale, "true");
        return ESP_OK;
    }
    if (basic) return ESP_OK;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_rtsp_authorization(const solar_os_rtsp_auth_challenge_t *c,
                                     const char *method, const char *uri,
                                     const char *user, const char *password,
                                     char *header, size_t capacity)
{
    if (!c || !method || !uri || !user || !password || !header) return ESP_ERR_INVALID_ARG;
    if (strchr(user, '"') || strchr(uri, '"')) return ESP_ERR_INVALID_ARG;
    if (!c->digest) {
        char raw[SOLAR_OS_RTSP_AUTH_USER_MAX + SOLAR_OS_RTSP_AUTH_PASSWORD_MAX + 2];
        char encoded[160];
        int n = snprintf(raw, sizeof(raw), "%s:%s", user, password);
        if (n < 0 || (size_t)n >= sizeof(raw)) return ESP_ERR_INVALID_SIZE;
        if (!base64((const uint8_t *)raw, (size_t)n, encoded, sizeof(encoded)))
            return ESP_ERR_INVALID_SIZE;
        solar_os_rtsp_auth_wipe(raw, sizeof(raw));
        n = snprintf(header, capacity, "Basic %s", encoded);
        solar_os_rtsp_auth_wipe(encoded, sizeof(encoded));
        return n > 0 && (size_t)n < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    char ha1[33], ha2[33], response[33], material[320];
    int n = snprintf(material, sizeof(material), "%s:%s:%s", user, c->realm, password);
    if (n < 0 || (size_t)n >= sizeof(material)) return ESP_ERR_INVALID_SIZE;
    md5_hex(material, ha1);
    solar_os_rtsp_auth_wipe(material, sizeof(material));
    n = snprintf(material, sizeof(material), "%s:%s", method, uri);
    if (n < 0 || (size_t)n >= sizeof(material)) return ESP_ERR_INVALID_SIZE;
    md5_hex(material, ha2);
    if (c->qop_auth) {
        char nc[9];
        snprintf(nc, sizeof(nc), "%08lx", (unsigned long)(c->nc ? c->nc : 1));
        n = snprintf(material, sizeof(material), "%s:%s:%s:%s:auth:%s",
                     ha1, c->nonce, nc, c->cnonce, ha2);
    } else {
        n = snprintf(material, sizeof(material), "%s:%s:%s", ha1, c->nonce, ha2);
    }
    if (n < 0 || (size_t)n >= sizeof(material)) return ESP_ERR_INVALID_SIZE;
    md5_hex(material, response);
    solar_os_rtsp_auth_wipe(material, sizeof(material));
    solar_os_rtsp_auth_wipe(ha1, sizeof(ha1));
    if (c->qop_auth) {
        n = snprintf(header, capacity,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "algorithm=MD5, response=\"%s\", qop=auth, nc=%08lx, cnonce=\"%s\"%s%s%s",
            user, c->realm, c->nonce, uri, response, (unsigned long)(c->nc ? c->nc : 1),
            c->cnonce, c->opaque[0] ? ", opaque=\"" : "", c->opaque, c->opaque[0] ? "\"" : "");
    } else {
        n = snprintf(header, capacity,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "algorithm=MD5, response=\"%s\"%s%s%s",
            user, c->realm, c->nonce, uri, response,
            c->opaque[0] ? ", opaque=\"" : "", c->opaque, c->opaque[0] ? "\"" : "");
    }
    solar_os_rtsp_auth_wipe(response, sizeof(response));
    return n > 0 && (size_t)n < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool host_equal(const char *a, const char *b)
{
    return a && b && !strcasecmp(a, b);
}

static int find_account(const char *host, uint16_t port)
{
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; i++)
        if (auth_records[i].host[0] && host_equal(auth_records[i].host, host) &&
            auth_records[i].port == port)
            return (int)i;
    return -1;
}

#ifdef ESP_PLATFORM
static esp_err_t persist(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(RTSP_AUTH_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, RTSP_AUTH_NVS_KEY, auth_records, sizeof(auth_records));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void load(void)
{
    if (auth_loaded) return;
    auth_loaded = true;
    nvs_handle_t nvs;
    if (nvs_open(RTSP_AUTH_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(auth_records);
    if (nvs_get_blob(nvs, RTSP_AUTH_NVS_KEY, auth_records, &len) != ESP_OK || len != sizeof(auth_records))
        memset(auth_records, 0, sizeof(auth_records));
    nvs_close(nvs);
}
#else
static esp_err_t persist(void) { auth_loaded = true; return ESP_OK; }
static void load(void) { auth_loaded = true; }
#endif

esp_err_t solar_os_rtsp_auth_set(const char *host, uint16_t port,
                                const char *user, const char *password)
{
    if (!host || !*host || !user || !*user || !password || !port) return ESP_ERR_INVALID_ARG;
    if (strlen(host) > SOLAR_OS_RTSP_AUTH_HOST_MAX ||
        strlen(user) > SOLAR_OS_RTSP_AUTH_USER_MAX ||
        strlen(password) > SOLAR_OS_RTSP_AUTH_PASSWORD_MAX)
        return ESP_ERR_INVALID_SIZE;
    load();
    int slot = find_account(host, port);
    if (slot < 0) {
        for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; i++)
            if (!auth_records[i].host[0]) { slot = (int)i; break; }
    }
    if (slot < 0) return ESP_ERR_NO_MEM;
    rtsp_auth_record_t *r = &auth_records[slot];
    memset(r, 0, sizeof(*r));
    snprintf(r->host, sizeof(r->host), "%s", host);
    r->port = port;
    snprintf(r->user, sizeof(r->user), "%s", user);
    snprintf(r->password, sizeof(r->password), "%s", password);
    return persist();
}

esp_err_t solar_os_rtsp_auth_clear(const char *host, uint16_t port)
{
    load();
    if (!host || !*host) {
        for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY; i++)
            solar_os_rtsp_auth_wipe(&auth_records[i], sizeof(auth_records[i]));
        return persist();
    }
    int slot = find_account(host, port ? port : 554);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    solar_os_rtsp_auth_wipe(&auth_records[slot], sizeof(auth_records[slot]));
    return persist();
}

esp_err_t solar_os_rtsp_auth_lookup(const char *host, uint16_t port,
                                   char *user, size_t user_cap,
                                   char *password, size_t password_cap)
{
    if (!host || !port || !user || !password) return ESP_ERR_INVALID_ARG;
    load();
    int slot = find_account(host, port);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    if (strlen(auth_records[slot].user) >= user_cap ||
        strlen(auth_records[slot].password) >= password_cap)
        return ESP_ERR_INVALID_SIZE;
    snprintf(user, user_cap, "%s", auth_records[slot].user);
    snprintf(password, password_cap, "%s", auth_records[slot].password);
    return ESP_OK;
}

size_t solar_os_rtsp_auth_list(solar_os_rtsp_auth_account_t *records, size_t max_records)
{
    load();
    size_t n = 0;
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUTH_CAPACITY && n < max_records; i++) {
        if (!auth_records[i].host[0]) continue;
        if (records) {
            memcpy(records[n].host, auth_records[i].host, sizeof(records[n].host));
            records[n].port = auth_records[i].port;
            memcpy(records[n].user, auth_records[i].user, sizeof(records[n].user));
        }
        n++;
    }
    return n;
}
