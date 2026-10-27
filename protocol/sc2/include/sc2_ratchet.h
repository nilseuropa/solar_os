/*
 * SC2 portable core: Double Ratchet for DMs.
 *
 *   KDF_RK(rk, dh) = HKDF(salt = rk, IKM = dh, info = "SC2-RK\x01", L = 64)
 *   KDF_CK(ck)     = (mk = HMAC(ck, 0x01), ck' = HMAC(ck, 0x02))
 *   ct = XChaCha20-Poly1305(mk, nonce, pad(pt), aad = AD || Hd)
 *   Hd = "SC2D" | suite:u8 | from:16 | to:16 | dh:32 | pn:u32 | n:u32 | flags:u8
 *        [| ek:32 | spk:u32 | opk:u32]           (flags bit 0)
 *
 * Limits: a received message may skip at most SC2_RATCHET_MAX_SKIP keys; at
 * most SC2_RATCHET_MAX_STORED skipped keys are kept (oldest evicted) and
 * each expires SC2_RATCHET_SKIP_TTL seconds after it was stored. Times are
 * supplied by the caller (seconds; any clock that does not run backwards
 * across a session is fine). A failed decrypt leaves the state unchanged.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_RATCHET_H
#define SC2_RATCHET_H

#include "sc2_x3dh.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_RATCHET_MAX_SKIP 32u
/* Stored skipped keys per session: 80 bytes each in RAM. A local limit
 * (oldest evicted first), not part of the wire format; a blob written with
 * more entries loads with the oldest ones dropped. At most 255. */
#ifndef SC2_RATCHET_MAX_STORED
#if SC2_LOW_RAM
#define SC2_RATCHET_MAX_STORED 8u
#else
#define SC2_RATCHET_MAX_STORED 32u
#endif
#endif
#define SC2_RATCHET_SKIP_TTL (7u * 24u * 3600u)
#define SC2_DM_HDR_BASE_LEN 78u
#define SC2_DM_HDR_MAX_LEN (SC2_DM_HDR_BASE_LEN + 40u)
#define SC2_DM_HDR_FLAG_X3DH 0x01u

/* Maximum plaintext (inner JSON) per DM; the padded size is one bucket. */
#define SC2_DM_MAX_PLAINTEXT 8191u
#define SC2_DM_MAX_CIPHERTEXT (8192u + SC2_TAG_LEN)

typedef struct {
    uint8_t dh[SC2_KEY_LEN];
    uint32_t pn;
    uint32_t n;
    uint8_t has_x3dh;
    sc2_x3dh_hdr_t x3dh;
} sc2_dm_header_t;

typedef struct {
    uint8_t dh[SC2_KEY_LEN];
    uint8_t mk[SC2_KEY_LEN];
    uint64_t stored_at;
    uint32_t n;
    uint8_t used;
} sc2_skipped_key_t;

#define SC2_RATCHET_F_SEND 0x01u    /* sending chain present */
#define SC2_RATCHET_F_RECV 0x02u    /* receiving chain present */
#define SC2_RATCHET_F_X3DH 0x04u    /* initiator: attach X3DH header until a reply arrives */
#define SC2_RATCHET_F_INIT 0x08u    /* this side initiated */

typedef struct {
    uint8_t ad[SC2_AD_LEN];
    uint8_t self_id[SC2_DEVICE_ID_LEN];
    uint8_t peer_id[SC2_DEVICE_ID_LEN];
    uint8_t rk[SC2_KEY_LEN];
    uint8_t dhs_priv[SC2_KEY_LEN];
    uint8_t dhs_pub[SC2_KEY_LEN];
    uint8_t dhr[SC2_KEY_LEN];
    uint8_t cks[SC2_KEY_LEN];
    uint8_t ckr[SC2_KEY_LEN];
    uint32_t ns;
    uint32_t nr;
    uint32_t pn;
    uint8_t flags;
    sc2_x3dh_hdr_t x3dh;
    sc2_skipped_key_t skipped[SC2_RATCHET_MAX_STORED];
} sc2_ratchet_t;

/* Initiator after sc2_x3dh_initiate. RNG order: first ratchet key (32). */
int sc2_ratchet_init_initiator(sc2_ratchet_t *st, const uint8_t sk[SC2_KEY_LEN], const uint8_t ad[SC2_AD_LEN],
                               const uint8_t self_id[SC2_DEVICE_ID_LEN], const uint8_t peer_id[SC2_DEVICE_ID_LEN],
                               const uint8_t peer_spk[SC2_KEY_LEN], const sc2_x3dh_hdr_t *x3dh,
                               const sc2_rng_t *rng);

/* Responder after sc2_x3dh_respond; its SPK is the first ratchet key. */
int sc2_ratchet_init_responder(sc2_ratchet_t *st, const uint8_t sk[SC2_KEY_LEN], const uint8_t ad[SC2_AD_LEN],
                               const uint8_t self_id[SC2_DEVICE_ID_LEN], const uint8_t peer_id[SC2_DEVICE_ID_LEN],
                               const sc2_spk_t *spk);

/* Binary header Hd. Returns its length in *out_len. */
int sc2_dm_header_encode(uint8_t out[SC2_DM_HDR_MAX_LEN], size_t *out_len, const sc2_dm_header_t *hdr,
                         const uint8_t from[SC2_DEVICE_ID_LEN], const uint8_t to[SC2_DEVICE_ID_LEN]);

/* Pads pt, advances the sending chain and seals. ct receives
 * sc2_pad_bucket(pt_len) + 16 bytes. RNG order: nonce (24). The caller must
 * persist the state before transmitting. */
int sc2_ratchet_encrypt(sc2_ratchet_t *st, sc2_dm_header_t *hdr, uint8_t nonce[SC2_NONCE_LEN], uint8_t *ct,
                        size_t ct_cap, size_t *ct_len, const uint8_t *pt, size_t pt_len, const sc2_rng_t *rng);

/* Opens a message. work is caller-provided scratch of the same type (the
 * update is computed there and copied into st only on success). pt needs
 * ct_len - 16 bytes; *pt_len is the unpadded length. pt may equal ct (in
 * place): the tag is checked before anything is written, so ct is intact
 * after an SC2_ERR_AUTH and can be retried with another state. RNG order
 * (only on a DH ratchet step): new ratchet key (32). */
int sc2_ratchet_decrypt(sc2_ratchet_t *st, sc2_ratchet_t *work, const sc2_dm_header_t *hdr,
                        const uint8_t nonce[SC2_NONCE_LEN], const uint8_t *ct, size_t ct_len, uint8_t *pt,
                        size_t pt_cap, size_t *pt_len, uint64_t now, const sc2_rng_t *rng);

/* Drop skipped keys older than the TTL. Returns the number dropped. */
size_t sc2_ratchet_expire(sc2_ratchet_t *st, uint64_t now);
size_t sc2_ratchet_skipped_count(const sc2_ratchet_t *st);

/* Versioned state blob: "SC2S" | ver:u8 | fields | crc32. */
#define SC2_RATCHET_BLOB_VERSION 1u
size_t sc2_ratchet_blob_max(void);
int sc2_ratchet_serialize(const sc2_ratchet_t *st, uint8_t *out, size_t cap, size_t *out_len);
int sc2_ratchet_deserialize(sc2_ratchet_t *st, const uint8_t *in, size_t len);

void sc2_ratchet_wipe(sc2_ratchet_t *st);

/* Symmetric chain step shared with room sender chains. */
int sc2_chain_step(uint8_t mk[SC2_KEY_LEN], uint8_t ck[SC2_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif
