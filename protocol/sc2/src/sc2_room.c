/*
 * SC2 portable core: private-room sender chains and sealed room messages.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_room.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"
#include "sc2_hash.h"
#include "sc2_ratchet.h"

static const uint8_t LABEL_SENDER[] = {'S', 'C', '2', '-', 'S', 'E', 'N', 'D', 'E', 'R', 0x01};
static const uint8_t LABEL_ROOM[] = {'S', 'C', '2', '-', 'R', 'O', 'O', 'M', 0x01};
static const uint8_t MAGIC_ROOM[4] = {'S', 'C', '2', 'R'};

int sc2_room_chain_init(sc2_room_chain_t *chain, const uint8_t epoch_key[SC2_KEY_LEN],
                        const uint8_t room[SC2_ROOM_ID_LEN], uint32_t epoch,
                        const uint8_t sender[SC2_DEVICE_ID_LEN])
{
    uint8_t salt[SC2_ROOM_ID_LEN + 4];
    uint8_t prk[SC2_SHA256_LEN];
    if (chain == NULL || epoch_key == NULL || room == NULL || sender == NULL) {
        return SC2_ERR_ARG;
    }
    memcpy(salt, room, SC2_ROOM_ID_LEN);
    sc2_store_be32(salt + SC2_ROOM_ID_LEN, epoch);
    sc2_part_t info[2] = {{LABEL_SENDER, sizeof(LABEL_SENDER)}, {sender, SC2_DEVICE_ID_LEN}};
    int rc = sc2_hkdf_extract(prk, salt, sizeof(salt), epoch_key, SC2_KEY_LEN);
    if (rc == SC2_OK) {
        rc = sc2_hkdf_expand_parts(chain->ck, SC2_KEY_LEN, prk, sizeof(prk), info, 2);
    }
    chain->next = 0;
    sc2_memzero(prk, sizeof(prk));
    if (rc != SC2_OK) {
        sc2_memzero(chain, sizeof(*chain));
    }
    return rc;
}

int sc2_room_chain_next(sc2_room_chain_t *chain, uint8_t mk[SC2_KEY_LEN], uint32_t *n)
{
    if (chain == NULL || mk == NULL || n == NULL) {
        return SC2_ERR_ARG;
    }
    if (chain->next == UINT32_MAX) {
        return SC2_ERR_STATE;
    }
    int rc = sc2_chain_step(mk, chain->ck);
    if (rc == SC2_OK) {
        *n = chain->next++;
    }
    return rc;
}

int sc2_room_chain_key(sc2_room_chain_t *chain, uint32_t n, uint8_t mk[SC2_KEY_LEN], int commit)
{
    if (chain == NULL || mk == NULL) {
        return SC2_ERR_ARG;
    }
    if (n < chain->next) {
        return SC2_ERR_REPLAY;
    }
    if (n - chain->next >= SC2_ROOM_MAX_ADVANCE || n == UINT32_MAX) {
        return SC2_ERR_SKIP;
    }
    sc2_room_chain_t tmp = *chain;
    int rc = SC2_OK;
    while (rc == SC2_OK && tmp.next <= n) {
        rc = sc2_chain_step(mk, tmp.ck);
        tmp.next++;
    }
    if (rc == SC2_OK && commit) {
        *chain = tmp;
    }
    if (rc != SC2_OK) {
        sc2_memzero(mk, SC2_KEY_LEN);
    }
    sc2_memzero(&tmp, sizeof(tmp));
    return rc;
}

int sc2_room_header(uint8_t out[SC2_ROOM_HDR_LEN], const uint8_t room[SC2_ROOM_ID_LEN], uint32_t epoch,
                    const uint8_t from[SC2_DEVICE_ID_LEN], uint32_t n)
{
    if (out == NULL || room == NULL || from == NULL) {
        return SC2_ERR_ARG;
    }
    memcpy(out, MAGIC_ROOM, 4);
    out[4] = (uint8_t)SC2_SUITE_1;
    memcpy(out + 5, room, SC2_ROOM_ID_LEN);
    sc2_store_be32(out + 21, epoch);
    memcpy(out + 25, from, SC2_DEVICE_ID_LEN);
    sc2_store_be32(out + 41, n);
    return SC2_OK;
}

int sc2_room_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t hdr[SC2_ROOM_HDR_LEN],
                    const uint8_t nonce[SC2_NONCE_LEN], const uint8_t *ct, size_t ct_len,
                    const uint8_t ik_pub[SC2_KEY_LEN])
{
    if (sig == NULL || hdr == NULL || nonce == NULL || ct == NULL || ik_pub == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_part_t parts[4] = {{LABEL_ROOM, sizeof(LABEL_ROOM)},
                           {hdr, SC2_ROOM_HDR_LEN},
                           {nonce, SC2_NONCE_LEN},
                           {ct, ct_len}};
    return sc2_ed25519_verify_parts(sig, parts, 4, ik_pub);
}

int sc2_room_seal(uint8_t *ct, size_t ct_cap, size_t *ct_len, uint8_t sig[SC2_SIG_LEN],
                  const uint8_t mk[SC2_KEY_LEN], const uint8_t nonce[SC2_NONCE_LEN],
                  const uint8_t hdr[SC2_ROOM_HDR_LEN], const uint8_t *pt, size_t pt_len,
                  const uint8_t ik_seed[SC2_KEY_LEN])
{
    if (ct == NULL || ct_len == NULL || sig == NULL || mk == NULL || nonce == NULL || hdr == NULL ||
        (pt == NULL && pt_len != 0) || ik_seed == NULL) {
        return SC2_ERR_ARG;
    }
    if (pt_len > SC2_PAD_MAX_INPUT) {
        return SC2_ERR_BOUNDS;
    }
    size_t bucket = sc2_pad_bucket(pt_len);
    if (bucket == 0 || ct_cap < bucket + SC2_TAG_LEN) {
        return SC2_ERR_BOUNDS;
    }
    size_t padded = 0;
    int rc = sc2_pad(ct, ct_cap, &padded, pt, pt_len);
    if (rc == SC2_OK) {
        rc = sc2_aead_seal(ct, ct, padded, hdr, SC2_ROOM_HDR_LEN, nonce, mk);
    }
    if (rc == SC2_OK) {
        sc2_part_t parts[4] = {{LABEL_ROOM, sizeof(LABEL_ROOM)},
                               {hdr, SC2_ROOM_HDR_LEN},
                               {nonce, SC2_NONCE_LEN},
                               {ct, padded + SC2_TAG_LEN}};
        rc = sc2_ed25519_sign_parts(sig, parts, 4, ik_seed);
    }
    if (rc == SC2_OK) {
        *ct_len = padded + SC2_TAG_LEN;
    } else {
        sc2_memzero(ct, ct_cap < bucket + SC2_TAG_LEN ? ct_cap : bucket + SC2_TAG_LEN);
    }
    return rc;
}

int sc2_room_open(uint8_t *pt, size_t pt_cap, size_t *pt_len, const uint8_t mk[SC2_KEY_LEN],
                  const uint8_t nonce[SC2_NONCE_LEN], const uint8_t hdr[SC2_ROOM_HDR_LEN], const uint8_t *ct,
                  size_t ct_len, const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN])
{
    if (pt == NULL || pt_len == NULL || mk == NULL) {
        return SC2_ERR_ARG;
    }
    if (ct_len < 256u + SC2_TAG_LEN || ct_len > SC2_PAD_MAX_BUCKET + SC2_TAG_LEN) {
        return SC2_ERR_FORMAT;
    }
    if (pt_cap < ct_len - SC2_TAG_LEN) {
        return SC2_ERR_BOUNDS;
    }
    int rc = sc2_room_verify(sig, hdr, nonce, ct, ct_len, ik_pub);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_aead_open(pt, ct, ct_len, hdr, SC2_ROOM_HDR_LEN, nonce, mk);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_unpad(pt, ct_len - SC2_TAG_LEN, pt_len);
    if (rc != SC2_OK) {
        sc2_memzero(pt, ct_len - SC2_TAG_LEN);
    }
    return rc;
}
