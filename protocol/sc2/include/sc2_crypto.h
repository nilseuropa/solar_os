/*
 * SC2 portable core: public-key and AEAD primitives.
 *
 * Ed25519 reuses the orlp sources in src/vendor/meshcore/ed25519 and adds
 * the strict checks libsodium applies (canonical S, canonical and
 * non-small-order public key and R). X25519 and XChaCha20-Poly1305 reuse
 * the WireGuard reference code in components/wireguard_lwip/src/crypto/refc.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_CRYPTO_H
#define SC2_CRYPTO_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ed25519 (identity keys). Secret keys are stored as 32-byte seeds. ---- */

/* Derive the public key from a 32-byte seed (RFC 8032 secret key). */
int sc2_ed25519_public(uint8_t pub[SC2_KEY_LEN], const uint8_t seed[SC2_KEY_LEN]);

/* Generate a seed from the RNG and derive its public key. */
int sc2_ed25519_generate(uint8_t seed[SC2_KEY_LEN], uint8_t pub[SC2_KEY_LEN], const sc2_rng_t *rng);

/* Deterministic RFC 8032 signature over the concatenation of parts. */
int sc2_ed25519_sign_parts(uint8_t sig[SC2_SIG_LEN], const sc2_part_t *parts, size_t count,
                           const uint8_t seed[SC2_KEY_LEN]);
int sc2_ed25519_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t *msg, size_t len,
                     const uint8_t seed[SC2_KEY_LEN]);

/* Strict verification. Returns SC2_OK, SC2_ERR_KEY (unusable public key)
 * or SC2_ERR_AUTH (bad or non-canonical signature). */
int sc2_ed25519_verify_parts(const uint8_t sig[SC2_SIG_LEN], const sc2_part_t *parts, size_t count,
                             const uint8_t pub[SC2_KEY_LEN]);
int sc2_ed25519_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t *msg, size_t len,
                       const uint8_t pub[SC2_KEY_LEN]);

/* SC2_OK if pub is a canonical encoding of a point that is not of small order. */
int sc2_ed25519_check_public(const uint8_t pub[SC2_KEY_LEN]);

/* ---- X25519 (DH keys). Private keys are 32 random bytes, clamped on use. ---- */

int sc2_x25519_public(uint8_t pub[SC2_KEY_LEN], const uint8_t priv[SC2_KEY_LEN]);
int sc2_x25519_generate(uint8_t priv[SC2_KEY_LEN], uint8_t pub[SC2_KEY_LEN], const sc2_rng_t *rng);

/* Shared secret. The top bit of peer is masked (RFC 7748). Returns
 * SC2_ERR_KEY, with out zeroed, when the result is all zero. */
int sc2_x25519(uint8_t out[SC2_KEY_LEN], const uint8_t priv[SC2_KEY_LEN], const uint8_t peer[SC2_KEY_LEN]);

/* ---- XChaCha20-Poly1305 (IETF construction, as libsodium _ietf). ---- */

/* out receives len + 16 bytes. out may equal pt (in place). */
int sc2_aead_seal(uint8_t *out, const uint8_t *pt, size_t len, const uint8_t *ad, size_t ad_len,
                  const uint8_t nonce[SC2_NONCE_LEN], const uint8_t key[SC2_KEY_LEN]);

/* out receives ct_len - 16 bytes. out may equal ct. Returns SC2_ERR_AUTH
 * on a bad tag; out is then left untouched. */
int sc2_aead_open(uint8_t *out, const uint8_t *ct, size_t ct_len, const uint8_t *ad, size_t ad_len,
                  const uint8_t nonce[SC2_NONCE_LEN], const uint8_t key[SC2_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif
