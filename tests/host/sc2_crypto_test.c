/*
 * SC2 host test: hashing, HKDF, Ed25519 (strict), X25519, XChaCha20-Poly1305
 * and codecs against RFC and libsodium (PyNaCl) vectors.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_test_util.h"
#include "sc2_vectors.inc"

static void test_sha256(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_SHA256); i++) {
        size_t n;
        uint8_t *msg = sc2t_hex(SC2V_SHA256[i].msg, &n);
        uint8_t want[32], got[32];
        sc2t_hexn(want, 32, SC2V_SHA256[i].digest);
        CHECK_RC(sc2_sha256(got, msg, n), SC2_OK);
        CHECK_MEM(got, want, 32);
        /* byte-at-a-time and odd splits */
        sc2_sha256_ctx ctx;
        CHECK_RC(sc2_sha256_init(&ctx), SC2_OK);
        for (size_t k = 0; k < n; k++) {
            CHECK_RC(sc2_sha256_update(&ctx, msg + k, 1), SC2_OK);
        }
        CHECK_RC(sc2_sha256_final(&ctx, got), SC2_OK);
        CHECK_MEM(got, want, 32);
        size_t cut = n / 3u;
        CHECK_RC(sc2_sha256_init(&ctx), SC2_OK);
        CHECK_RC(sc2_sha256_update(&ctx, msg, cut), SC2_OK);
        CHECK_RC(sc2_sha256_update(&ctx, msg + cut, 0), SC2_OK);
        CHECK_RC(sc2_sha256_update(&ctx, msg + cut, n - cut), SC2_OK);
        CHECK_RC(sc2_sha256_final(&ctx, got), SC2_OK);
        CHECK_MEM(got, want, 32);
        free(msg);
    }
    /* one million 'a' (FIPS 180-2) */
    static uint8_t block[1000];
    memset(block, 'a', sizeof(block));
    sc2_sha256_ctx ctx;
    uint8_t got[32], want[32];
    sc2t_hexn(want, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    sc2_sha256_init(&ctx);
    for (int i = 0; i < 1000; i++) {
        sc2_sha256_update(&ctx, block, sizeof(block));
    }
    sc2_sha256_final(&ctx, got);
    CHECK_MEM(got, want, 32);
    CHECK_RC(sc2_sha256(NULL, block, 1), SC2_ERR_ARG);
}

static void test_hmac_hkdf(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_HMAC); i++) {
        size_t kl, ml;
        uint8_t *key = sc2t_hex(SC2V_HMAC[i].key, &kl);
        uint8_t *msg = sc2t_hex(SC2V_HMAC[i].msg, &ml);
        uint8_t want[32], got[32];
        sc2t_hexn(want, 32, SC2V_HMAC[i].mac);
        CHECK_RC(sc2_hmac_sha256(got, key, kl, msg, ml), SC2_OK);
        CHECK_MEM(got, want, 32);
        sc2_hmac_ctx h;
        CHECK_RC(sc2_hmac_init(&h, key, kl), SC2_OK);
        CHECK_RC(sc2_hmac_update(&h, msg, ml / 2u), SC2_OK);
        CHECK_RC(sc2_hmac_update(&h, msg + ml / 2u, ml - ml / 2u), SC2_OK);
        CHECK_RC(sc2_hmac_final(&h, got), SC2_OK);
        CHECK_MEM(got, want, 32);
        free(key);
        free(msg);
    }
    for (size_t i = 0; i < SC2V_COUNT(SC2V_HKDF); i++) {
        size_t il, sl, nl, ol;
        uint8_t *ikm = sc2t_hex(SC2V_HKDF[i].ikm, &il);
        uint8_t *salt = sc2t_hex(SC2V_HKDF[i].salt, &sl);
        uint8_t *info = sc2t_hex(SC2V_HKDF[i].info, &nl);
        uint8_t *okm = sc2t_hex(SC2V_HKDF[i].okm, &ol);
        uint8_t prk[32], want_prk[32];
        sc2t_hexn(want_prk, 32, SC2V_HKDF[i].prk);
        CHECK(ol == SC2V_HKDF[i].len);
        CHECK_RC(sc2_hkdf_extract(prk, sl ? salt : NULL, sl, ikm, il), SC2_OK);
        CHECK_MEM(prk, want_prk, 32);
        uint8_t *out = (uint8_t *)malloc(ol);
        CHECK_RC(sc2_hkdf(out, ol, salt, sl, ikm, il, info, nl), SC2_OK);
        CHECK_MEM(out, okm, ol);
        /* parts API: split info in two */
        sc2_part_t parts[2] = {{info, nl / 2u}, {info + nl / 2u, nl - nl / 2u}};
        memset(out, 0, ol);
        CHECK_RC(sc2_hkdf_expand_parts(out, ol, prk, 32, parts, 2), SC2_OK);
        CHECK_MEM(out, okm, ol);
        free(out);
        free(ikm);
        free(salt);
        free(info);
        free(okm);
    }
    uint8_t prk[32] = {0}, out[8];
    CHECK_RC(sc2_hkdf_expand(out, 0, prk, 32, NULL, 0), SC2_ERR_ARG);
    CHECK_RC(sc2_hkdf_expand(out, SC2_HKDF_MAX_OUT + 1u, prk, 32, NULL, 0), SC2_ERR_ARG);
    CHECK_RC(sc2_hkdf_expand(out, 8, prk, 16, NULL, 0), SC2_ERR_ARG);
}

static void test_ed25519(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_ED_SIGN); i++) {
        size_t ml;
        uint8_t seed[32], pk[32], sig[64], got_pk[32], got_sig[64];
        sc2t_hexn(seed, 32, SC2V_ED_SIGN[i].seed);
        sc2t_hexn(pk, 32, SC2V_ED_SIGN[i].pk);
        sc2t_hexn(sig, 64, SC2V_ED_SIGN[i].sig);
        uint8_t *msg = sc2t_hex(SC2V_ED_SIGN[i].msg, &ml);
        CHECK_RC(sc2_ed25519_public(got_pk, seed), SC2_OK);
        CHECK_MEM(got_pk, pk, 32);
        CHECK_RC(sc2_ed25519_sign(got_sig, msg, ml, seed), SC2_OK);
        CHECK_MEM(got_sig, sig, 64);
        CHECK_RC(sc2_ed25519_verify(sig, msg, ml, pk), SC2_OK);
        CHECK_RC(sc2_ed25519_check_public(pk), SC2_OK);
        /* multi-part signing equals single-buffer signing */
        size_t cut = ml / 2u;
        sc2_part_t parts[3] = {{msg, cut}, {NULL, 0}, {msg + cut, ml - cut}};
        CHECK_RC(sc2_ed25519_sign_parts(got_sig, parts, 3, seed), SC2_OK);
        CHECK_MEM(got_sig, sig, 64);
        CHECK_RC(sc2_ed25519_verify_parts(sig, parts, 3, pk), SC2_OK);
        free(msg);
    }
    int rejected = 0;
    for (size_t i = 0; i < SC2V_COUNT(SC2V_ED_VERIFY); i++) {
        const sc2v_ed_verify_t *v = &SC2V_ED_VERIFY[i];
        size_t ml;
        uint8_t pk[32], sig[64];
        sc2t_hexn(pk, 32, v->pk);
        sc2t_hexn(sig, 64, v->sig);
        uint8_t *msg = sc2t_hex(v->msg, &ml);
        int rc = sc2_ed25519_verify(sig, msg, ml, pk);
        if ((rc == SC2_OK) != (v->ok != 0)) {
            fprintf(stderr, "ed25519 strict case '%s': rc %d, libsodium says %s\n", v->note, rc,
                    v->ok ? "valid" : "invalid");
        }
        CHECK((rc == SC2_OK) == (v->ok != 0));
        if (!v->ok) {
            rejected++;
            CHECK(rc == SC2_ERR_AUTH || rc == SC2_ERR_KEY);
        }
        free(msg);
    }
    CHECK(rejected == (int)SC2V_COUNT(SC2V_ED_VERIFY) - 1);
    /* RNG plumbing */
    uint8_t seed[32], pk[32];
    sc2_rng_t bad = {sc2t_fail_fn, NULL};
    CHECK_RC(sc2_ed25519_generate(seed, pk, &bad), SC2_ERR_RNG);
    CHECK_RC(sc2_ed25519_generate(seed, pk, NULL), SC2_ERR_RNG);
    sc2_rng_t good = {sc2t_xs_fn, NULL};
    CHECK_RC(sc2_ed25519_generate(seed, pk, &good), SC2_OK);
    uint8_t sig[64];
    CHECK_RC(sc2_ed25519_sign(sig, (const uint8_t *)"x", 1, seed), SC2_OK);
    CHECK_RC(sc2_ed25519_verify(sig, (const uint8_t *)"x", 1, pk), SC2_OK);
    CHECK_RC(sc2_ed25519_verify(sig, (const uint8_t *)"y", 1, pk), SC2_ERR_AUTH);
}

static void test_x25519(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_X25519); i++) {
        uint8_t k[32], u[32], want[32], got[32];
        sc2t_hexn(k, 32, SC2V_X25519[i].scalar);
        sc2t_hexn(u, 32, SC2V_X25519[i].u);
        sc2t_hexn(want, 32, SC2V_X25519[i].out);
        CHECK_RC(sc2_x25519(got, k, u), SC2_OK);
        CHECK_MEM(got, want, 32);
    }
    for (size_t i = 0; i < SC2V_COUNT(SC2V_X25519_BAD); i++) {
        uint8_t k[32], u[32], got[32];
        sc2t_hexn(k, 32, SC2V_X25519_BAD[i].scalar);
        sc2t_hexn(u, 32, SC2V_X25519_BAD[i].u);
        memset(got, 0xAA, sizeof(got));
        CHECK_RC(sc2_x25519(got, k, u), SC2_ERR_KEY);
        CHECK(sc2_ct_is_zero(got, 32));
    }
    uint8_t k[32] = {9}, u[32] = {9}, t[32], want[32];
    for (int i = 1; i <= 1000; i++) {
        CHECK_RC(sc2_x25519(t, k, u), SC2_OK);
        memcpy(u, k, 32);
        memcpy(k, t, 32);
        if (i == 1) {
            sc2t_hexn(want, 32, SC2V_X25519_ITER_1);
            CHECK_MEM(k, want, 32);
        }
    }
    sc2t_hexn(want, 32, SC2V_X25519_ITER_1000);
    CHECK_MEM(k, want, 32);
    /* DH agreement with generated keys */
    sc2_rng_t rng = {sc2t_xs_fn, NULL};
    uint8_t a[32], ap[32], b[32], bp[32], s1[32], s2[32];
    CHECK_RC(sc2_x25519_generate(a, ap, &rng), SC2_OK);
    CHECK_RC(sc2_x25519_generate(b, bp, &rng), SC2_OK);
    CHECK_RC(sc2_x25519(s1, a, bp), SC2_OK);
    CHECK_RC(sc2_x25519(s2, b, ap), SC2_OK);
    CHECK_MEM(s1, s2, 32);
}

static void test_aead(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_AEAD); i++) {
        const sc2v_aead_t *v = &SC2V_AEAD[i];
        uint8_t key[32], nonce[24];
        size_t al, pl, cl;
        sc2t_hexn(key, 32, v->key);
        sc2t_hexn(nonce, 24, v->nonce);
        uint8_t *ad = sc2t_hex(v->ad, &al);
        uint8_t *pt = sc2t_hex(v->pt, &pl);
        uint8_t *ct = sc2t_hex(v->ct, &cl);
        CHECK(cl == pl + 16u);
        uint8_t *buf = (uint8_t *)malloc(cl + 1u);
        CHECK_RC(sc2_aead_seal(buf, pt, pl, ad, al, nonce, key), SC2_OK);
        CHECK_MEM(buf, ct, cl);
        memset(buf, 0, cl);
        CHECK_RC(sc2_aead_open(buf, ct, cl, ad, al, nonce, key), SC2_OK);
        CHECK_MEM(buf, pt, pl);
        /* in place */
        memcpy(buf, pt, pl);
        CHECK_RC(sc2_aead_seal(buf, buf, pl, ad, al, nonce, key), SC2_OK);
        CHECK_MEM(buf, ct, cl);
        CHECK_RC(sc2_aead_open(buf, buf, cl, ad, al, nonce, key), SC2_OK);
        CHECK_MEM(buf, pt, pl);
        /* tampering: ciphertext, tag, AD, nonce */
        uint8_t *bad = (uint8_t *)malloc(cl);
        memcpy(bad, ct, cl);
        bad[cl - 1u] ^= 1u;
        CHECK_RC(sc2_aead_open(buf, bad, cl, ad, al, nonce, key), SC2_ERR_AUTH);
        if (pl) {
            memcpy(bad, ct, cl);
            bad[0] ^= 0x80u;
            CHECK_RC(sc2_aead_open(buf, bad, cl, ad, al, nonce, key), SC2_ERR_AUTH);
        }
        if (al) {
            ad[al - 1u] ^= 1u;
            CHECK_RC(sc2_aead_open(buf, ct, cl, ad, al, nonce, key), SC2_ERR_AUTH);
            ad[al - 1u] ^= 1u;
        }
        nonce[23] ^= 1u;
        CHECK_RC(sc2_aead_open(buf, ct, cl, ad, al, nonce, key), SC2_ERR_AUTH);
        free(bad);
        free(buf);
        free(ad);
        free(pt);
        free(ct);
    }
    uint8_t key[32] = {0}, nonce[24] = {0}, tiny[15] = {0}, out[16];
    CHECK_RC(sc2_aead_open(out, tiny, sizeof(tiny), NULL, 0, nonce, key), SC2_ERR_AUTH);
}

static void test_codecs(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_B64); i++) {
        size_t bl;
        uint8_t *bin = sc2t_hex(SC2V_B64[i].bin, &bl);
        char enc[64];
        size_t el = 0;
        CHECK_RC(sc2_b64_encode(enc, sizeof(enc), &el, bin, bl), SC2_OK);
        CHECK(strcmp(enc, SC2V_B64[i].b64) == 0 && el == strlen(enc));
        uint8_t dec[64];
        size_t dl = 0;
        CHECK_RC(sc2_b64_decode(dec, sizeof(dec), &dl, SC2V_B64[i].b64, strlen(SC2V_B64[i].b64)), SC2_OK);
        CHECK(dl == bl && memcmp(dec, bin, bl) == 0);
        if (bl) {
            CHECK_RC(sc2_b64_decode(dec, bl - 1u, &dl, SC2V_B64[i].b64, strlen(SC2V_B64[i].b64)), SC2_ERR_BOUNDS);
        }
        CHECK_RC(sc2_b64_encode(enc, el, &el, bin, bl), SC2_ERR_BOUNDS);
        free(bin);
    }
    for (size_t i = 0; i < SC2V_COUNT(SC2V_B64_BAD); i++) {
        uint8_t dec[64];
        size_t dl;
        int rc = sc2_b64_decode(dec, sizeof(dec), &dl, SC2V_B64_BAD[i], strlen(SC2V_B64_BAD[i]));
        if (rc != SC2_ERR_FORMAT) {
            fprintf(stderr, "b64 accepted '%s'\n", SC2V_B64_BAD[i]);
        }
        CHECK(rc == SC2_ERR_FORMAT);
    }
    /* hex */
    uint8_t raw[4] = {0x00, 0xAB, 0x7f, 0xFF}, back[4];
    char hx[9];
    CHECK_RC(sc2_hex_encode(hx, sizeof(hx), raw, 4), SC2_OK);
    CHECK(strcmp(hx, "00ab7fff") == 0);
    CHECK_RC(sc2_hex_decode(back, 4, "00AB7fFf", 8), SC2_OK);
    CHECK_MEM(back, raw, 4);
    CHECK_RC(sc2_hex_decode(back, 4, "00ab7fg0", 8), SC2_ERR_FORMAT);
    CHECK_RC(sc2_hex_decode(back, 4, "00ab7f", 6), SC2_ERR_FORMAT);
    CHECK_RC(sc2_hex_encode(hx, 8, raw, 4), SC2_ERR_BOUNDS);
    /* padding buckets */
    static const size_t lens[] = {0, 1, 254, 255, 256, 511, 512, 1023, 2047, 4095, 4096, 8190, 8191};
    static const size_t buckets[] = {256, 256, 256, 256, 512, 512, 1024, 1024, 2048, 4096, 8192, 8192, 8192};
    static uint8_t in[8192], out[8192];
    for (size_t i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)(i * 7u + 1u);
    }
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        size_t pl = 0, ul = 0;
        CHECK(sc2_pad_bucket(lens[i]) == buckets[i]);
        CHECK_RC(sc2_pad(out, sizeof(out), &pl, in, lens[i]), SC2_OK);
        CHECK(pl == buckets[i]);
        CHECK(out[lens[i]] == 0x80);
        CHECK_RC(sc2_unpad(out, pl, &ul), SC2_OK);
        CHECK(ul == lens[i] && memcmp(out, in, ul) == 0);
    }
    size_t pl;
    CHECK(sc2_pad_bucket(8192) == 0);
    CHECK_RC(sc2_pad(out, sizeof(out), &pl, in, 8192), SC2_ERR_BOUNDS);
    CHECK_RC(sc2_pad(out, 255, &pl, in, 10), SC2_ERR_BOUNDS);
    memset(out, 0, 256);
    CHECK_RC(sc2_unpad(out, 256, &pl), SC2_ERR_FORMAT);   /* no marker */
    out[10] = 0x80;
    CHECK_RC(sc2_unpad(out, 300, &pl), SC2_ERR_FORMAT);   /* not a bucket */
    memset(out, 0, 512);
    out[10] = 0x80;
    CHECK_RC(sc2_unpad(out, 512, &pl), SC2_ERR_FORMAT);   /* not the minimal bucket */
    out[10] = 0x81;
    CHECK_RC(sc2_unpad(out, 256, &pl), SC2_ERR_FORMAT);
    /* helpers */
    CHECK(sc2_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(sc2_ct_eq("abc", "abc", 3) == 1 && sc2_ct_eq("abc", "abd", 3) == 0 && sc2_ct_eq("", "", 0) == 1);
    uint8_t z[5] = {0};
    CHECK(sc2_ct_is_zero(z, 5) == 1);
    z[4] = 1;
    CHECK(sc2_ct_is_zero(z, 5) == 0);
    CHECK(strcmp(sc2_strerror(SC2_ERR_AUTH), "authentication failed") == 0);
}

int main(void)
{
    test_sha256();
    test_hmac_hkdf();
    test_ed25519();
    test_x25519();
    test_aead();
    test_codecs();
    return sc2t_report("sc2_crypto_test");
}
