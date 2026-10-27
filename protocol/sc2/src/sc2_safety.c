/*
 * SC2 portable core: safety numbers and fingerprints.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_safety.h"

#include <string.h>

#include "sc2_hash.h"

static const uint8_t LABEL_SAFETY[] = {'S', 'C', '2', '-', 'S', 'A', 'F', 'E', 'T', 'Y', 0x01};
static const uint8_t LABEL_FULL[] = {'S', 'C', '2', '-', 'S', 'A', 'F', 'E', 'T', 'Y', '-', 'F', 'U', 'L', 'L', 0x01};

int sc2_safety_hash(uint8_t h[32], const uint8_t ik_a[SC2_KEY_LEN], const uint8_t ik_b[SC2_KEY_LEN])
{
    if (h == NULL || ik_a == NULL || ik_b == NULL) {
        return SC2_ERR_ARG;
    }
    const uint8_t *lo = ik_a;
    const uint8_t *hi = ik_b;
    if (memcmp(ik_a, ik_b, SC2_KEY_LEN) > 0) {
        lo = ik_b;
        hi = ik_a;
    }
    sc2_sha256_ctx ctx;
    int rc = sc2_sha256_init(&ctx);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_sha256_update(&ctx, LABEL_SAFETY, sizeof(LABEL_SAFETY));
    if (rc == SC2_OK) {
        rc = sc2_sha256_update(&ctx, lo, SC2_KEY_LEN);
    }
    if (rc == SC2_OK) {
        rc = sc2_sha256_update(&ctx, hi, SC2_KEY_LEN);
    }
    int rc2 = sc2_sha256_final(&ctx, h);
    return rc != SC2_OK ? rc : rc2;
}

static uint64_t load_be40(const uint8_t *p)
{
    return ((uint64_t)p[0] << 32) | ((uint64_t)p[1] << 24) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 8) |
           (uint64_t)p[4];
}

static void groups(char *out, const uint8_t *src, size_t ngroups, unsigned digits)
{
    uint64_t mod = 1;
    for (unsigned i = 0; i < digits; i++) {
        mod *= 10u;
    }
    size_t o = 0;
    for (size_t g = 0; g < ngroups; g++) {
        uint64_t v = load_be40(src + 5u * g) % mod;
        if (g) {
            out[o++] = ' ';
        }
        for (unsigned d = digits; d > 0; d--) {
            out[o + d - 1u] = (char)('0' + (int)(v % 10u));
            v /= 10u;
        }
        o += digits;
    }
    out[o] = '\0';
}

int sc2_safety_short(char out[SC2_SAFETY_SHORT_LEN + 1], const uint8_t h[32])
{
    if (out == NULL || h == NULL) {
        return SC2_ERR_ARG;
    }
    groups(out, h, 6, 4);
    return SC2_OK;
}

int sc2_safety_full(char out[SC2_SAFETY_FULL_LEN + 1], const uint8_t h[32])
{
    uint8_t m[60];
    if (out == NULL || h == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_hkdf_expand(m, sizeof(m), h, 32, LABEL_FULL, sizeof(LABEL_FULL));
    if (rc != SC2_OK) {
        return rc;
    }
    groups(out, m, 12, 5);
    sc2_memzero(m, sizeof(m));
    return SC2_OK;
}

int sc2_safety_word_indices(uint16_t idx[SC2_SAFETY_WORDS], const uint8_t h[32])
{
    if (idx == NULL || h == NULL) {
        return SC2_ERR_ARG;
    }
    for (unsigned w = 0; w < SC2_SAFETY_WORDS; w++) {
        unsigned bit = w * 11u;
        uint32_t v = ((uint32_t)h[bit / 8u] << 16) | ((uint32_t)h[bit / 8u + 1u] << 8) | (uint32_t)h[bit / 8u + 2u];
        idx[w] = (uint16_t)((v >> (24u - 11u - (bit % 8u))) & 0x7FFu);
    }
    return SC2_OK;
}

int sc2_safety_words(char *out, size_t cap, const uint8_t h[32], char sep)
{
    uint16_t idx[SC2_SAFETY_WORDS];
    if (out == NULL || h == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_safety_word_indices(idx, h);
    size_t o = 0;
    for (unsigned w = 0; w < SC2_SAFETY_WORDS; w++) {
        const char *word = sc2_wordlist_en(idx[w]);
        size_t n = strlen(word);
        if (o + n + (w ? 1u : 0u) + 1u > cap) {
            if (cap) {
                out[0] = '\0';
            }
            return SC2_ERR_BOUNDS;
        }
        if (w) {
            out[o++] = sep;
        }
        memcpy(out + o, word, n);
        o += n;
    }
    out[o] = '\0';
    return SC2_OK;
}

int sc2_fingerprint_short(char out[SC2_SAFETY_SHORT_LEN + 1], const uint8_t ik[SC2_KEY_LEN])
{
    uint8_t h[32];
    if (out == NULL || ik == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_sha256(h, ik, SC2_KEY_LEN);
    if (rc == SC2_OK) {
        groups(out, h, 6, 4);
    }
    return rc;
}
