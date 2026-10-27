/*
 * SC2 portable core: bounded JSON reader and writer.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_json.h"

#include <string.h>

#include "sc2_codec.h"

bool sc2_utf8_valid(const char *s, size_t len)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    if (s == NULL && len != 0) {
        return false;
    }
    while (i < len) {
        uint8_t c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t n;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) {
            n = 1;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 2;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (len - i <= n) {
            return false;
        }
        for (size_t k = 1; k <= n; k++) {
            uint8_t cc = p[i + k];
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((n == 2 && cp < 0x800u) || (n == 3 && (cp < 0x10000u || cp > 0x10FFFFu)) ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += n + 1u;
    }
    return true;
}

/* ---------------- reader ---------------- */

typedef struct {
    const char *p;
    const char *end;
} cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        int d;
        if (ch >= '0' && ch <= '9') {
            d = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            d = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            d = ch - 'A' + 10;
        } else {
            return -1;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 0;
}

/* Decode the next character of a JSON string body. On entry *p points
 * inside the string (not at the closing quote). Writes up to 4 UTF-8
 * bytes. Returns the number of bytes or -1 on a syntax error. */
static int next_char(const char **pp, const char *end, uint8_t out[4])
{
    const char *p = *pp;
    uint8_t c = (uint8_t)*p;
    if (c < 0x20) {
        return -1;
    }
    if (c != '\\') {
        /* Raw UTF-8 sequence: copy it whole (validated separately). */
        size_t n = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
        if ((size_t)(end - p) < n) {
            return -1;
        }
        memcpy(out, p, n);
        *pp = p + n;
        return (int)n;
    }
    if (end - p < 2) {
        return -1;
    }
    char e = p[1];
    p += 2;
    switch (e) {
    case '"': out[0] = '"'; break;
    case '\\': out[0] = '\\'; break;
    case '/': out[0] = '/'; break;
    case 'b': out[0] = '\b'; break;
    case 'f': out[0] = '\f'; break;
    case 'n': out[0] = '\n'; break;
    case 'r': out[0] = '\r'; break;
    case 't': out[0] = '\t'; break;
    case 'u': {
        uint32_t cp;
        if (end - p < 4 || hex4(p, &cp) != 0) {
            return -1;
        }
        p += 4;
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            uint32_t lo;
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u' || hex4(p + 2, &lo) != 0 || lo < 0xDC00u ||
                lo > 0xDFFFu) {
                return -1;
            }
            p += 6;
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            return -1;
        }
        *pp = p;
        if (cp < 0x80u) {
            out[0] = (uint8_t)cp;
            return 1;
        }
        if (cp < 0x800u) {
            out[0] = (uint8_t)(0xC0u | (cp >> 6));
            out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
            return 2;
        }
        if (cp < 0x10000u) {
            out[0] = (uint8_t)(0xE0u | (cp >> 12));
            out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
            out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
            return 3;
        }
        out[0] = (uint8_t)(0xF0u | (cp >> 18));
        out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    default:
        return -1;
    }
    *pp = p;
    return 1;
}

static int parse_string(cur_t *c)
{
    if (c->p >= c->end || *c->p != '"') {
        return SC2_ERR_FORMAT;
    }
    const char *p = c->p + 1;
    const char *raw_start = p;
    while (p < c->end && *p != '"') {
        if (*p == '\\') {
            /* raw run before the escape must be valid UTF-8 */
            if (!sc2_utf8_valid(raw_start, (size_t)(p - raw_start))) {
                return SC2_ERR_FORMAT;
            }
            uint8_t tmp[4];
            if (next_char(&p, c->end, tmp) < 0) {
                return SC2_ERR_FORMAT;
            }
            raw_start = p;
            continue;
        }
        if ((uint8_t)*p < 0x20) {
            return SC2_ERR_FORMAT;
        }
        p++;
    }
    if (p >= c->end) {
        return SC2_ERR_FORMAT;
    }
    if (!sc2_utf8_valid(raw_start, (size_t)(p - raw_start))) {
        return SC2_ERR_FORMAT;
    }
    c->p = p + 1;
    return SC2_OK;
}

static int is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static int parse_number(cur_t *c)
{
    const char *p = c->p;
    if (p < c->end && *p == '-') {
        p++;
    }
    if (p >= c->end) {
        return SC2_ERR_FORMAT;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < c->end && is_digit(*p)) {
            p++;
        }
    } else {
        return SC2_ERR_FORMAT;
    }
    if (p < c->end && *p == '.') {
        p++;
        if (p >= c->end || !is_digit(*p)) {
            return SC2_ERR_FORMAT;
        }
        while (p < c->end && is_digit(*p)) {
            p++;
        }
    }
    if (p < c->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < c->end && (*p == '+' || *p == '-')) {
            p++;
        }
        if (p >= c->end || !is_digit(*p)) {
            return SC2_ERR_FORMAT;
        }
        while (p < c->end && is_digit(*p)) {
            p++;
        }
    }
    c->p = p;
    return SC2_OK;
}

static int parse_literal(cur_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        return SC2_ERR_FORMAT;
    }
    c->p += n;
    return SC2_OK;
}

static int parse_value(cur_t *c, int depth, sc2_json_val_t *out);

static int parse_object(cur_t *c, int depth)
{
    if (depth >= SC2_JSON_MAX_DEPTH) {
        return SC2_ERR_FORMAT;
    }
    c->p++; /* '{' */
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return SC2_OK;
    }
    for (;;) {
        skip_ws(c);
        int rc = parse_string(c);
        if (rc != SC2_OK) {
            return rc;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            return SC2_ERR_FORMAT;
        }
        c->p++;
        skip_ws(c);
        rc = parse_value(c, depth + 1, NULL);
        if (rc != SC2_OK) {
            return rc;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return SC2_OK;
        }
        return SC2_ERR_FORMAT;
    }
}

static int parse_array(cur_t *c, int depth)
{
    if (depth >= SC2_JSON_MAX_DEPTH) {
        return SC2_ERR_FORMAT;
    }
    c->p++; /* '[' */
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return SC2_OK;
    }
    for (;;) {
        skip_ws(c);
        int rc = parse_value(c, depth + 1, NULL);
        if (rc != SC2_OK) {
            return rc;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return SC2_OK;
        }
        return SC2_ERR_FORMAT;
    }
}

static int parse_value(cur_t *c, int depth, sc2_json_val_t *out)
{
    if (c->p >= c->end) {
        return SC2_ERR_FORMAT;
    }
    const char *start = c->p;
    sc2_json_type_t t;
    int rc;
    switch (*c->p) {
    case '{':
        t = SC2_JSON_OBJECT;
        rc = parse_object(c, depth);
        break;
    case '[':
        t = SC2_JSON_ARRAY;
        rc = parse_array(c, depth);
        break;
    case '"':
        t = SC2_JSON_STRING;
        rc = parse_string(c);
        break;
    case 't':
        t = SC2_JSON_TRUE;
        rc = parse_literal(c, "true");
        break;
    case 'f':
        t = SC2_JSON_FALSE;
        rc = parse_literal(c, "false");
        break;
    case 'n':
        t = SC2_JSON_NULL;
        rc = parse_literal(c, "null");
        break;
    default:
        t = SC2_JSON_NUMBER;
        rc = parse_number(c);
        break;
    }
    if (rc == SC2_OK && out) {
        out->type = t;
        out->ptr = start;
        out->len = (size_t)(c->p - start);
    }
    return rc;
}

int sc2_json_root(const char *text, size_t len, sc2_json_val_t *root)
{
    if (text == NULL || root == NULL) {
        return SC2_ERR_ARG;
    }
    cur_t c = {text, text + len};
    skip_ws(&c);
    int rc = parse_value(&c, 0, root);
    if (rc != SC2_OK) {
        return rc;
    }
    skip_ws(&c);
    if (c.p != c.end) {
        return SC2_ERR_FORMAT;
    }
    return SC2_OK;
}

int sc2_json_validate(const char *text, size_t len)
{
    sc2_json_val_t v;
    return sc2_json_root(text, len, &v);
}

/* Compare a raw string token (with quotes) against a C string. */
static bool raw_string_eq(const char *tok, size_t tok_len, const char *s)
{
    const char *p = tok + 1;
    const char *end = tok + tok_len - 1;
    size_t slen = strlen(s);
    size_t si = 0;
    while (p < end) {
        uint8_t buf[4];
        int n = next_char(&p, end, buf);
        if (n < 0 || si + (size_t)n > slen || memcmp(s + si, buf, (size_t)n) != 0) {
            return false;
        }
        si += (size_t)n;
    }
    return si == slen;
}

int sc2_json_get(const sc2_json_val_t *obj, const char *key, sc2_json_val_t *out)
{
    if (obj == NULL || key == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    if (obj->type != SC2_JSON_OBJECT || obj->len < 2) {
        return SC2_ERR_FORMAT;
    }
    cur_t c = {obj->ptr + 1, obj->ptr + obj->len - 1};
    bool found = false;
    for (;;) {
        skip_ws(&c);
        if (c.p >= c.end) {
            break;
        }
        const char *ks = c.p;
        if (parse_string(&c) != SC2_OK) {
            return SC2_ERR_FORMAT;
        }
        size_t klen = (size_t)(c.p - ks);
        skip_ws(&c);
        if (c.p >= c.end || *c.p != ':') {
            return SC2_ERR_FORMAT;
        }
        c.p++;
        skip_ws(&c);
        sc2_json_val_t v;
        if (parse_value(&c, 1, &v) != SC2_OK) {
            return SC2_ERR_FORMAT;
        }
        if (raw_string_eq(ks, klen, key)) {
            if (found) {
                return SC2_ERR_FORMAT; /* duplicate key */
            }
            found = true;
            *out = v;
        }
        skip_ws(&c);
        if (c.p < c.end && *c.p == ',') {
            c.p++;
        }
    }
    return found ? SC2_OK : SC2_ERR_NOT_FOUND;
}

int sc2_json_array_next(const sc2_json_val_t *arr, size_t *pos, sc2_json_val_t *out)
{
    if (arr == NULL || pos == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    if (arr->type != SC2_JSON_ARRAY || arr->len < 2) {
        return SC2_ERR_FORMAT;
    }
    size_t off = *pos ? *pos : 1u;
    if (off >= arr->len - 1u) {
        return SC2_ERR_NOT_FOUND;
    }
    cur_t c = {arr->ptr + off, arr->ptr + arr->len - 1};
    skip_ws(&c);
    if (c.p < c.end && *c.p == ',') {
        c.p++;
        skip_ws(&c);
    }
    if (c.p >= c.end) {
        return SC2_ERR_NOT_FOUND;
    }
    if (parse_value(&c, 1, out) != SC2_OK) {
        return SC2_ERR_FORMAT;
    }
    *pos = (size_t)(c.p - arr->ptr);
    return SC2_OK;
}

int sc2_json_string(const sc2_json_val_t *val, char *out, size_t cap, size_t *out_len)
{
    if (val == NULL || out == NULL || cap == 0) {
        return SC2_ERR_ARG;
    }
    if (val->type != SC2_JSON_STRING || val->len < 2) {
        return SC2_ERR_FORMAT;
    }
    const char *p = val->ptr + 1;
    const char *end = val->ptr + val->len - 1;
    size_t o = 0;
    while (p < end) {
        uint8_t buf[4];
        int n = next_char(&p, end, buf);
        if (n < 0) {
            out[0] = '\0';
            return SC2_ERR_FORMAT;
        }
        if (o + (size_t)n + 1u > cap) {
            out[0] = '\0';
            return SC2_ERR_BOUNDS;
        }
        memcpy(out + o, buf, (size_t)n);
        o += (size_t)n;
    }
    out[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return SC2_OK;
}

bool sc2_json_string_eq(const sc2_json_val_t *val, const char *s)
{
    if (val == NULL || s == NULL || val->type != SC2_JSON_STRING || val->len < 2) {
        return false;
    }
    return raw_string_eq(val->ptr, val->len, s);
}

static int digits_u64(const char *p, size_t n, uint64_t *out)
{
    if (n == 0 || n > 20 || (n > 1 && p[0] == '0')) {
        return SC2_ERR_FORMAT;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(p[i])) {
            return SC2_ERR_FORMAT;
        }
        uint64_t d = (uint64_t)(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) {
            return SC2_ERR_BOUNDS;
        }
        v = v * 10u + d;
    }
    *out = v;
    return SC2_OK;
}

int sc2_json_u64(const sc2_json_val_t *val, uint64_t max, uint64_t *out)
{
    if (val == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    if (val->type != SC2_JSON_NUMBER) {
        return SC2_ERR_FORMAT;
    }
    uint64_t v;
    int rc = digits_u64(val->ptr, val->len, &v);
    if (rc != SC2_OK) {
        return rc;
    }
    if (v > max) {
        return SC2_ERR_BOUNDS;
    }
    *out = v;
    return SC2_OK;
}

int sc2_json_u64_str(const sc2_json_val_t *val, uint64_t *out)
{
    if (val == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    if (val->type != SC2_JSON_STRING || val->len < 3) {
        return SC2_ERR_FORMAT;
    }
    return digits_u64(val->ptr + 1, val->len - 2, out);
}

int sc2_json_bool(const sc2_json_val_t *val, bool *out)
{
    if (val == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    if (val->type == SC2_JSON_TRUE) {
        *out = true;
        return SC2_OK;
    }
    if (val->type == SC2_JSON_FALSE) {
        *out = false;
        return SC2_OK;
    }
    return SC2_ERR_FORMAT;
}

int sc2_json_b64(const sc2_json_val_t *val, uint8_t *out, size_t cap, size_t *out_len)
{
    if (val == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    if (val->type != SC2_JSON_STRING || val->len < 2) {
        return SC2_ERR_FORMAT;
    }
    /* base64 never needs escapes; reject them rather than unescaping */
    if (memchr(val->ptr + 1, '\\', val->len - 2) != NULL) {
        return SC2_ERR_FORMAT;
    }
    return sc2_b64_decode(out, cap, out_len, val->ptr + 1, val->len - 2);
}

int sc2_json_b64_exact(const sc2_json_val_t *val, uint8_t *out, size_t len)
{
    size_t got = 0;
    if (val == NULL || val->type != SC2_JSON_STRING || val->len != SC2_B64_ENCODED_LEN(len) + 2u) {
        return SC2_ERR_FORMAT;
    }
    int rc = sc2_json_b64(val, out, len, &got);
    if (rc == SC2_OK && got != len) {
        rc = SC2_ERR_FORMAT;
    }
    return rc;
}

int sc2_json_hex_exact(const sc2_json_val_t *val, uint8_t *out, size_t len)
{
    if (val == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    if (val->type != SC2_JSON_STRING || val->len != 2u * len + 2u) {
        return SC2_ERR_FORMAT;
    }
    return sc2_hex_decode(out, len, val->ptr + 1, val->len - 2);
}

int sc2_json_get_string(const sc2_json_val_t *obj, const char *key, char *out, size_t cap, size_t *out_len)
{
    sc2_json_val_t v;
    int rc = sc2_json_get(obj, key, &v);
    return rc == SC2_OK ? sc2_json_string(&v, out, cap, out_len) : rc;
}

int sc2_json_get_u64(const sc2_json_val_t *obj, const char *key, uint64_t max, uint64_t *out)
{
    sc2_json_val_t v;
    int rc = sc2_json_get(obj, key, &v);
    return rc == SC2_OK ? sc2_json_u64(&v, max, out) : rc;
}

int sc2_json_get_b64_exact(const sc2_json_val_t *obj, const char *key, uint8_t *out, size_t len)
{
    sc2_json_val_t v;
    int rc = sc2_json_get(obj, key, &v);
    return rc == SC2_OK ? sc2_json_b64_exact(&v, out, len) : rc;
}

int sc2_json_get_hex_exact(const sc2_json_val_t *obj, const char *key, uint8_t *out, size_t len)
{
    sc2_json_val_t v;
    int rc = sc2_json_get(obj, key, &v);
    return rc == SC2_OK ? sc2_json_hex_exact(&v, out, len) : rc;
}

/* ---------------- writer ---------------- */

static void put(sc2_jw_t *w, const char *s, size_t n)
{
    if (w->err) {
        return;
    }
    if (w->cap - w->len < n + 1u) {
        w->err = SC2_ERR_BOUNDS;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void putc1(sc2_jw_t *w, char ch)
{
    put(w, &ch, 1);
}

static void pre_value(sc2_jw_t *w)
{
    if (w->err) {
        return;
    }
    if (w->after_key) {
        w->after_key = 0;
        return;
    }
    if (w->depth > 0) {
        if (!w->first[w->depth]) {
            putc1(w, ',');
        }
        w->first[w->depth] = 0;
    } else if (w->len != 0) {
        w->err = SC2_ERR_STATE; /* only one top-level value */
    }
}

void sc2_jw_init(sc2_jw_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (buf == NULL || cap == 0) {
        w->err = SC2_ERR_ARG;
    } else {
        buf[0] = '\0';
    }
}

static void open_container(sc2_jw_t *w, char ch)
{
    pre_value(w);
    if (w->err) {
        return;
    }
    if (w->depth >= SC2_JSON_MAX_DEPTH) {
        w->err = SC2_ERR_BOUNDS;
        return;
    }
    putc1(w, ch);
    w->depth++;
    w->first[w->depth] = 1;
}

static void close_container(sc2_jw_t *w, char ch)
{
    if (w->err) {
        return;
    }
    if (w->depth <= 0 || w->after_key) {
        w->err = SC2_ERR_STATE;
        return;
    }
    putc1(w, ch);
    w->depth--;
}

void sc2_jw_object_begin(sc2_jw_t *w)
{
    open_container(w, '{');
}

void sc2_jw_object_end(sc2_jw_t *w)
{
    close_container(w, '}');
}

void sc2_jw_array_begin(sc2_jw_t *w)
{
    open_container(w, '[');
}

void sc2_jw_array_end(sc2_jw_t *w)
{
    close_container(w, ']');
}

static void write_string(sc2_jw_t *w, const char *s, size_t len)
{
    static const char HEX[] = "0123456789abcdef";
    if (w->err) {
        return;
    }
    if (s == NULL && len != 0) {
        w->err = SC2_ERR_ARG;
        return;
    }
    if (!sc2_utf8_valid(s, len)) {
        w->err = SC2_ERR_FORMAT;
        return;
    }
    putc1(w, '"');
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        const char *esc = NULL;
        char ubuf[6];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) {
                ubuf[0] = '\\';
                ubuf[1] = 'u';
                ubuf[2] = '0';
                ubuf[3] = '0';
                ubuf[4] = HEX[c >> 4];
                ubuf[5] = HEX[c & 15u];
            }
            break;
        }
        if (esc == NULL && c >= 0x20) {
            run++;
            continue;
        }
        put(w, s + i - run, run);
        run = 0;
        if (esc) {
            put(w, esc, strlen(esc));
        } else {
            put(w, ubuf, 6);
        }
    }
    put(w, s + len - run, run);
    putc1(w, '"');
}

void sc2_jw_key(sc2_jw_t *w, const char *key)
{
    if (w->err) {
        return;
    }
    if (w->depth <= 0 || w->after_key || key == NULL) {
        w->err = SC2_ERR_STATE;
        return;
    }
    if (!w->first[w->depth]) {
        putc1(w, ',');
    }
    w->first[w->depth] = 0;
    write_string(w, key, strlen(key));
    putc1(w, ':');
    w->after_key = 1;
}

void sc2_jw_string(sc2_jw_t *w, const char *s, size_t len)
{
    pre_value(w);
    write_string(w, s, len);
}

void sc2_jw_cstring(sc2_jw_t *w, const char *s)
{
    sc2_jw_string(w, s, s ? strlen(s) : 0);
}

static size_t u64_dec(char out[21], uint64_t v)
{
    char tmp[21];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v);
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    return n;
}

void sc2_jw_u64(sc2_jw_t *w, uint64_t v)
{
    char d[21];
    pre_value(w);
    size_t n = u64_dec(d, v);
    put(w, d, n);
}

void sc2_jw_u64_str(sc2_jw_t *w, uint64_t v)
{
    char d[21];
    pre_value(w);
    size_t n = u64_dec(d, v);
    putc1(w, '"');
    put(w, d, n);
    putc1(w, '"');
}

void sc2_jw_bool(sc2_jw_t *w, bool v)
{
    pre_value(w);
    put(w, v ? "true" : "false", v ? 4u : 5u);
}

void sc2_jw_null(sc2_jw_t *w)
{
    pre_value(w);
    put(w, "null", 4u);
}

void sc2_jw_b64(sc2_jw_t *w, const uint8_t *data, size_t len)
{
    pre_value(w);
    if (w->err) {
        return;
    }
    size_t need = SC2_B64_ENCODED_LEN(len);
    if (w->cap - w->len < need + 3u) {
        w->err = SC2_ERR_BOUNDS;
        return;
    }
    putc1(w, '"');
    size_t got = 0;
    int rc = sc2_b64_encode(w->buf + w->len, w->cap - w->len, &got, data, len);
    if (rc != SC2_OK) {
        w->err = rc;
        return;
    }
    w->len += got;
    putc1(w, '"');
}

void sc2_jw_hex(sc2_jw_t *w, const uint8_t *data, size_t len)
{
    pre_value(w);
    if (w->err) {
        return;
    }
    if (w->cap - w->len < 2u * len + 3u) {
        w->err = SC2_ERR_BOUNDS;
        return;
    }
    putc1(w, '"');
    int rc = sc2_hex_encode(w->buf + w->len, w->cap - w->len, data, len);
    if (rc != SC2_OK) {
        w->err = rc;
        return;
    }
    w->len += 2u * len;
    putc1(w, '"');
}

int sc2_jw_finish(sc2_jw_t *w, size_t *out_len)
{
    if (w == NULL) {
        return SC2_ERR_ARG;
    }
    if (!w->err && (w->depth != 0 || w->after_key || w->len == 0)) {
        w->err = SC2_ERR_STATE;
    }
    if (w->err) {
        if (w->buf && w->cap) {
            w->buf[0] = '\0';
        }
        return w->err;
    }
    w->buf[w->len] = '\0';
    if (out_len) {
        *out_len = w->len;
    }
    return SC2_OK;
}
