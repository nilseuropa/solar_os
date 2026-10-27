/*
 * SC2 portable core: SHA-256 / HMAC-SHA-256 backends and HKDF (RFC 5869).
 * SPDX-License-Identifier: MIT
 */
#include "sc2_hash.h"

#include <string.h>

#if defined(SC2_USE_MBEDTLS) && SC2_USE_MBEDTLS

const char *sc2_hash_backend(void)
{
    return "mbedtls";
}

int sc2_sha256_init(sc2_sha256_ctx *ctx)
{
    if (ctx == NULL) {
        return SC2_ERR_ARG;
    }
    mbedtls_sha256_init(&ctx->ctx);
    if (mbedtls_sha256_starts(&ctx->ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx->ctx);
        return SC2_ERR_BACKEND;
    }
    return SC2_OK;
}

int sc2_sha256_update(sc2_sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL || (data == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    if (len == 0) {
        return SC2_OK;
    }
    return mbedtls_sha256_update(&ctx->ctx, data, len) == 0 ? SC2_OK : SC2_ERR_BACKEND;
}

int sc2_sha256_final(sc2_sha256_ctx *ctx, uint8_t out[SC2_SHA256_LEN])
{
    if (ctx == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = mbedtls_sha256_finish(&ctx->ctx, out) == 0 ? SC2_OK : SC2_ERR_BACKEND;
    mbedtls_sha256_free(&ctx->ctx);
    return rc;
}

int sc2_hmac_init(sc2_hmac_ctx *ctx, const uint8_t *key, size_t key_len)
{
    if (ctx == NULL || (key == NULL && key_len != 0)) {
        return SC2_ERR_ARG;
    }
    static const uint8_t empty[1] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return SC2_ERR_BACKEND;
    }
    mbedtls_md_init(&ctx->md);
    if (mbedtls_md_setup(&ctx->md, info, 1) != 0 ||
        mbedtls_md_hmac_starts(&ctx->md, key_len ? key : empty, key_len) != 0) {
        mbedtls_md_free(&ctx->md);
        return SC2_ERR_BACKEND;
    }
    return SC2_OK;
}

int sc2_hmac_update(sc2_hmac_ctx *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL || (data == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    if (len == 0) {
        return SC2_OK;
    }
    return mbedtls_md_hmac_update(&ctx->md, data, len) == 0 ? SC2_OK : SC2_ERR_BACKEND;
}

int sc2_hmac_final(sc2_hmac_ctx *ctx, uint8_t out[SC2_SHA256_LEN])
{
    if (ctx == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = mbedtls_md_hmac_finish(&ctx->md, out) == 0 ? SC2_OK : SC2_ERR_BACKEND;
    mbedtls_md_free(&ctx->md);
    return rc;
}

#else /* portable backend */

const char *sc2_hash_backend(void)
{
    return "portable";
}

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t ror32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(uint32_t st[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = sc2_load_be32(p + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k256[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
    st[5] += f;
    st[6] += g;
    st[7] += h;
    sc2_memzero(w, sizeof(w));
}

int sc2_sha256_init(sc2_sha256_ctx *ctx)
{
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    if (ctx == NULL) {
        return SC2_ERR_ARG;
    }
    memcpy(ctx->state, iv, sizeof(iv));
    ctx->total = 0;
    ctx->used = 0;
    return SC2_OK;
}

int sc2_sha256_update(sc2_sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL || (data == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    ctx->total += (uint64_t)len;
    if (ctx->used) {
        size_t take = SC2_SHA256_BLOCK - ctx->used;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->used, data, take);
        ctx->used += take;
        data += take;
        len -= take;
        if (ctx->used < SC2_SHA256_BLOCK) {
            return SC2_OK;
        }
        sha256_block(ctx->state, ctx->buf);
        ctx->used = 0;
    }
    while (len >= SC2_SHA256_BLOCK) {
        sha256_block(ctx->state, data);
        data += SC2_SHA256_BLOCK;
        len -= SC2_SHA256_BLOCK;
    }
    if (len) {
        memcpy(ctx->buf, data, len);
        ctx->used = len;
    }
    return SC2_OK;
}

int sc2_sha256_final(sc2_sha256_ctx *ctx, uint8_t out[SC2_SHA256_LEN])
{
    if (ctx == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    uint64_t bits = ctx->total * 8u;
    ctx->buf[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->buf + ctx->used, 0, SC2_SHA256_BLOCK - ctx->used);
        sha256_block(ctx->state, ctx->buf);
        ctx->used = 0;
    }
    memset(ctx->buf + ctx->used, 0, 56 - ctx->used);
    sc2_store_be64(ctx->buf + 56, bits);
    sha256_block(ctx->state, ctx->buf);
    for (int i = 0; i < 8; i++) {
        sc2_store_be32(out + 4 * i, ctx->state[i]);
    }
    sc2_memzero(ctx, sizeof(*ctx));
    return SC2_OK;
}

int sc2_hmac_init(sc2_hmac_ctx *ctx, const uint8_t *key, size_t key_len)
{
    if (ctx == NULL || (key == NULL && key_len != 0)) {
        return SC2_ERR_ARG;
    }
    uint8_t k[SC2_SHA256_BLOCK];
    uint8_t pad[SC2_SHA256_BLOCK];
    memset(k, 0, sizeof(k));
    if (key_len > SC2_SHA256_BLOCK) {
        sc2_sha256(k, key, key_len);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    for (size_t i = 0; i < SC2_SHA256_BLOCK; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x36u);
    }
    sc2_sha256_init(&ctx->inner);
    sc2_sha256_update(&ctx->inner, pad, sizeof(pad));
    for (size_t i = 0; i < SC2_SHA256_BLOCK; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    sc2_sha256_init(&ctx->outer);
    sc2_sha256_update(&ctx->outer, pad, sizeof(pad));
    sc2_memzero(k, sizeof(k));
    sc2_memzero(pad, sizeof(pad));
    return SC2_OK;
}

int sc2_hmac_update(sc2_hmac_ctx *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL) {
        return SC2_ERR_ARG;
    }
    return sc2_sha256_update(&ctx->inner, data, len);
}

int sc2_hmac_final(sc2_hmac_ctx *ctx, uint8_t out[SC2_SHA256_LEN])
{
    if (ctx == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    uint8_t ih[SC2_SHA256_LEN];
    sc2_sha256_final(&ctx->inner, ih);
    sc2_sha256_update(&ctx->outer, ih, sizeof(ih));
    sc2_sha256_final(&ctx->outer, out);
    sc2_memzero(ih, sizeof(ih));
    return SC2_OK;
}

#endif /* backend */

int sc2_sha256(uint8_t out[SC2_SHA256_LEN], const uint8_t *data, size_t len)
{
    sc2_sha256_ctx ctx;
    int rc = sc2_sha256_init(&ctx);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_sha256_update(&ctx, data, len);
    int rc2 = sc2_sha256_final(&ctx, out);
    return rc != SC2_OK ? rc : rc2;
}

int sc2_hmac_sha256(uint8_t out[SC2_SHA256_LEN], const uint8_t *key, size_t key_len, const uint8_t *data,
                    size_t len)
{
    sc2_hmac_ctx ctx;
    int rc = sc2_hmac_init(&ctx, key, key_len);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_hmac_update(&ctx, data, len);
    int rc2 = sc2_hmac_final(&ctx, out);
    return rc != SC2_OK ? rc : rc2;
}

int sc2_hkdf_extract(uint8_t prk[SC2_SHA256_LEN], const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                     size_t ikm_len)
{
    static const uint8_t zero_salt[SC2_SHA256_LEN] = {0};
    if (prk == NULL || (ikm == NULL && ikm_len != 0) || (salt == NULL && salt_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (salt_len == 0) {
        salt = zero_salt;
        salt_len = sizeof(zero_salt);
    }
    return sc2_hmac_sha256(prk, salt, salt_len, ikm, ikm_len);
}

int sc2_hkdf_expand_parts(uint8_t *out, size_t out_len, const uint8_t *prk, size_t prk_len,
                          const sc2_part_t *info, size_t info_count)
{
    if (out == NULL || prk == NULL || prk_len < SC2_SHA256_LEN || out_len == 0 || out_len > SC2_HKDF_MAX_OUT ||
        (info == NULL && info_count != 0)) {
        return SC2_ERR_ARG;
    }
    uint8_t t[SC2_SHA256_LEN];
    size_t t_len = 0;
    size_t done = 0;
    uint8_t counter = 1;
    int rc = SC2_OK;
    while (done < out_len) {
        sc2_hmac_ctx ctx;
        rc = sc2_hmac_init(&ctx, prk, prk_len);
        if (rc != SC2_OK) {
            break;
        }
        rc = sc2_hmac_update(&ctx, t, t_len);
        for (size_t i = 0; rc == SC2_OK && i < info_count; i++) {
            rc = sc2_hmac_update(&ctx, info[i].ptr, info[i].len);
        }
        if (rc == SC2_OK) {
            rc = sc2_hmac_update(&ctx, &counter, 1);
        }
        int rc2 = sc2_hmac_final(&ctx, t);
        if (rc == SC2_OK) {
            rc = rc2;
        }
        if (rc != SC2_OK) {
            break;
        }
        t_len = SC2_SHA256_LEN;
        size_t take = out_len - done < SC2_SHA256_LEN ? out_len - done : SC2_SHA256_LEN;
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
    sc2_memzero(t, sizeof(t));
    if (rc != SC2_OK) {
        sc2_memzero(out, out_len);
    }
    return rc;
}

int sc2_hkdf_expand(uint8_t *out, size_t out_len, const uint8_t *prk, size_t prk_len, const uint8_t *info,
                    size_t info_len)
{
    sc2_part_t part = {info, info_len};
    if (info == NULL && info_len != 0) {
        return SC2_ERR_ARG;
    }
    return sc2_hkdf_expand_parts(out, out_len, prk, prk_len, &part, info_len ? 1u : 0u);
}

int sc2_hkdf(uint8_t *out, size_t out_len, const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
             size_t ikm_len, const uint8_t *info, size_t info_len)
{
    uint8_t prk[SC2_SHA256_LEN];
    int rc = sc2_hkdf_extract(prk, salt, salt_len, ikm, ikm_len);
    if (rc == SC2_OK) {
        rc = sc2_hkdf_expand(out, out_len, prk, sizeof(prk), info, info_len);
    }
    sc2_memzero(prk, sizeof(prk));
    return rc;
}
