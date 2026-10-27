/*
 * SC2 portable core: bounded JSON reader and writer for NDJSON frames.
 *
 * The reader works on one complete object without allocating. It validates
 * the whole text (RFC 8259 syntax, depth <= SC2_JSON_MAX_DEPTH, valid
 * UTF-8 in strings) and rejects duplicate keys in the looked-up object.
 * The writer produces compact JSON (no spaces) identical to Python's
 * json.dumps(obj, separators=(",", ":"), ensure_ascii=False).
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_JSON_H
#define SC2_JSON_H

#include <stdbool.h>
#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_JSON_MAX_DEPTH 8

typedef enum {
    SC2_JSON_NONE = 0,
    SC2_JSON_OBJECT,
    SC2_JSON_ARRAY,
    SC2_JSON_STRING,
    SC2_JSON_NUMBER,
    SC2_JSON_TRUE,
    SC2_JSON_FALSE,
    SC2_JSON_NULL
} sc2_json_type_t;

/* A value inside a JSON text: [ptr, ptr+len) covers the raw token
 * (strings include their quotes, objects their braces). */
typedef struct {
    sc2_json_type_t type;
    const char *ptr;
    size_t len;
} sc2_json_val_t;

/* SC2_OK if text is exactly one JSON value (surrounding whitespace allowed). */
int sc2_json_validate(const char *text, size_t len);

/* Parse text as one value into *root. */
int sc2_json_root(const char *text, size_t len, sc2_json_val_t *root);

/* Look up key in object obj. SC2_ERR_NOT_FOUND if absent, SC2_ERR_FORMAT
 * if obj is not an object or the key occurs twice. */
int sc2_json_get(const sc2_json_val_t *obj, const char *key, sc2_json_val_t *out);

/* Array iteration: *index starts at 0. Returns SC2_ERR_NOT_FOUND at the end. */
int sc2_json_array_next(const sc2_json_val_t *arr, size_t *pos, sc2_json_val_t *out);

/* Unescape a string into out (NUL-terminated); *out_len excludes the NUL. */
int sc2_json_string(const sc2_json_val_t *val, char *out, size_t cap, size_t *out_len);
/* Compare a string value with a C string, without unescaping into a buffer. */
bool sc2_json_string_eq(const sc2_json_val_t *val, const char *s);
/* Non-negative integer number without fraction/exponent, <= max. */
int sc2_json_u64(const sc2_json_val_t *val, uint64_t max, uint64_t *out);
/* Decimal digits inside a string ("4523"), as used for relay seq values. */
int sc2_json_u64_str(const sc2_json_val_t *val, uint64_t *out);
int sc2_json_bool(const sc2_json_val_t *val, bool *out);
/* Strict base64 string into bytes. */
int sc2_json_b64(const sc2_json_val_t *val, uint8_t *out, size_t cap, size_t *out_len);
/* Base64 string that must decode to exactly len bytes. */
int sc2_json_b64_exact(const sc2_json_val_t *val, uint8_t *out, size_t len);
/* Hex string that must decode to exactly len bytes. */
int sc2_json_hex_exact(const sc2_json_val_t *val, uint8_t *out, size_t len);

/* Convenience lookups: get + convert. */
int sc2_json_get_string(const sc2_json_val_t *obj, const char *key, char *out, size_t cap, size_t *out_len);
int sc2_json_get_u64(const sc2_json_val_t *obj, const char *key, uint64_t max, uint64_t *out);
int sc2_json_get_b64_exact(const sc2_json_val_t *obj, const char *key, uint8_t *out, size_t len);
int sc2_json_get_hex_exact(const sc2_json_val_t *obj, const char *key, uint8_t *out, size_t len);

/* ---- Writer ---- */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int err;
    int depth;
    uint8_t first[SC2_JSON_MAX_DEPTH + 1];
    uint8_t after_key;
} sc2_jw_t;

void sc2_jw_init(sc2_jw_t *w, char *buf, size_t cap);
void sc2_jw_object_begin(sc2_jw_t *w);
void sc2_jw_object_end(sc2_jw_t *w);
void sc2_jw_array_begin(sc2_jw_t *w);
void sc2_jw_array_end(sc2_jw_t *w);
void sc2_jw_key(sc2_jw_t *w, const char *key);
/* UTF-8 string; invalid UTF-8 sets SC2_ERR_FORMAT. */
void sc2_jw_string(sc2_jw_t *w, const char *s, size_t len);
void sc2_jw_cstring(sc2_jw_t *w, const char *s);
void sc2_jw_u64(sc2_jw_t *w, uint64_t v);
/* Decimal string ("4523"). */
void sc2_jw_u64_str(sc2_jw_t *w, uint64_t v);
void sc2_jw_bool(sc2_jw_t *w, bool v);
void sc2_jw_null(sc2_jw_t *w);
void sc2_jw_b64(sc2_jw_t *w, const uint8_t *data, size_t len);
void sc2_jw_hex(sc2_jw_t *w, const uint8_t *data, size_t len);
/* NUL-terminates; returns the first error or SC2_OK with *out_len set. */
int sc2_jw_finish(sc2_jw_t *w, size_t *out_len);

/* Validate UTF-8 (no overlongs, no surrogates, <= U+10FFFF). */
bool sc2_utf8_valid(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif
