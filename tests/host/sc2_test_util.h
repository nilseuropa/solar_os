/*
 * Shared helpers for the SC2 host tests (protocol/sc2).
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_TEST_UTIL_H
#define SC2_TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2.h"

static int sc2t_failures;
static int sc2t_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        sc2t_checks++;                                                           \
        if (!(cond)) {                                                           \
            sc2t_failures++;                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_RC(expr, want)                                                     \
    do {                                                                         \
        int rc_ = (expr);                                                        \
        sc2t_checks++;                                                           \
        if (rc_ != (want)) {                                                     \
            sc2t_failures++;                                                     \
            fprintf(stderr, "%s:%d: %s returned %d (%s), expected %d\n", __FILE__, __LINE__, #expr, rc_, \
                    sc2_strerror(rc_), (int)(want));                             \
        }                                                                        \
    } while (0)

#define CHECK_MEM(a, b, n)                                                       \
    do {                                                                         \
        sc2t_checks++;                                                           \
        if (memcmp((a), (b), (n)) != 0) {                                        \
            sc2t_failures++;                                                     \
            fprintf(stderr, "%s:%d: bytes differ: %s vs %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                        \
    } while (0)

/* Independent hex decoder (not the one under test). Allocates; caller frees. */
static inline uint8_t *sc2t_hex(const char *hex, size_t *len)
{
    size_t n = strlen(hex);
    if (n % 2u) {
        fprintf(stderr, "odd hex length\n");
        exit(2);
    }
    uint8_t *out = (uint8_t *)malloc(n / 2u + 1u);
    if (out == NULL) {
        exit(2);
    }
    for (size_t i = 0; i < n / 2u; i++) {
        unsigned v;
        if (sscanf(hex + 2u * i, "%2x", &v) != 1) {
            fprintf(stderr, "bad hex\n");
            exit(2);
        }
        out[i] = (uint8_t)v;
    }
    if (len) {
        *len = n / 2u;
    }
    return out;
}

/* Decode exactly n bytes into a fixed buffer. */
static inline void sc2t_hexn(uint8_t *out, size_t n, const char *hex)
{
    size_t got = 0;
    uint8_t *b = sc2t_hex(hex, &got);
    if (got != n) {
        fprintf(stderr, "hex length %zu, expected %zu\n", got, n);
        exit(2);
    }
    memcpy(out, b, n);
    free(b);
}

/* Scripted RNG: serves bytes from a fixed buffer, fails when exhausted. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} sc2t_script_t;

static inline int sc2t_script_fn(void *ctx, uint8_t *out, size_t n)
{
    sc2t_script_t *s = (sc2t_script_t *)ctx;
    if (s->pos + n > s->len) {
        return -1;
    }
    memcpy(out, s->data + s->pos, n);
    s->pos += n;
    return 0;
}

/* Deterministic xorshift RNG for property tests. */
static uint64_t sc2t_xs_state = 0x9E3779B97F4A7C15ull;

static inline uint64_t sc2t_xs(void)
{
    uint64_t x = sc2t_xs_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    sc2t_xs_state = x;
    return x;
}

static inline int sc2t_xs_fn(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)sc2t_xs();
    }
    return 0;
}

static inline int sc2t_fail_fn(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    (void)out;
    (void)n;
    return -1;
}

static inline int sc2t_report(const char *name)
{
    if (sc2t_failures) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, sc2t_failures, sc2t_checks);
        return 1;
    }
    printf("%s: all %d checks passed (hash backend: %s)\n", name, sc2t_checks, sc2_hash_backend());
    return 0;
}

#endif
