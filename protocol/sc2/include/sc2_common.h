/*
 * SC2 (SolarOS Chat v2) portable core: common definitions.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_COMMON_H
#define SC2_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Suite SC2-1: Ed25519 + X25519 + XChaCha20-Poly1305 + HKDF/HMAC-SHA-256. */
#define SC2_SUITE_1 1u

#define SC2_DEVICE_ID_LEN 16u      /* raw bytes; 32 hex characters on the wire */
#define SC2_DEVICE_ID_HEX_LEN 32u
#define SC2_KEY_LEN 32u
#define SC2_SIG_LEN 64u
#define SC2_NONCE_LEN 24u
#define SC2_TAG_LEN 16u
#define SC2_ROOM_ID_LEN 16u
#define SC2_MSG_ID_LEN 8u

/* Memory profile. SC2_LOW_RAM=1 trades a few local limits for less RAM on
 * devices without PSRAM; it never changes the wire format.
 * The core includes no SDK headers, so the build decides: the SolarOS
 * firmware (src/CMakeLists.txt) sets it when CONFIG_SPIRAM is off. Default 0. */
#ifndef SC2_LOW_RAM
#define SC2_LOW_RAM 0
#endif

/* Error codes. Every function that can fail returns SC2_OK or one of these. */
enum {
    SC2_OK = 0,
    SC2_ERR_ARG = -1,      /* invalid argument (NULL, bad length, out of range) */
    SC2_ERR_BOUNDS = -2,   /* output buffer too small or input over a limit */
    SC2_ERR_FORMAT = -3,   /* malformed encoding (JSON, base64, hex, padding, state blob) */
    SC2_ERR_AUTH = -4,     /* signature or AEAD tag did not verify */
    SC2_ERR_KEY = -5,      /* unusable key: small order, non-canonical, all-zero DH */
    SC2_ERR_RNG = -6,      /* random source failed or missing */
    SC2_ERR_REPLAY = -7,   /* message index already consumed */
    SC2_ERR_SKIP = -8,     /* too many skipped messages */
    SC2_ERR_STATE = -9,    /* operation not valid in the current session state */
    SC2_ERR_BACKEND = -10, /* hash backend failure (e.g. mbedTLS allocation) */
    SC2_ERR_NOT_FOUND = -11 /* requested field or entry is absent */
};

/* Platform random source. Must fill out[0..len) with cryptographically
 * secure random bytes and return 0, or return non-zero on failure. */
typedef int (*sc2_rng_fn)(void *ctx, uint8_t *out, size_t len);

typedef struct {
    sc2_rng_fn fn;
    void *ctx;
} sc2_rng_t;

/* A borrowed byte span, used for multi-part signing and hashing. */
typedef struct {
    const uint8_t *ptr;
    size_t len;
} sc2_part_t;

const char *sc2_strerror(int err);

/* Zero memory in a way the compiler may not elide. */
void sc2_memzero(void *ptr, size_t len);

/* Constant-time comparison: returns 1 if equal, 0 otherwise. */
int sc2_ct_eq(const void *a, const void *b, size_t len);

/* Constant-time test: returns 1 if all bytes are zero, 0 otherwise. */
int sc2_ct_is_zero(const void *ptr, size_t len);

/* Fill from the injected RNG. Returns SC2_ERR_RNG on failure. */
int sc2_random(const sc2_rng_t *rng, uint8_t *out, size_t len);

/* CRC-32 (IEEE 802.3, reflected, as zlib/binascii.crc32). */
uint32_t sc2_crc32(uint32_t crc, const uint8_t *data, size_t len);

static inline void sc2_store_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void sc2_store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void sc2_store_be64(uint8_t *p, uint64_t v)
{
    sc2_store_be32(p, (uint32_t)(v >> 32));
    sc2_store_be32(p + 4, (uint32_t)v);
}

static inline uint32_t sc2_load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t sc2_load_be64(const uint8_t *p)
{
    return ((uint64_t)sc2_load_be32(p) << 32) | (uint64_t)sc2_load_be32(p + 4);
}

#ifdef __cplusplus
}
#endif

#endif
