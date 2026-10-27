/*
 * SC2 portable core: X3DH-lite.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_x3dh.h"

#include <string.h>

#include "sc2_crypto.h"
#include "sc2_hash.h"

static const uint8_t LABEL_X3DH[] = {'S', 'C', '2', '-', 'X', '3', 'D', 'H', 0x01};

int sc2_bundle_from_keys(sc2_bundle_t *out, const sc2_identity_t *id, const sc2_spk_t *spk, const sc2_opk_t *opk)
{
    if (out == NULL || id == NULL || spk == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->ik, id->ik_pub, SC2_KEY_LEN);
    memcpy(out->dk, id->dk_pub, SC2_KEY_LEN);
    memcpy(out->dk_sig, id->dk_sig, SC2_SIG_LEN);
    out->spk_id = spk->id;
    memcpy(out->spk, spk->pub, SC2_KEY_LEN);
    memcpy(out->spk_sig, spk->sig, SC2_SIG_LEN);
    out->opk_id = SC2_NO_OPK;
    if (opk) {
        out->opk_id = opk->id;
        memcpy(out->opk, opk->pub, SC2_KEY_LEN);
    }
    return SC2_OK;
}

int sc2_bundle_verify(const sc2_bundle_t *b)
{
    if (b == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_dk_verify(b->dk_sig, b->ik, b->dk);
    if (rc == SC2_OK) {
        rc = sc2_spk_verify(b->spk_sig, b->ik, b->spk_id, b->spk);
    }
    return rc;
}

/* SK = HKDF(0^32, 0xFF^32 || dh[0] || ... , "SC2-X3DH\x01", 32) */
static int derive_sk(uint8_t sk[SC2_KEY_LEN], uint8_t ikm[32 * 5], size_t ndh)
{
    memset(ikm, 0xFF, 32);
    int rc = sc2_hkdf(sk, SC2_KEY_LEN, NULL, 0, ikm, 32u * (ndh + 1u), LABEL_X3DH, sizeof(LABEL_X3DH));
    sc2_memzero(ikm, 32 * 5);
    return rc;
}

int sc2_x3dh_initiate(uint8_t sk[SC2_KEY_LEN], uint8_t ad[SC2_AD_LEN], sc2_x3dh_hdr_t *hdr,
                      const sc2_identity_t *self, const sc2_bundle_t *peer, const sc2_rng_t *rng)
{
    uint8_t ek_priv[SC2_KEY_LEN];
    uint8_t ikm[32 * 5];
    if (sk == NULL || ad == NULL || hdr == NULL || self == NULL || peer == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_bundle_verify(peer);
    if (rc != SC2_OK) {
        return rc;
    }
    sc2_x3dh_hdr_t h;
    rc = sc2_x25519_generate(ek_priv, h.ek, rng);
    if (rc != SC2_OK) {
        return rc;
    }
    h.spk_id = peer->spk_id;
    h.opk_id = peer->opk_id;
    size_t ndh = 3;
    rc = sc2_x25519(ikm + 32, self->dk_priv, peer->spk);
    if (rc == SC2_OK) {
        rc = sc2_x25519(ikm + 64, ek_priv, peer->dk);
    }
    if (rc == SC2_OK) {
        rc = sc2_x25519(ikm + 96, ek_priv, peer->spk);
    }
    if (rc == SC2_OK && peer->opk_id != SC2_NO_OPK) {
        rc = sc2_x25519(ikm + 128, ek_priv, peer->opk);
        ndh = 4;
    }
    sc2_memzero(ek_priv, sizeof(ek_priv));
    if (rc != SC2_OK) {
        sc2_memzero(ikm, sizeof(ikm));
        return rc;
    }
    rc = derive_sk(sk, ikm, ndh);
    if (rc != SC2_OK) {
        return rc;
    }
    memcpy(ad, self->ik_pub, SC2_KEY_LEN);
    memcpy(ad + SC2_KEY_LEN, peer->ik, SC2_KEY_LEN);
    *hdr = h;
    return SC2_OK;
}

int sc2_x3dh_respond(uint8_t sk[SC2_KEY_LEN], uint8_t ad[SC2_AD_LEN], const sc2_identity_t *self,
                     const sc2_spk_t *spk, const sc2_opk_t *opk, const uint8_t peer_ik[SC2_KEY_LEN],
                     const uint8_t peer_dk[SC2_KEY_LEN], const uint8_t peer_dk_sig[SC2_SIG_LEN],
                     const sc2_x3dh_hdr_t *hdr)
{
    uint8_t ikm[32 * 5];
    if (sk == NULL || ad == NULL || self == NULL || spk == NULL || peer_ik == NULL || peer_dk == NULL ||
        peer_dk_sig == NULL || hdr == NULL) {
        return SC2_ERR_ARG;
    }
    if (hdr->spk_id != spk->id) {
        return SC2_ERR_STATE;
    }
    if (hdr->opk_id == SC2_NO_OPK) {
        if (opk != NULL) {
            return SC2_ERR_ARG;
        }
    } else if (opk == NULL || opk->id != hdr->opk_id) {
        return SC2_ERR_STATE;
    }
    int rc = sc2_dk_verify(peer_dk_sig, peer_ik, peer_dk);
    if (rc != SC2_OK) {
        return rc;
    }
    size_t ndh = 3;
    rc = sc2_x25519(ikm + 32, spk->priv, peer_dk);
    if (rc == SC2_OK) {
        rc = sc2_x25519(ikm + 64, self->dk_priv, hdr->ek);
    }
    if (rc == SC2_OK) {
        rc = sc2_x25519(ikm + 96, spk->priv, hdr->ek);
    }
    if (rc == SC2_OK && opk != NULL) {
        rc = sc2_x25519(ikm + 128, opk->priv, hdr->ek);
        ndh = 4;
    }
    if (rc != SC2_OK) {
        sc2_memzero(ikm, sizeof(ikm));
        return rc;
    }
    rc = derive_sk(sk, ikm, ndh);
    if (rc != SC2_OK) {
        return rc;
    }
    memcpy(ad, peer_ik, SC2_KEY_LEN);
    memcpy(ad + SC2_KEY_LEN, self->ik_pub, SC2_KEY_LEN);
    return SC2_OK;
}
