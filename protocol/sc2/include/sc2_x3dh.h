/*
 * SC2 portable core: X3DH-lite key agreement.
 *
 *   DH1 = X25519(DK_A, SPK_B)  DH2 = X25519(EK_A, DK_B)
 *   DH3 = X25519(EK_A, SPK_B)  DH4 = X25519(EK_A, OPK_B)  (optional)
 *   SK  = HKDF(salt = 0^32, IKM = 0xFF^32 || DH1 || DH2 || DH3 [|| DH4],
 *              info = "SC2-X3DH\x01", L = 32)
 *   AD  = IK_A || IK_B
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_X3DH_H
#define SC2_X3DH_H

#include "sc2_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_AD_LEN 64u

/* A peer's published bundle as returned by keys.bundle. */
typedef struct {
    uint8_t ik[SC2_KEY_LEN];
    uint8_t dk[SC2_KEY_LEN];
    uint8_t dk_sig[SC2_SIG_LEN];
    uint32_t spk_id;
    uint8_t spk[SC2_KEY_LEN];
    uint8_t spk_sig[SC2_SIG_LEN];
    uint32_t opk_id; /* SC2_NO_OPK when absent */
    uint8_t opk[SC2_KEY_LEN];
} sc2_bundle_t;

/* X3DH fields carried in the first DM envelope(s). */
typedef struct {
    uint8_t ek[SC2_KEY_LEN];
    uint32_t spk_id;
    uint32_t opk_id; /* SC2_NO_OPK when none was used */
} sc2_x3dh_hdr_t;

/* Fill a bundle from own keys (opk may be NULL). */
int sc2_bundle_from_keys(sc2_bundle_t *out, const sc2_identity_t *id, const sc2_spk_t *spk, const sc2_opk_t *opk);

/* Verify dk_sig and spk_sig strictly; SC2_ERR_KEY / SC2_ERR_AUTH on failure. */
int sc2_bundle_verify(const sc2_bundle_t *bundle);

/* Initiator. Verifies the bundle first. RNG order: EK private (32). */
int sc2_x3dh_initiate(uint8_t sk[SC2_KEY_LEN], uint8_t ad[SC2_AD_LEN], sc2_x3dh_hdr_t *hdr,
                      const sc2_identity_t *self, const sc2_bundle_t *peer, const sc2_rng_t *rng);

/* Responder. peer_dk_sig is checked against peer_ik. opk must match
 * hdr->opk_id (NULL when hdr->opk_id == SC2_NO_OPK). */
int sc2_x3dh_respond(uint8_t sk[SC2_KEY_LEN], uint8_t ad[SC2_AD_LEN], const sc2_identity_t *self,
                     const sc2_spk_t *spk, const sc2_opk_t *opk, const uint8_t peer_ik[SC2_KEY_LEN],
                     const uint8_t peer_dk[SC2_KEY_LEN], const uint8_t peer_dk_sig[SC2_SIG_LEN],
                     const sc2_x3dh_hdr_t *hdr);

#ifdef __cplusplus
}
#endif

#endif
