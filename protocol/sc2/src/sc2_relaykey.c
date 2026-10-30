/* SC2 portable core: relay descriptor, welcome proof and key rotation
 * (see sc2_relaykey.h). SPDX-License-Identifier: MIT */
#include "sc2_relaykey.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"

static const uint8_t DESC_LABEL[9] = {'S', 'C', '2', '-', 'D', 'E', 'S', 'C', 0x01};
static const uint8_t WELC_LABEL[9] = {'S', 'C', '2', '-', 'W', 'E', 'L', 'C', 0x01};
static const uint8_t ROTK_LABEL[9] = {'S', 'C', '2', '-', 'R', 'O', 'T', 'K', 0x01};

/* ---------------- signatures ---------------- */

static int id_part(const char *relay_id, uint8_t *len_byte, sc2_part_t *part)
{
    size_t n = relay_id != NULL ? strlen(relay_id) : 0;
    if (n == 0 || n > SC2_RELAY_ID_MAX) {
        return SC2_ERR_ARG;
    }
    *len_byte = (uint8_t)n;
    part->ptr = (const uint8_t *)relay_id;
    part->len = n;
    return SC2_OK;
}

int sc2_desc_verify(const uint8_t sig[SC2_SIG_LEN], const char *text, size_t len, const uint8_t key[SC2_KEY_LEN])
{
    if (sig == NULL || text == NULL || key == NULL || len == 0 || len > SC2_DESC_TEXT_MAX) {
        return SC2_ERR_ARG;
    }
    const sc2_part_t parts[2] = {{DESC_LABEL, sizeof(DESC_LABEL)}, {(const uint8_t *)text, len}};
    return sc2_ed25519_verify_parts(sig, parts, 2, key);
}

int sc2_desc_sign(uint8_t sig[SC2_SIG_LEN], const char *text, size_t len, const uint8_t seed[SC2_KEY_LEN])
{
    if (sig == NULL || text == NULL || seed == NULL || len == 0 || len > SC2_DESC_TEXT_MAX) {
        return SC2_ERR_ARG;
    }
    const sc2_part_t parts[2] = {{DESC_LABEL, sizeof(DESC_LABEL)}, {(const uint8_t *)text, len}};
    return sc2_ed25519_sign_parts(sig, parts, 2, seed);
}

static int welcome_parts(sc2_part_t parts[5], uint8_t *len_byte, const char *relay_id, const uint8_t *nonce,
                         const uint8_t *challenge)
{
    if (nonce == NULL || challenge == NULL) {
        return SC2_ERR_ARG;
    }
    parts[0] = (sc2_part_t){WELC_LABEL, sizeof(WELC_LABEL)};
    parts[1] = (sc2_part_t){len_byte, 1};
    parts[3] = (sc2_part_t){nonce, SC2_HELLO_NONCE_LEN};
    parts[4] = (sc2_part_t){challenge, SC2_AUTH_CHALLENGE_LEN};
    return id_part(relay_id, len_byte, &parts[2]);
}

int sc2_welcome_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t key[SC2_KEY_LEN], const char *relay_id,
                       const uint8_t nonce[SC2_HELLO_NONCE_LEN], const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN])
{
    sc2_part_t parts[5];
    uint8_t n = 0;
    int rc = sig != NULL && key != NULL ? welcome_parts(parts, &n, relay_id, nonce, challenge) : SC2_ERR_ARG;
    return rc == SC2_OK ? sc2_ed25519_verify_parts(sig, parts, 5, key) : rc;
}

int sc2_welcome_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t seed[SC2_KEY_LEN], const char *relay_id,
                     const uint8_t nonce[SC2_HELLO_NONCE_LEN], const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN])
{
    sc2_part_t parts[5];
    uint8_t n = 0;
    int rc = sig != NULL && seed != NULL ? welcome_parts(parts, &n, relay_id, nonce, challenge) : SC2_ERR_ARG;
    return rc == SC2_OK ? sc2_ed25519_sign_parts(sig, parts, 5, seed) : rc;
}

int sc2_rotation_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t old_key[SC2_KEY_LEN], const char *relay_id,
                        const uint8_t new_key[SC2_KEY_LEN])
{
    if (sig == NULL || old_key == NULL || new_key == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_part_t parts[5];
    uint8_t n = 0;
    int rc = id_part(relay_id, &n, &parts[2]);
    if (rc != SC2_OK) {
        return rc;
    }
    parts[0] = (sc2_part_t){ROTK_LABEL, sizeof(ROTK_LABEL)};
    parts[1] = (sc2_part_t){&n, 1};
    parts[3] = (sc2_part_t){old_key, SC2_KEY_LEN};
    parts[4] = (sc2_part_t){new_key, SC2_KEY_LEN};
    return sc2_ed25519_verify_parts(sig, parts, 5, old_key);
}

int sc2_rotation_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t old_seed[SC2_KEY_LEN], const char *relay_id,
                      const uint8_t new_key[SC2_KEY_LEN])
{
    uint8_t old_key[SC2_KEY_LEN];
    if (sig == NULL || old_seed == NULL || new_key == NULL || sc2_ed25519_public(old_key, old_seed) != SC2_OK) {
        return SC2_ERR_ARG;
    }
    sc2_part_t parts[5];
    uint8_t n = 0;
    int rc = id_part(relay_id, &n, &parts[2]);
    if (rc != SC2_OK) {
        return rc;
    }
    parts[0] = (sc2_part_t){ROTK_LABEL, sizeof(ROTK_LABEL)};
    parts[1] = (sc2_part_t){&n, 1};
    parts[3] = (sc2_part_t){old_key, SC2_KEY_LEN};
    parts[4] = (sc2_part_t){new_key, SC2_KEY_LEN};
    return sc2_ed25519_sign_parts(sig, parts, 5, old_seed);
}

/* ---------------- descriptor text ---------------- */

static bool word_eq(const char *p, size_t n, const char *w)
{
    return strlen(w) == n && memcmp(p, w, n) == 0;
}

/* Printable ASCII without spaces (URLs, host names, pins, base64). */
static bool token_ok(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)p[i] <= 0x20 || (unsigned char)p[i] >= 0x7f) {
            return false;
        }
    }
    return n > 0;
}

static bool copy_token(char *dst, size_t cap, const char *p, size_t n)
{
    if (!token_ok(p, n) || n >= cap) {
        return false;
    }
    memcpy(dst, p, n);
    dst[n] = '\0';
    return true;
}

static bool parse_u64(const char *p, size_t n, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    if (n == 0 || n > 20 || (n > 1 && p[0] == '0')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9' || v > (max - (uint64_t)(p[i] - '0')) / 10u) {
            return false;
        }
        v = v * 10u + (uint64_t)(p[i] - '0');
    }
    *out = v;
    return true;
}

static bool parse_key(const char *p, size_t n, uint8_t out[SC2_KEY_LEN])
{
    size_t got = 0;
    return n == 44 && sc2_b64_decode(out, SC2_KEY_LEN, &got, p, n) == SC2_OK && got == SC2_KEY_LEN;
}

static bool host_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || h[0] == '-' || h[0] == '.' || h[n - 1] == '.' || h[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

int sc2_desc_parse(const char *text, size_t len, sc2_desc_t *out)
{
    if (text == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (len == 0 || len > SC2_DESC_TEXT_MAX) {
        return SC2_ERR_FORMAT;
    }
    bool have_id = false, have_key = false, have_port = false, have_tls = false;
    size_t line_no = 0;
    for (size_t pos = 0; pos < len; line_no++) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', len - pos);
        size_t n = nl != NULL ? (size_t)(nl - line) : len - pos;
        pos += n + (nl != NULL ? 1u : 0u);
        if (n == 0) {
            continue;
        }
        const char *sp = memchr(line, ' ', n);
        if (sp == NULL) {
            return SC2_ERR_FORMAT;
        }
        size_t kn = (size_t)(sp - line);
        const char *v = sp + 1;
        size_t vn = n - kn - 1u;
        uint64_t u = 0;
        if (line_no == 0) {
            if (!word_eq(line, kn, "sc2-relay") || !word_eq(v, vn, "1")) {
                return SC2_ERR_FORMAT;
            }
        } else if (word_eq(line, kn, "id")) {
            if (have_id || !copy_token(out->relay_id, sizeof(out->relay_id), v, vn)) {
                return SC2_ERR_FORMAT;
            }
            have_id = true;
        } else if (word_eq(line, kn, "key")) {
            if (have_key || !parse_key(v, vn, out->key)) {
                return SC2_ERR_FORMAT;
            }
            have_key = true;
        } else if (word_eq(line, kn, "host")) {
            if (out->host[0] != '\0' || !copy_token(out->host, sizeof(out->host), v, vn) || !host_ok(out->host)) {
                return SC2_ERR_FORMAT;
            }
        } else if (word_eq(line, kn, "port")) {
            if (have_port || !parse_u64(v, vn, 65535u, &u) || u == 0) {
                return SC2_ERR_FORMAT;
            }
            out->port = (uint16_t)u;
            have_port = true;
        } else if (word_eq(line, kn, "tls")) {
            if (have_tls || !(word_eq(v, vn, "0") || word_eq(v, vn, "1"))) {
                return SC2_ERR_FORMAT;
            }
            out->tls = v[0] == '1';
            have_tls = true;
        } else if (word_eq(line, kn, "pin")) {
            if (out->pin[0] != '\0' || vn < 8 || memcmp(v, "sha256/", 7) != 0 ||
                !copy_token(out->pin, sizeof(out->pin), v, vn)) {
                return SC2_ERR_FORMAT;
            }
        } else if (word_eq(line, kn, "fallback")) {
            const char *sp2 = memchr(v, ' ', vn);
            if (out->fallback_count >= SC2_DESC_FALLBACK_MAX || sp2 == NULL) {
                return SC2_ERR_FORMAT;
            }
            sc2_desc_fallback_t *f = &out->fallback[out->fallback_count];
            size_t un = (size_t)(sp2 - v);
            if (!copy_token(f->url, sizeof(f->url), v, un) || !parse_key(sp2 + 1, vn - un - 1u, f->key)) {
                return SC2_ERR_FORMAT;
            }
            out->fallback_count++;
        } else if (word_eq(line, kn, "ts")) {
            if (!parse_u64(v, vn, UINT64_MAX, &out->ts)) {
                return SC2_ERR_FORMAT;
            }
        } else if (!token_ok(line, kn)) {
            return SC2_ERR_FORMAT; /* unknown names are skipped, but must look like names */
        }
    }
    return line_no > 0 && have_id && have_key && have_port && have_tls ? SC2_OK : SC2_ERR_FORMAT;
}

/* Find "name" <ws> : <ws> "<value>" in a JSON prefix; the value must not
 * contain escapes (base64 never does). */
static int find_string(const char *body, size_t len, const char *name, const char **val, size_t *val_len)
{
    size_t nn = strlen(name);
    for (size_t i = 0; i + nn + 2 < len; i++) {
        if (body[i] != '"' || memcmp(body + i + 1, name, nn) != 0 || body[i + 1 + nn] != '"' ||
            (i > 0 && body[i - 1] == '\\')) {
            continue;
        }
        size_t p = i + nn + 2;
        while (p < len && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) {
            p++;
        }
        if (p >= len || body[p] != ':') {
            continue;
        }
        p++;
        while (p < len && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) {
            p++;
        }
        if (p >= len || body[p] != '"') {
            return SC2_ERR_FORMAT;
        }
        const char *start = body + p + 1;
        const char *end = memchr(start, '"', len - p - 1);
        if (end == NULL) {
            return SC2_ERR_FORMAT; /* cut short inside the value */
        }
        if (memchr(start, '\\', (size_t)(end - start)) != NULL) {
            return SC2_ERR_FORMAT;
        }
        *val = start;
        *val_len = (size_t)(end - start);
        return SC2_OK;
    }
    return SC2_ERR_NOT_FOUND;
}

int sc2_wellknown_parse(char *body, size_t len, sc2_desc_t *out)
{
    if (body == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    const char *d = NULL, *s = NULL;
    size_t dn = 0, sn = 0, tn = 0, got = 0;
    int rc = find_string(body, len, "descriptor", &d, &dn);
    if (rc == SC2_OK) {
        rc = find_string(body, len, "descriptor_sig", &s, &sn);
        if (rc == SC2_ERR_NOT_FOUND) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc != SC2_OK) {
        return rc;
    }
    uint8_t sig[SC2_SIG_LEN];
    if (sn != 88 || dn > (SC2_DESC_TEXT_MAX / 3u + 1u) * 4u ||
        sc2_b64_decode(sig, sizeof(sig), &got, s, sn) != SC2_OK || got != SC2_SIG_LEN) {
        return SC2_ERR_FORMAT;
    }
    /* in place: the text lands at body[0], before the base64 it comes from */
    uint8_t *text = (uint8_t *)body;
    if (sc2_b64_decode(text, SC2_DESC_TEXT_MAX, &tn, d, dn) != SC2_OK) {
        return SC2_ERR_FORMAT;
    }
    rc = sc2_desc_parse((const char *)text, tn, out);
    if (rc == SC2_OK) {
        rc = sc2_desc_verify(sig, (const char *)text, tn, out->key);
        if (rc == SC2_ERR_KEY) {
            rc = SC2_ERR_AUTH;
        }
    }
    if (rc != SC2_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}
