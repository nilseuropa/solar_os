/*
 * SC2 portable core: strict Ed25519 on top of the orlp implementation
 * (src/vendor/meshcore/ed25519, zlib licence, used unmodified).
 *
 * orlp's ed25519_verify only rejects S with the top three bits set. SC2
 * applies the checks libsodium (>= 1.0.16, non-compat build) applies:
 *   - S is canonical (S < L), so S + L malleations are rejected;
 *   - the public key is a canonical encoding (y < p), decodes, and is not
 *     of small order;
 *   - R is a canonical encoding, decodes, and is not of small order.
 * Signing and verification are re-expressed over multiple message parts
 * with orlp's SHA-512, scalar and group functions, so callers do not need
 * to concatenate large buffers.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_crypto.h"

#include <string.h>

#include "ge.h"
#include "sc.h"
#include "sha512.h"

static const uint8_t ED25519_L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                      0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

/* 1 if s < L (little-endian), constant time. */
static int scalar_is_canonical(const uint8_t s[32])
{
    unsigned c = 0;
    unsigned n = 1;
    size_t i = 32;
    do {
        i--;
        c |= (((unsigned)s[i] - (unsigned)ED25519_L[i]) >> 8) & n;
        n &= (((unsigned)(s[i] ^ ED25519_L[i])) - 1u) >> 8;
    } while (i != 0);
    return c != 0;
}

/* 1 if the y coordinate (sign bit ignored) is < p. */
static int point_is_canonical(const uint8_t s[32])
{
    unsigned c = (s[31] & 0x7fu) ^ 0x7fu;
    for (size_t i = 30; i > 0; i--) {
        c |= s[i] ^ 0xffu;
    }
    c = (c - 1u) >> 8;
    unsigned d = (0xedu - 1u - (unsigned)s[0]) >> 8;
    return 1 - (int)(c & d & 1u);
}

/* Decode (negated, as orlp does; the order is the same) and test 8*P == O. */
static int point_decode_check(const uint8_t s[32])
{
    ge_p3 p;
    ge_p1p1 t;
    uint8_t enc[32];
    static const uint8_t identity[32] = {1};

    if (!point_is_canonical(s)) {
        return SC2_ERR_KEY;
    }
    if (ge_frombytes_negate_vartime(&p, s) != 0) {
        return SC2_ERR_KEY;
    }
    for (int i = 0; i < 3; i++) {
        ge_p3_dbl(&t, &p);
        ge_p1p1_to_p3(&p, &t);
    }
    ge_p3_tobytes(enc, &p);
    if (memcmp(enc, identity, sizeof(enc)) == 0) {
        return SC2_ERR_KEY;
    }
    return SC2_OK;
}

static void expand_seed(uint8_t az[64], uint8_t pub[32], const uint8_t seed[32])
{
    ge_p3 a;
    sha512(seed, 32, az);
    az[0] &= 248;
    az[31] &= 63;
    az[31] |= 64;
    ge_scalarmult_base(&a, az);
    ge_p3_tobytes(pub, &a);
}

int sc2_ed25519_public(uint8_t pub[SC2_KEY_LEN], const uint8_t seed[SC2_KEY_LEN])
{
    uint8_t az[64];
    if (pub == NULL || seed == NULL) {
        return SC2_ERR_ARG;
    }
    expand_seed(az, pub, seed);
    sc2_memzero(az, sizeof(az));
    return SC2_OK;
}

int sc2_ed25519_generate(uint8_t seed[SC2_KEY_LEN], uint8_t pub[SC2_KEY_LEN], const sc2_rng_t *rng)
{
    if (seed == NULL || pub == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_random(rng, seed, SC2_KEY_LEN);
    if (rc != SC2_OK) {
        return rc;
    }
    return sc2_ed25519_public(pub, seed);
}

static int parts_valid(const sc2_part_t *parts, size_t count)
{
    if (parts == NULL && count != 0) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (parts[i].ptr == NULL && parts[i].len != 0) {
            return 0;
        }
    }
    return 1;
}

static void hash_parts(sha512_context *h, const sc2_part_t *parts, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (parts[i].len) {
            sha512_update(h, parts[i].ptr, parts[i].len);
        }
    }
}

int sc2_ed25519_sign_parts(uint8_t sig[SC2_SIG_LEN], const sc2_part_t *parts, size_t count,
                           const uint8_t seed[SC2_KEY_LEN])
{
    uint8_t az[64];
    uint8_t pub[32];
    uint8_t r[64];
    uint8_t hram[64];
    sha512_context h;
    ge_p3 rp;

    if (sig == NULL || seed == NULL || !parts_valid(parts, count)) {
        return SC2_ERR_ARG;
    }
    expand_seed(az, pub, seed);

    sha512_init(&h);
    sha512_update(&h, az + 32, 32);
    hash_parts(&h, parts, count);
    sha512_final(&h, r);
    sc_reduce(r);
    ge_scalarmult_base(&rp, r);
    ge_p3_tobytes(sig, &rp);

    sha512_init(&h);
    sha512_update(&h, sig, 32);
    sha512_update(&h, pub, 32);
    hash_parts(&h, parts, count);
    sha512_final(&h, hram);
    sc_reduce(hram);
    sc_muladd(sig + 32, hram, az, r);

    sc2_memzero(az, sizeof(az));
    sc2_memzero(r, sizeof(r));
    sc2_memzero(&h, sizeof(h));
    return SC2_OK;
}

int sc2_ed25519_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t *msg, size_t len, const uint8_t seed[SC2_KEY_LEN])
{
    sc2_part_t part = {msg, len};
    return sc2_ed25519_sign_parts(sig, &part, 1, seed);
}

int sc2_ed25519_check_public(const uint8_t pub[SC2_KEY_LEN])
{
    if (pub == NULL) {
        return SC2_ERR_ARG;
    }
    return point_decode_check(pub);
}

int sc2_ed25519_verify_parts(const uint8_t sig[SC2_SIG_LEN], const sc2_part_t *parts, size_t count,
                             const uint8_t pub[SC2_KEY_LEN])
{
    uint8_t h[64];
    uint8_t check[32];
    sha512_context hs;
    ge_p3 a;
    ge_p2 r;

    if (sig == NULL || pub == NULL || !parts_valid(parts, count)) {
        return SC2_ERR_ARG;
    }
    if (point_decode_check(pub) != SC2_OK) {
        return SC2_ERR_KEY;
    }
    if (!scalar_is_canonical(sig + 32)) {
        return SC2_ERR_AUTH;
    }
    if (point_decode_check(sig) != SC2_OK) {
        return SC2_ERR_AUTH;
    }
    if (ge_frombytes_negate_vartime(&a, pub) != 0) {
        return SC2_ERR_KEY;
    }
    sha512_init(&hs);
    sha512_update(&hs, sig, 32);
    sha512_update(&hs, pub, 32);
    hash_parts(&hs, parts, count);
    sha512_final(&hs, h);
    sc_reduce(h);
    ge_double_scalarmult_vartime(&r, h, &a, sig + 32);
    ge_tobytes(check, &r);
    return sc2_ct_eq(check, sig, 32) ? SC2_OK : SC2_ERR_AUTH;
}

int sc2_ed25519_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t *msg, size_t len,
                       const uint8_t pub[SC2_KEY_LEN])
{
    sc2_part_t part = {msg, len};
    return sc2_ed25519_verify_parts(sig, &part, 1, pub);
}
