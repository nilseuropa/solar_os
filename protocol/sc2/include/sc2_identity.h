/*
 * SC2 portable core: device identity, prekeys and auth signatures.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_IDENTITY_H
#define SC2_IDENTITY_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_NO_OPK 0xFFFFFFFFu

/* Long-term device keys: IK (Ed25519, the identity) and DK (X25519, signed by IK). */
typedef struct {
    uint8_t ik_seed[SC2_KEY_LEN];
    uint8_t ik_pub[SC2_KEY_LEN];
    uint8_t dk_priv[SC2_KEY_LEN];
    uint8_t dk_pub[SC2_KEY_LEN];
    uint8_t dk_sig[SC2_SIG_LEN];
} sc2_identity_t;

/* Signed prekey. */
typedef struct {
    uint32_t id;
    uint8_t priv[SC2_KEY_LEN];
    uint8_t pub[SC2_KEY_LEN];
    uint8_t sig[SC2_SIG_LEN];
} sc2_spk_t;

/* One-time prekey (unsigned). */
typedef struct {
    uint32_t id;
    uint8_t priv[SC2_KEY_LEN];
    uint8_t pub[SC2_KEY_LEN];
} sc2_opk_t;

/* RNG order: IK seed (32), DK private (32). */
int sc2_identity_generate(sc2_identity_t *id, const sc2_rng_t *rng);
/* Rebuild public parts and dk_sig from ik_seed and dk_priv. */
int sc2_identity_from_secrets(sc2_identity_t *id, const uint8_t ik_seed[SC2_KEY_LEN],
                              const uint8_t dk_priv[SC2_KEY_LEN]);
/* Replace DK (rotation) with a fresh key from the RNG and re-sign it. */
int sc2_identity_rotate_dk(sc2_identity_t *id, const sc2_rng_t *rng);

/* Device ID = SHA-256(IK_pub)[0:16]. */
int sc2_device_id(uint8_t out[SC2_DEVICE_ID_LEN], const uint8_t ik_pub[SC2_KEY_LEN]);
/* 32 lowercase hex characters plus NUL. */
int sc2_device_id_hex(char out[SC2_DEVICE_ID_HEX_LEN + 1], const uint8_t ik_pub[SC2_KEY_LEN]);

/* dk_sig = Ed25519(IK, "SC2-DK\x01" || dk_pub) */
int sc2_dk_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t ik_seed[SC2_KEY_LEN], const uint8_t dk_pub[SC2_KEY_LEN]);
int sc2_dk_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN], const uint8_t dk_pub[SC2_KEY_LEN]);

/* spk.sig = Ed25519(IK, "SC2-SPK\x01" || id:u32be || spk_pub). RNG: private (32). */
int sc2_spk_generate(sc2_spk_t *spk, uint32_t spk_id, const sc2_identity_t *id, const sc2_rng_t *rng);
int sc2_spk_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN], uint32_t spk_id,
                   const uint8_t spk_pub[SC2_KEY_LEN]);

/* RNG: private (32). opk_id must not be SC2_NO_OPK. */
int sc2_opk_generate(sc2_opk_t *opk, uint32_t opk_id, const sc2_rng_t *rng);

/* Relay auth: Ed25519(IK, "SC2-AUTH\x01" || len:u8 || relay_id || challenge:32 || ik_pub). */
#define SC2_AUTH_CHALLENGE_LEN 32u
#define SC2_RELAY_ID_MAX 255u
int sc2_auth_sign(uint8_t sig[SC2_SIG_LEN], const sc2_identity_t *id, const char *relay_id, size_t relay_id_len,
                  const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN]);
int sc2_auth_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN], const char *relay_id,
                    size_t relay_id_len, const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN]);

/* Wipe all secrets in place. */
void sc2_identity_wipe(sc2_identity_t *id);

#ifdef __cplusplus
}
#endif

#endif
