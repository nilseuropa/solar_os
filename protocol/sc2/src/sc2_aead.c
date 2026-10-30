/*
 * SC2 portable core: XChaCha20-Poly1305 on the WireGuard reference
 * implementation (components/wireguard_lwip/src/crypto/refc,
 * chacha20poly1305.c: HChaCha20 subkey + IETF ChaCha20-Poly1305 with the
 * last 8 nonce bytes; identical to libsodium's _ietf construction).
 * SPDX-License-Identifier: MIT
 */
#include "sc2_crypto.h"

#include "crypto/refc/chacha20poly1305.h"

/* The refc code pads lengths with 32-bit masks; keep inputs well inside. */
#define SC2_AEAD_MAX_LEN 0x0FFFFFFFu

int sc2_aead_seal(uint8_t *out, const uint8_t *pt, size_t len, const uint8_t *ad, size_t ad_len,
                  const uint8_t nonce[SC2_NONCE_LEN], const uint8_t key[SC2_KEY_LEN])
{
    static const uint8_t empty[1] = {0};
    if (out == NULL || nonce == NULL || key == NULL || (pt == NULL && len != 0) || (ad == NULL && ad_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (len > SC2_AEAD_MAX_LEN || ad_len > SC2_AEAD_MAX_LEN) {
        return SC2_ERR_BOUNDS;
    }
    xchacha20poly1305_encrypt(out, len ? pt : empty, len, ad_len ? ad : empty, ad_len, nonce, key);
    return SC2_OK;
}

int sc2_aead_open(uint8_t *out, const uint8_t *ct, size_t ct_len, const uint8_t *ad, size_t ad_len,
                  const uint8_t nonce[SC2_NONCE_LEN], const uint8_t key[SC2_KEY_LEN])
{
    static const uint8_t empty[1] = {0};
    if (ct == NULL || nonce == NULL || key == NULL || (ad == NULL && ad_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (ct_len < SC2_TAG_LEN) {
        return SC2_ERR_AUTH;
    }
    if (out == NULL && ct_len != SC2_TAG_LEN) {
        return SC2_ERR_ARG;
    }
    if (ct_len > SC2_AEAD_MAX_LEN || ad_len > SC2_AEAD_MAX_LEN) {
        return SC2_ERR_BOUNDS;
    }
    uint8_t dummy[1];
    if (!xchacha20poly1305_decrypt(out ? out : dummy, ct, ct_len, ad_len ? ad : empty, ad_len, nonce, key)) {
        return SC2_ERR_AUTH;
    }
    return SC2_OK;
}
