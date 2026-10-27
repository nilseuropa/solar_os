/*
 * SC2 portable core: identity keys, prekeys and auth signatures.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_identity.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"
#include "sc2_hash.h"

static const uint8_t LABEL_DK[] = {'S', 'C', '2', '-', 'D', 'K', 0x01};
static const uint8_t LABEL_SPK[] = {'S', 'C', '2', '-', 'S', 'P', 'K', 0x01};
static const uint8_t LABEL_AUTH[] = {'S', 'C', '2', '-', 'A', 'U', 'T', 'H', 0x01};

int sc2_device_id(uint8_t out[SC2_DEVICE_ID_LEN], const uint8_t ik_pub[SC2_KEY_LEN])
{
    uint8_t h[SC2_SHA256_LEN];
    if (out == NULL || ik_pub == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_sha256(h, ik_pub, SC2_KEY_LEN);
    if (rc == SC2_OK) {
        memcpy(out, h, SC2_DEVICE_ID_LEN);
    }
    return rc;
}

int sc2_device_id_hex(char out[SC2_DEVICE_ID_HEX_LEN + 1], const uint8_t ik_pub[SC2_KEY_LEN])
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    if (out == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_device_id(id, ik_pub);
    if (rc != SC2_OK) {
        return rc;
    }
    return sc2_hex_encode(out, SC2_DEVICE_ID_HEX_LEN + 1, id, sizeof(id));
}

int sc2_dk_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t ik_seed[SC2_KEY_LEN], const uint8_t dk_pub[SC2_KEY_LEN])
{
    if (dk_pub == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_part_t parts[2] = {{LABEL_DK, sizeof(LABEL_DK)}, {dk_pub, SC2_KEY_LEN}};
    return sc2_ed25519_sign_parts(sig, parts, 2, ik_seed);
}

int sc2_dk_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN],
                  const uint8_t dk_pub[SC2_KEY_LEN])
{
    if (dk_pub == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_part_t parts[2] = {{LABEL_DK, sizeof(LABEL_DK)}, {dk_pub, SC2_KEY_LEN}};
    return sc2_ed25519_verify_parts(sig, parts, 2, ik_pub);
}

static void spk_parts(sc2_part_t parts[3], uint8_t idbuf[4], uint32_t spk_id, const uint8_t *spk_pub)
{
    sc2_store_be32(idbuf, spk_id);
    parts[0].ptr = LABEL_SPK;
    parts[0].len = sizeof(LABEL_SPK);
    parts[1].ptr = idbuf;
    parts[1].len = 4;
    parts[2].ptr = spk_pub;
    parts[2].len = SC2_KEY_LEN;
}

int sc2_spk_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN], uint32_t spk_id,
                   const uint8_t spk_pub[SC2_KEY_LEN])
{
    sc2_part_t parts[3];
    uint8_t idbuf[4];
    if (spk_pub == NULL) {
        return SC2_ERR_ARG;
    }
    spk_parts(parts, idbuf, spk_id, spk_pub);
    return sc2_ed25519_verify_parts(sig, parts, 3, ik_pub);
}

int sc2_spk_generate(sc2_spk_t *spk, uint32_t spk_id, const sc2_identity_t *id, const sc2_rng_t *rng)
{
    sc2_part_t parts[3];
    uint8_t idbuf[4];
    if (spk == NULL || id == NULL) {
        return SC2_ERR_ARG;
    }
    spk->id = spk_id;
    int rc = sc2_x25519_generate(spk->priv, spk->pub, rng);
    if (rc != SC2_OK) {
        sc2_memzero(spk, sizeof(*spk));
        return rc;
    }
    spk_parts(parts, idbuf, spk_id, spk->pub);
    rc = sc2_ed25519_sign_parts(spk->sig, parts, 3, id->ik_seed);
    if (rc != SC2_OK) {
        sc2_memzero(spk, sizeof(*spk));
    }
    return rc;
}

int sc2_opk_generate(sc2_opk_t *opk, uint32_t opk_id, const sc2_rng_t *rng)
{
    if (opk == NULL || opk_id == SC2_NO_OPK) {
        return SC2_ERR_ARG;
    }
    opk->id = opk_id;
    int rc = sc2_x25519_generate(opk->priv, opk->pub, rng);
    if (rc != SC2_OK) {
        sc2_memzero(opk, sizeof(*opk));
    }
    return rc;
}

int sc2_identity_from_secrets(sc2_identity_t *id, const uint8_t ik_seed[SC2_KEY_LEN],
                              const uint8_t dk_priv[SC2_KEY_LEN])
{
    if (id == NULL || ik_seed == NULL || dk_priv == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_identity_t tmp;
    memcpy(tmp.ik_seed, ik_seed, SC2_KEY_LEN);
    memcpy(tmp.dk_priv, dk_priv, SC2_KEY_LEN);
    int rc = sc2_ed25519_public(tmp.ik_pub, tmp.ik_seed);
    if (rc == SC2_OK) {
        rc = sc2_x25519_public(tmp.dk_pub, tmp.dk_priv);
    }
    if (rc == SC2_OK) {
        rc = sc2_dk_sign(tmp.dk_sig, tmp.ik_seed, tmp.dk_pub);
    }
    if (rc == SC2_OK) {
        memcpy(id, &tmp, sizeof(tmp));
    }
    sc2_memzero(&tmp, sizeof(tmp));
    return rc;
}

int sc2_identity_generate(sc2_identity_t *id, const sc2_rng_t *rng)
{
    uint8_t seed[SC2_KEY_LEN];
    uint8_t dk[SC2_KEY_LEN];
    if (id == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_random(rng, seed, sizeof(seed));
    if (rc == SC2_OK) {
        rc = sc2_random(rng, dk, sizeof(dk));
    }
    if (rc == SC2_OK) {
        rc = sc2_identity_from_secrets(id, seed, dk);
    }
    sc2_memzero(seed, sizeof(seed));
    sc2_memzero(dk, sizeof(dk));
    return rc;
}

int sc2_identity_rotate_dk(sc2_identity_t *id, const sc2_rng_t *rng)
{
    uint8_t dk[SC2_KEY_LEN];
    if (id == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_random(rng, dk, sizeof(dk));
    if (rc == SC2_OK) {
        rc = sc2_identity_from_secrets(id, id->ik_seed, dk);
    }
    sc2_memzero(dk, sizeof(dk));
    return rc;
}

int sc2_auth_sign(uint8_t sig[SC2_SIG_LEN], const sc2_identity_t *id, const char *relay_id, size_t relay_id_len,
                  const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN])
{
    if (id == NULL || relay_id == NULL || challenge == NULL || relay_id_len == 0 ||
        relay_id_len > SC2_RELAY_ID_MAX) {
        return SC2_ERR_ARG;
    }
    uint8_t len8 = (uint8_t)relay_id_len;
    sc2_part_t parts[5] = {{LABEL_AUTH, sizeof(LABEL_AUTH)},
                           {&len8, 1},
                           {(const uint8_t *)relay_id, relay_id_len},
                           {challenge, SC2_AUTH_CHALLENGE_LEN},
                           {id->ik_pub, SC2_KEY_LEN}};
    return sc2_ed25519_sign_parts(sig, parts, 5, id->ik_seed);
}

int sc2_auth_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN], const char *relay_id,
                    size_t relay_id_len, const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN])
{
    if (ik_pub == NULL || relay_id == NULL || challenge == NULL || relay_id_len == 0 ||
        relay_id_len > SC2_RELAY_ID_MAX) {
        return SC2_ERR_ARG;
    }
    uint8_t len8 = (uint8_t)relay_id_len;
    sc2_part_t parts[5] = {{LABEL_AUTH, sizeof(LABEL_AUTH)},
                           {&len8, 1},
                           {(const uint8_t *)relay_id, relay_id_len},
                           {challenge, SC2_AUTH_CHALLENGE_LEN},
                           {ik_pub, SC2_KEY_LEN}};
    return sc2_ed25519_verify_parts(sig, parts, 5, ik_pub);
}

void sc2_identity_wipe(sc2_identity_t *id)
{
    if (id) {
        sc2_memzero(id, sizeof(*id));
    }
}
