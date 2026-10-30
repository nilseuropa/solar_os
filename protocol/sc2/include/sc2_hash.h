/*
 * SC2 portable core: SHA-256, HMAC-SHA-256 and HKDF-SHA-256 (RFC 5869).
 *
 * Backends: the portable C implementation (default, used for host builds)
 * or mbedTLS (define SC2_USE_MBEDTLS=1; used in ESP-IDF firmware, where
 * MBEDTLS_HKDF_C is disabled, so HKDF is always implemented here).
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_HASH_H
#define SC2_HASH_H

#include "sc2_common.h"

#if defined(SC2_USE_MBEDTLS) && SC2_USE_MBEDTLS
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_SHA256_LEN 32u
#define SC2_SHA256_BLOCK 64u
#define SC2_HKDF_MAX_OUT (255u * SC2_SHA256_LEN)

typedef struct {
#if defined(SC2_USE_MBEDTLS) && SC2_USE_MBEDTLS
    mbedtls_sha256_context ctx;
#else
    uint32_t state[8];
    uint64_t total;
    uint8_t buf[SC2_SHA256_BLOCK];
    size_t used;
#endif
} sc2_sha256_ctx;

typedef struct {
#if defined(SC2_USE_MBEDTLS) && SC2_USE_MBEDTLS
    mbedtls_md_context_t md;
#else
    sc2_sha256_ctx inner;
    sc2_sha256_ctx outer;
#endif
} sc2_hmac_ctx;

/* Name of the compiled backend: "portable" or "mbedtls". */
const char *sc2_hash_backend(void);

int sc2_sha256_init(sc2_sha256_ctx *ctx);
int sc2_sha256_update(sc2_sha256_ctx *ctx, const uint8_t *data, size_t len);
/* Writes the digest and releases the context (it must be re-initialised). */
int sc2_sha256_final(sc2_sha256_ctx *ctx, uint8_t out[SC2_SHA256_LEN]);
int sc2_sha256(uint8_t out[SC2_SHA256_LEN], const uint8_t *data, size_t len);

int sc2_hmac_init(sc2_hmac_ctx *ctx, const uint8_t *key, size_t key_len);
int sc2_hmac_update(sc2_hmac_ctx *ctx, const uint8_t *data, size_t len);
/* Writes the MAC and releases the context. */
int sc2_hmac_final(sc2_hmac_ctx *ctx, uint8_t out[SC2_SHA256_LEN]);
int sc2_hmac_sha256(uint8_t out[SC2_SHA256_LEN], const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t len);

/* HKDF-Extract: prk = HMAC(salt, ikm). salt may be NULL/0 (= 32 zero bytes). */
int sc2_hkdf_extract(uint8_t prk[SC2_SHA256_LEN], const uint8_t *salt, size_t salt_len,
                     const uint8_t *ikm, size_t ikm_len);
/* HKDF-Expand with info given as concatenated parts. out_len <= 8160. */
int sc2_hkdf_expand_parts(uint8_t *out, size_t out_len, const uint8_t *prk, size_t prk_len,
                          const sc2_part_t *info, size_t info_count);
int sc2_hkdf_expand(uint8_t *out, size_t out_len, const uint8_t *prk, size_t prk_len,
                    const uint8_t *info, size_t info_len);
/* Extract-then-expand. */
int sc2_hkdf(uint8_t *out, size_t out_len, const uint8_t *salt, size_t salt_len,
             const uint8_t *ikm, size_t ikm_len, const uint8_t *info, size_t info_len);

#ifdef __cplusplus
}
#endif

#endif
