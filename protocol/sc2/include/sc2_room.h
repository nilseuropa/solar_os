/*
 * SC2 portable core: private-room sender chains.
 *
 *   CK_0 = HKDF(salt = room:16 || epoch:u32, IKM = epoch_key,
 *               info = "SC2-SENDER\x01" || sender:16, L = 32)
 *   H    = "SC2R" | suite:u8 | room:16 | epoch:u32 | from:16 | n:u32
 *   ct   = XChaCha20-Poly1305(mk_n, nonce, pad(pt), aad = H)
 *   sig  = Ed25519(IK_from, "SC2-ROOM\x01" || H || nonce || ct)
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_ROOM_H
#define SC2_ROOM_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_ROOM_HDR_LEN 45u
/* Forward steps a receiver will take to reach an index. */
#define SC2_ROOM_MAX_ADVANCE 256u

typedef struct {
    uint8_t ck[SC2_KEY_LEN];
    uint32_t next; /* index of the next message key */
} sc2_room_chain_t;

int sc2_room_chain_init(sc2_room_chain_t *chain, const uint8_t epoch_key[SC2_KEY_LEN],
                        const uint8_t room[SC2_ROOM_ID_LEN], uint32_t epoch,
                        const uint8_t sender[SC2_DEVICE_ID_LEN]);

/* Sender: key for index chain->next, then advance. */
int sc2_room_chain_next(sc2_room_chain_t *chain, uint8_t mk[SC2_KEY_LEN], uint32_t *n);

/* Receiver: key for index n >= chain->next (forward only, at most
 * SC2_ROOM_MAX_ADVANCE steps). Earlier indices return SC2_ERR_REPLAY.
 * The chain is advanced past n only when commit is non-zero, so a caller
 * can derive, try to open, and commit on success. */
int sc2_room_chain_key(sc2_room_chain_t *chain, uint32_t n, uint8_t mk[SC2_KEY_LEN], int commit);

int sc2_room_header(uint8_t out[SC2_ROOM_HDR_LEN], const uint8_t room[SC2_ROOM_ID_LEN], uint32_t epoch,
                    const uint8_t from[SC2_DEVICE_ID_LEN], uint32_t n);

/* Pad, seal and sign. ct receives sc2_pad_bucket(pt_len) + 16 bytes. */
int sc2_room_seal(uint8_t *ct, size_t ct_cap, size_t *ct_len, uint8_t sig[SC2_SIG_LEN],
                  const uint8_t mk[SC2_KEY_LEN], const uint8_t nonce[SC2_NONCE_LEN],
                  const uint8_t hdr[SC2_ROOM_HDR_LEN], const uint8_t *pt, size_t pt_len,
                  const uint8_t ik_seed[SC2_KEY_LEN]);

/* Verify the signature first, then open and unpad. pt may equal ct. */
int sc2_room_open(uint8_t *pt, size_t pt_cap, size_t *pt_len, const uint8_t mk[SC2_KEY_LEN],
                  const uint8_t nonce[SC2_NONCE_LEN], const uint8_t hdr[SC2_ROOM_HDR_LEN], const uint8_t *ct,
                  size_t ct_len, const uint8_t sig[SC2_SIG_LEN], const uint8_t ik_pub[SC2_KEY_LEN]);

/* Signature only (relay or pre-check): over "SC2-ROOM\x01" || H || nonce || ct. */
int sc2_room_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t hdr[SC2_ROOM_HDR_LEN],
                    const uint8_t nonce[SC2_NONCE_LEN], const uint8_t *ct, size_t ct_len,
                    const uint8_t ik_pub[SC2_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif
