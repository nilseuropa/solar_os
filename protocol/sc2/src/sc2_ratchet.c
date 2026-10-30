/*
 * SC2 portable core: Double Ratchet (Signal spec, without header
 * encryption) with HMAC-SHA-256 chains and an HKDF-SHA-256 root KDF.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_ratchet.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"
#include "sc2_hash.h"

static const uint8_t LABEL_RK[] = {'S', 'C', '2', '-', 'R', 'K', 0x01};
static const uint8_t MAGIC_HDR[4] = {'S', 'C', '2', 'D'};
static const uint8_t MAGIC_BLOB[4] = {'S', 'C', '2', 'S'};

#define FLAGS_KNOWN (SC2_RATCHET_F_SEND | SC2_RATCHET_F_RECV | SC2_RATCHET_F_X3DH | SC2_RATCHET_F_INIT)
#define AAD_MAX (SC2_AD_LEN + SC2_DM_HDR_MAX_LEN)

int sc2_chain_step(uint8_t mk[SC2_KEY_LEN], uint8_t ck[SC2_KEY_LEN])
{
    static const uint8_t one = 0x01;
    static const uint8_t two = 0x02;
    uint8_t next[SC2_KEY_LEN];
    if (mk == NULL || ck == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_hmac_sha256(mk, ck, SC2_KEY_LEN, &one, 1);
    if (rc == SC2_OK) {
        rc = sc2_hmac_sha256(next, ck, SC2_KEY_LEN, &two, 1);
    }
    if (rc == SC2_OK) {
        memcpy(ck, next, SC2_KEY_LEN);
    } else {
        sc2_memzero(mk, SC2_KEY_LEN);
    }
    sc2_memzero(next, sizeof(next));
    return rc;
}

/* (rk, ck) = HKDF(salt = rk, IKM = dh, "SC2-RK\x01", 64) */
static int kdf_rk(uint8_t rk[SC2_KEY_LEN], uint8_t ck[SC2_KEY_LEN], const uint8_t dh[SC2_KEY_LEN])
{
    uint8_t out[64];
    int rc = sc2_hkdf(out, sizeof(out), rk, SC2_KEY_LEN, dh, SC2_KEY_LEN, LABEL_RK, sizeof(LABEL_RK));
    if (rc == SC2_OK) {
        memcpy(rk, out, SC2_KEY_LEN);
        memcpy(ck, out + SC2_KEY_LEN, SC2_KEY_LEN);
    }
    sc2_memzero(out, sizeof(out));
    return rc;
}

/* rk, ck <- KDF_RK(rk, X25519(priv, pub)) */
static int dh_step(uint8_t rk[SC2_KEY_LEN], uint8_t ck[SC2_KEY_LEN], const uint8_t priv[SC2_KEY_LEN],
                   const uint8_t pub[SC2_KEY_LEN])
{
    uint8_t dh[SC2_KEY_LEN];
    int rc = sc2_x25519(dh, priv, pub);
    if (rc == SC2_OK) {
        rc = kdf_rk(rk, ck, dh);
    }
    sc2_memzero(dh, sizeof(dh));
    return rc;
}

int sc2_ratchet_init_initiator(sc2_ratchet_t *st, const uint8_t sk[SC2_KEY_LEN], const uint8_t ad[SC2_AD_LEN],
                               const uint8_t self_id[SC2_DEVICE_ID_LEN], const uint8_t peer_id[SC2_DEVICE_ID_LEN],
                               const uint8_t peer_spk[SC2_KEY_LEN], const sc2_x3dh_hdr_t *x3dh,
                               const sc2_rng_t *rng)
{
    if (st == NULL || sk == NULL || ad == NULL || self_id == NULL || peer_id == NULL || peer_spk == NULL ||
        x3dh == NULL) {
        return SC2_ERR_ARG;
    }
    /* Work on small locals (not a full state copy: ~3 KB of stack on the device);
     * *st is written only on success. */
    uint8_t priv[SC2_KEY_LEN], pub[SC2_KEY_LEN], rk[SC2_KEY_LEN], cks[SC2_KEY_LEN];
    memcpy(rk, sk, SC2_KEY_LEN);
    int rc = sc2_x25519_generate(priv, pub, rng);
    if (rc == SC2_OK) {
        rc = dh_step(rk, cks, priv, peer_spk);
    }
    if (rc == SC2_OK) {
        sc2_ratchet_t *s = st;
        memset(s, 0, sizeof(*s));
        memcpy(s->ad, ad, SC2_AD_LEN);
        memcpy(s->self_id, self_id, SC2_DEVICE_ID_LEN);
        memcpy(s->peer_id, peer_id, SC2_DEVICE_ID_LEN);
        memcpy(s->rk, rk, SC2_KEY_LEN);
        memcpy(s->dhs_priv, priv, SC2_KEY_LEN);
        memcpy(s->dhs_pub, pub, SC2_KEY_LEN);
        memcpy(s->dhr, peer_spk, SC2_KEY_LEN);
        memcpy(s->cks, cks, SC2_KEY_LEN);
        s->flags = SC2_RATCHET_F_SEND | SC2_RATCHET_F_X3DH | SC2_RATCHET_F_INIT;
        s->x3dh = *x3dh;
    }
    sc2_memzero(priv, sizeof(priv));
    sc2_memzero(rk, sizeof(rk));
    sc2_memzero(cks, sizeof(cks));
    return rc;
}

int sc2_ratchet_init_responder(sc2_ratchet_t *st, const uint8_t sk[SC2_KEY_LEN], const uint8_t ad[SC2_AD_LEN],
                               const uint8_t self_id[SC2_DEVICE_ID_LEN], const uint8_t peer_id[SC2_DEVICE_ID_LEN],
                               const sc2_spk_t *spk)
{
    if (st == NULL || sk == NULL || ad == NULL || self_id == NULL || peer_id == NULL || spk == NULL) {
        return SC2_ERR_ARG;
    }
    memset(st, 0, sizeof(*st));
    memcpy(st->ad, ad, SC2_AD_LEN);
    memcpy(st->self_id, self_id, SC2_DEVICE_ID_LEN);
    memcpy(st->peer_id, peer_id, SC2_DEVICE_ID_LEN);
    memcpy(st->rk, sk, SC2_KEY_LEN);
    memcpy(st->dhs_priv, spk->priv, SC2_KEY_LEN);
    memcpy(st->dhs_pub, spk->pub, SC2_KEY_LEN);
    st->x3dh.opk_id = SC2_NO_OPK;
    return SC2_OK;
}

int sc2_dm_header_encode(uint8_t out[SC2_DM_HDR_MAX_LEN], size_t *out_len, const sc2_dm_header_t *hdr,
                         const uint8_t from[SC2_DEVICE_ID_LEN], const uint8_t to[SC2_DEVICE_ID_LEN])
{
    if (out == NULL || out_len == NULL || hdr == NULL || from == NULL || to == NULL) {
        return SC2_ERR_ARG;
    }
    size_t o = 0;
    memcpy(out, MAGIC_HDR, 4);
    o += 4;
    out[o++] = (uint8_t)SC2_SUITE_1;
    memcpy(out + o, from, SC2_DEVICE_ID_LEN);
    o += SC2_DEVICE_ID_LEN;
    memcpy(out + o, to, SC2_DEVICE_ID_LEN);
    o += SC2_DEVICE_ID_LEN;
    memcpy(out + o, hdr->dh, SC2_KEY_LEN);
    o += SC2_KEY_LEN;
    sc2_store_be32(out + o, hdr->pn);
    o += 4;
    sc2_store_be32(out + o, hdr->n);
    o += 4;
    out[o++] = hdr->has_x3dh ? (uint8_t)SC2_DM_HDR_FLAG_X3DH : 0u;
    if (hdr->has_x3dh) {
        memcpy(out + o, hdr->x3dh.ek, SC2_KEY_LEN);
        o += SC2_KEY_LEN;
        sc2_store_be32(out + o, hdr->x3dh.spk_id);
        o += 4;
        sc2_store_be32(out + o, hdr->x3dh.opk_id);
        o += 4;
    }
    *out_len = o;
    return SC2_OK;
}

static int build_aad(uint8_t aad[AAD_MAX], size_t *aad_len, const sc2_ratchet_t *st, const sc2_dm_header_t *hdr,
                     int outgoing)
{
    size_t hl = 0;
    memcpy(aad, st->ad, SC2_AD_LEN);
    int rc = sc2_dm_header_encode(aad + SC2_AD_LEN, &hl, hdr, outgoing ? st->self_id : st->peer_id,
                                  outgoing ? st->peer_id : st->self_id);
    *aad_len = SC2_AD_LEN + hl;
    return rc;
}

int sc2_ratchet_encrypt(sc2_ratchet_t *st, sc2_dm_header_t *hdr, uint8_t nonce[SC2_NONCE_LEN], uint8_t *ct,
                        size_t ct_cap, size_t *ct_len, const uint8_t *pt, size_t pt_len, const sc2_rng_t *rng)
{
    uint8_t aad[AAD_MAX];
    size_t aad_len = 0;
    uint8_t mk[SC2_KEY_LEN];
    uint8_t ck[SC2_KEY_LEN];
    sc2_dm_header_t h;

    if (st == NULL || hdr == NULL || nonce == NULL || ct == NULL || ct_len == NULL || (pt == NULL && pt_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (!(st->flags & SC2_RATCHET_F_SEND)) {
        return SC2_ERR_STATE;
    }
    if (st->ns == UINT32_MAX) {
        return SC2_ERR_STATE;
    }
    if (pt_len > SC2_DM_MAX_PLAINTEXT) {
        return SC2_ERR_BOUNDS;
    }
    size_t bucket = sc2_pad_bucket(pt_len);
    if (bucket == 0 || ct_cap < bucket + SC2_TAG_LEN) {
        return SC2_ERR_BOUNDS;
    }
    uint8_t n24[SC2_NONCE_LEN];
    int rc = sc2_random(rng, n24, sizeof(n24));
    if (rc != SC2_OK) {
        return rc;
    }
    memset(&h, 0, sizeof(h));
    memcpy(h.dh, st->dhs_pub, SC2_KEY_LEN);
    h.pn = st->pn;
    h.n = st->ns;
    h.has_x3dh = (st->flags & SC2_RATCHET_F_X3DH) ? 1u : 0u;
    if (h.has_x3dh) {
        h.x3dh = st->x3dh;
    } else {
        h.x3dh.opk_id = SC2_NO_OPK;
    }
    memcpy(ck, st->cks, SC2_KEY_LEN);
    rc = sc2_chain_step(mk, ck);
    if (rc == SC2_OK) {
        rc = build_aad(aad, &aad_len, st, &h, 1);
    }
    size_t padded = 0;
    if (rc == SC2_OK) {
        rc = sc2_pad(ct, ct_cap, &padded, pt, pt_len);
    }
    if (rc == SC2_OK) {
        rc = sc2_aead_seal(ct, ct, padded, aad, aad_len, n24, mk);
    }
    if (rc == SC2_OK) {
        memcpy(st->cks, ck, SC2_KEY_LEN);
        st->ns++;
        *hdr = h;
        memcpy(nonce, n24, SC2_NONCE_LEN);
        *ct_len = padded + SC2_TAG_LEN;
    }
    sc2_memzero(mk, sizeof(mk));
    sc2_memzero(ck, sizeof(ck));
    return rc;
}

/* Skipped-key store: entries [0, count) are in use, oldest first. */
static size_t skipped_used(const sc2_ratchet_t *st)
{
    size_t n = 0;
    while (n < SC2_RATCHET_MAX_STORED && st->skipped[n].used) {
        n++;
    }
    return n;
}

static void skipped_remove(sc2_ratchet_t *st, size_t idx)
{
    size_t used = skipped_used(st);
    if (idx >= used) {
        return;
    }
    memmove(&st->skipped[idx], &st->skipped[idx + 1], (used - idx - 1) * sizeof(st->skipped[0]));
    sc2_memzero(&st->skipped[used - 1], sizeof(st->skipped[0]));
}

static void skipped_add(sc2_ratchet_t *st, const uint8_t dh[SC2_KEY_LEN], uint32_t n, const uint8_t mk[SC2_KEY_LEN],
                        uint64_t now)
{
    size_t used = skipped_used(st);
    if (used == SC2_RATCHET_MAX_STORED) {
        skipped_remove(st, 0);
        used--;
    }
    sc2_skipped_key_t *e = &st->skipped[used];
    memcpy(e->dh, dh, SC2_KEY_LEN);
    memcpy(e->mk, mk, SC2_KEY_LEN);
    e->n = n;
    e->stored_at = now;
    e->used = 1;
}

size_t sc2_ratchet_expire(sc2_ratchet_t *st, uint64_t now)
{
    size_t dropped = 0;
    if (st == NULL) {
        return 0;
    }
    size_t i = 0;
    while (i < skipped_used(st)) {
        const sc2_skipped_key_t *e = &st->skipped[i];
        if (now >= e->stored_at && now - e->stored_at >= SC2_RATCHET_SKIP_TTL) {
            skipped_remove(st, i);
            dropped++;
        } else {
            i++;
        }
    }
    return dropped;
}

size_t sc2_ratchet_skipped_count(const sc2_ratchet_t *st)
{
    return st ? skipped_used(st) : 0;
}

static int skip_keys(sc2_ratchet_t *st, uint32_t until, uint64_t now)
{
    if (!(st->flags & SC2_RATCHET_F_RECV)) {
        return SC2_OK;
    }
    if (until <= st->nr) {
        return SC2_OK;
    }
    if (until - st->nr > SC2_RATCHET_MAX_SKIP) {
        return SC2_ERR_SKIP;
    }
    uint8_t mk[SC2_KEY_LEN];
    while (st->nr < until) {
        int rc = sc2_chain_step(mk, st->ckr);
        if (rc != SC2_OK) {
            sc2_memzero(mk, sizeof(mk));
            return rc;
        }
        skipped_add(st, st->dhr, st->nr, mk, now);
        st->nr++;
    }
    sc2_memzero(mk, sizeof(mk));
    return SC2_OK;
}

static int open_with(const sc2_ratchet_t *st, const sc2_dm_header_t *hdr, const uint8_t mk[SC2_KEY_LEN],
                     const uint8_t nonce[SC2_NONCE_LEN], const uint8_t *ct, size_t ct_len, uint8_t *pt,
                     size_t *pt_len)
{
    uint8_t aad[AAD_MAX];
    size_t aad_len = 0;
    int rc = build_aad(aad, &aad_len, st, hdr, 0);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_aead_open(pt, ct, ct_len, aad, aad_len, nonce, mk);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_unpad(pt, ct_len - SC2_TAG_LEN, pt_len);
    if (rc != SC2_OK) {
        sc2_memzero(pt, ct_len - SC2_TAG_LEN);
    }
    return rc;
}

int sc2_ratchet_decrypt(sc2_ratchet_t *st, sc2_ratchet_t *work, const sc2_dm_header_t *hdr,
                        const uint8_t nonce[SC2_NONCE_LEN], const uint8_t *ct, size_t ct_len, uint8_t *pt,
                        size_t pt_cap, size_t *pt_len, uint64_t now, const sc2_rng_t *rng)
{
    uint8_t mk[SC2_KEY_LEN];
    if (st == NULL || work == NULL || work == st || hdr == NULL || nonce == NULL || ct == NULL || pt == NULL ||
        pt_len == NULL) {
        return SC2_ERR_ARG;
    }
    if (ct_len < 256u + SC2_TAG_LEN || ct_len > SC2_DM_MAX_CIPHERTEXT) {
        return SC2_ERR_FORMAT;
    }
    if (pt_cap < ct_len - SC2_TAG_LEN) {
        return SC2_ERR_BOUNDS;
    }
    memcpy(work, st, sizeof(*work));
    sc2_ratchet_expire(work, now);

    int rc = SC2_OK;
    size_t used = skipped_used(work);
    for (size_t i = 0; i < used; i++) {
        sc2_skipped_key_t *e = &work->skipped[i];
        if (e->n == hdr->n && memcmp(e->dh, hdr->dh, SC2_KEY_LEN) == 0) {
            memcpy(mk, e->mk, SC2_KEY_LEN);
            rc = open_with(work, hdr, mk, nonce, ct, ct_len, pt, pt_len);
            if (rc == SC2_OK) {
                skipped_remove(work, i);
                goto commit;
            }
            goto fail;
        }
    }

    if (!(work->flags & SC2_RATCHET_F_RECV) || memcmp(hdr->dh, work->dhr, SC2_KEY_LEN) != 0) {
        rc = skip_keys(work, hdr->pn, now);
        if (rc != SC2_OK) {
            goto fail;
        }
        work->pn = work->ns;
        work->ns = 0;
        work->nr = 0;
        memcpy(work->dhr, hdr->dh, SC2_KEY_LEN);
        rc = dh_step(work->rk, work->ckr, work->dhs_priv, work->dhr);
        if (rc != SC2_OK) {
            goto fail;
        }
        work->flags |= SC2_RATCHET_F_RECV;
        rc = sc2_x25519_generate(work->dhs_priv, work->dhs_pub, rng);
        if (rc != SC2_OK) {
            goto fail;
        }
        rc = dh_step(work->rk, work->cks, work->dhs_priv, work->dhr);
        if (rc != SC2_OK) {
            goto fail;
        }
        work->flags |= SC2_RATCHET_F_SEND;
    } else if (hdr->n < work->nr) {
        rc = SC2_ERR_REPLAY;
        goto fail;
    }
    if (hdr->n == UINT32_MAX) {
        rc = SC2_ERR_FORMAT;
        goto fail;
    }
    rc = skip_keys(work, hdr->n, now);
    if (rc != SC2_OK) {
        goto fail;
    }
    rc = sc2_chain_step(mk, work->ckr);
    if (rc != SC2_OK) {
        goto fail;
    }
    work->nr = hdr->n + 1u;
    rc = open_with(work, hdr, mk, nonce, ct, ct_len, pt, pt_len);
    if (rc != SC2_OK) {
        goto fail;
    }

commit:
    work->flags &= (uint8_t)~SC2_RATCHET_F_X3DH;
    memcpy(st, work, sizeof(*st));
    sc2_memzero(work, sizeof(*work));
    sc2_memzero(mk, sizeof(mk));
    return SC2_OK;

fail:
    sc2_memzero(work, sizeof(*work));
    sc2_memzero(mk, sizeof(mk));
    return rc;
}

#if SC2_RATCHET_MAX_STORED < 1u || SC2_RATCHET_MAX_STORED > 255u
#error "SC2_RATCHET_MAX_STORED must be 1..255"
#endif

#define BLOB_BASE (4u + 1u + 1u + SC2_AD_LEN + 2u * SC2_DEVICE_ID_LEN + 6u * SC2_KEY_LEN + 12u + SC2_KEY_LEN + 8u + 1u)
#define BLOB_ENTRY (SC2_KEY_LEN + 4u + SC2_KEY_LEN + 8u)

size_t sc2_ratchet_blob_max(void)
{
    return BLOB_BASE + SC2_RATCHET_MAX_STORED * BLOB_ENTRY + 4u;
}

int sc2_ratchet_serialize(const sc2_ratchet_t *st, uint8_t *out, size_t cap, size_t *out_len)
{
    if (st == NULL || out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    size_t count = skipped_used(st);
    size_t need = BLOB_BASE + count * BLOB_ENTRY + 4u;
    if (cap < need) {
        return SC2_ERR_BOUNDS;
    }
    size_t o = 0;
#define PUT(src, n)                   \
    do {                              \
        memcpy(out + o, (src), (n));  \
        o += (n);                     \
    } while (0)
#define PUT32(v)                      \
    do {                              \
        sc2_store_be32(out + o, (v)); \
        o += 4u;                      \
    } while (0)
    PUT(MAGIC_BLOB, 4);
    out[o++] = (uint8_t)SC2_RATCHET_BLOB_VERSION;
    out[o++] = st->flags;
    PUT(st->ad, SC2_AD_LEN);
    PUT(st->self_id, SC2_DEVICE_ID_LEN);
    PUT(st->peer_id, SC2_DEVICE_ID_LEN);
    PUT(st->rk, SC2_KEY_LEN);
    PUT(st->dhs_priv, SC2_KEY_LEN);
    PUT(st->dhs_pub, SC2_KEY_LEN);
    PUT(st->dhr, SC2_KEY_LEN);
    PUT(st->cks, SC2_KEY_LEN);
    PUT(st->ckr, SC2_KEY_LEN);
    PUT32(st->ns);
    PUT32(st->nr);
    PUT32(st->pn);
    PUT(st->x3dh.ek, SC2_KEY_LEN);
    PUT32(st->x3dh.spk_id);
    PUT32(st->x3dh.opk_id);
    out[o++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        const sc2_skipped_key_t *e = &st->skipped[i];
        PUT(e->dh, SC2_KEY_LEN);
        PUT32(e->n);
        PUT(e->mk, SC2_KEY_LEN);
        sc2_store_be64(out + o, e->stored_at);
        o += 8u;
    }
    PUT32(sc2_crc32(0, out, o));
#undef PUT
#undef PUT32
    *out_len = o;
    return SC2_OK;
}

int sc2_ratchet_deserialize(sc2_ratchet_t *st, const uint8_t *in, size_t len)
{
    if (st == NULL || in == NULL) {
        return SC2_ERR_ARG;
    }
    if (len < BLOB_BASE + 4u || memcmp(in, MAGIC_BLOB, 4) != 0 || in[4] != SC2_RATCHET_BLOB_VERSION) {
        return SC2_ERR_FORMAT;
    }
    size_t count = in[BLOB_BASE - 1u];
    if (len != BLOB_BASE + count * BLOB_ENTRY + 4u) {
        return SC2_ERR_FORMAT;
    }
    if (sc2_crc32(0, in, len - 4u) != sc2_load_be32(in + len - 4u)) {
        return SC2_ERR_FORMAT;
    }
    if (in[5] & (uint8_t)~FLAGS_KNOWN) {
        return SC2_ERR_FORMAT;
    }
    /* fully validated above: decode straight into *st (no stack copy) */
    sc2_ratchet_t *s = st;
    memset(s, 0, sizeof(*s));
    size_t o = 5;
#define GET(dst, n)                  \
    do {                             \
        memcpy((dst), in + o, (n));  \
        o += (n);                    \
    } while (0)
#define GET32(v)                     \
    do {                             \
        (v) = sc2_load_be32(in + o); \
        o += 4u;                     \
    } while (0)
    s->flags = in[o++];
    GET(s->ad, SC2_AD_LEN);
    GET(s->self_id, SC2_DEVICE_ID_LEN);
    GET(s->peer_id, SC2_DEVICE_ID_LEN);
    GET(s->rk, SC2_KEY_LEN);
    GET(s->dhs_priv, SC2_KEY_LEN);
    GET(s->dhs_pub, SC2_KEY_LEN);
    GET(s->dhr, SC2_KEY_LEN);
    GET(s->cks, SC2_KEY_LEN);
    GET(s->ckr, SC2_KEY_LEN);
    GET32(s->ns);
    GET32(s->nr);
    GET32(s->pn);
    GET(s->x3dh.ek, SC2_KEY_LEN);
    GET32(s->x3dh.spk_id);
    GET32(s->x3dh.opk_id);
    o++; /* count */
    /* Entries are stored oldest first; keep the newest that fit (a build
     * with a smaller SC2_RATCHET_MAX_STORED reading an older blob). */
    size_t drop = count > SC2_RATCHET_MAX_STORED ? count - SC2_RATCHET_MAX_STORED : 0u;
    o += drop * BLOB_ENTRY;
    for (size_t i = 0; i < count - drop; i++) {
        sc2_skipped_key_t *e = &s->skipped[i];
        GET(e->dh, SC2_KEY_LEN);
        GET32(e->n);
        GET(e->mk, SC2_KEY_LEN);
        e->stored_at = sc2_load_be64(in + o);
        o += 8u;
        e->used = 1;
    }
#undef GET
#undef GET32
    return SC2_OK;
}

void sc2_ratchet_wipe(sc2_ratchet_t *st)
{
    if (st) {
        sc2_memzero(st, sizeof(*st));
    }
}
