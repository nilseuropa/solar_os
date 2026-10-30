/*
 * SC2 portable core: common helpers.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_common.h"

#include <string.h>

const char *sc2_strerror(int err)
{
    switch (err) {
    case SC2_OK: return "ok";
    case SC2_ERR_ARG: return "invalid argument";
    case SC2_ERR_BOUNDS: return "buffer too small or limit exceeded";
    case SC2_ERR_FORMAT: return "malformed encoding";
    case SC2_ERR_AUTH: return "authentication failed";
    case SC2_ERR_KEY: return "unusable key";
    case SC2_ERR_RNG: return "random source failed";
    case SC2_ERR_REPLAY: return "replayed message";
    case SC2_ERR_SKIP: return "too many skipped messages";
    case SC2_ERR_STATE: return "invalid session state";
    case SC2_ERR_BACKEND: return "hash backend failure";
    case SC2_ERR_NOT_FOUND: return "not found";
    default: return "unknown error";
    }
}

void sc2_memzero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

int sc2_ct_eq(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) {
        d |= (uint8_t)(x[i] ^ y[i]);
    }
    return (int)(1u & (((uint32_t)d - 1u) >> 8));
}

int sc2_ct_is_zero(const void *ptr, size_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)ptr;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) {
        d |= p[i];
    }
    return (int)(1u & (((uint32_t)d - 1u) >> 8));
}

int sc2_random(const sc2_rng_t *rng, uint8_t *out, size_t len)
{
    if (rng == NULL || rng->fn == NULL || (out == NULL && len != 0)) {
        return SC2_ERR_RNG;
    }
    if (len == 0) {
        return SC2_OK;
    }
    if (rng->fn(rng->ctx, out, len) != 0) {
        sc2_memzero(out, len);
        return SC2_ERR_RNG;
    }
    return SC2_OK;
}

uint32_t sc2_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
