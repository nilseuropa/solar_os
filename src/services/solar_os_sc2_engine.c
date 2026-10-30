/*
 * SC2 session engine (see solar_os_sc2_engine.h).
 * SPDX-License-Identifier: MIT
 */
#include "solar_os_sc2_engine.h"

#include <string.h>

#define REC_MAGIC "SC2E"
#define REC_VERSION 1u
#define DM_SEEN_EK 4u
#define ROOM_REC_MAX 4096u
#define PREKEYS_REC_MAX 3072u
#define PEER_REC_MAX 512u
#define DM_REC_MAX 3072u
#define IDENTITY_REC_MAX 128u
#define ROOM_SHARE_INNER_MAX (336u + 6u * SC2E_ROOM_TITLE_MAX + ((17u * SC2_ROSTER_CAP + 2u) / 3u) * 4u)

/* dm_decrypt lends its record buffer to prekeys_save_into. */
_Static_assert(DM_REC_MAX >= PREKEYS_REC_MAX, "dm record buffer doubles as the prekeys buffer");

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t len;
    int err;
} bw_t;

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    int err;
} br_t;

static void bw_put(bw_t *w, const void *data, size_t n)
{
    if (w->err) {
        return;
    }
    if (w->cap - w->len < n) {
        w->err = SC2_ERR_BOUNDS;
        return;
    }
    memcpy(w->p + w->len, data, n);
    w->len += n;
}

static void bw_u8(bw_t *w, uint8_t v)
{
    bw_put(w, &v, 1);
}

static void bw_u32(bw_t *w, uint32_t v)
{
    uint8_t b[4];
    sc2_store_be32(b, v);
    bw_put(w, b, 4);
}

static void bw_u64(bw_t *w, uint64_t v)
{
    uint8_t b[8];
    sc2_store_be64(b, v);
    bw_put(w, b, 8);
}

static void br_get(br_t *r, void *out, size_t n)
{
    if (r->err) {
        memset(out, 0, n);
        return;
    }
    if (r->len - r->pos < n) {
        r->err = SC2_ERR_FORMAT;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->p + r->pos, n);
    r->pos += n;
}

static uint8_t br_u8(br_t *r)
{
    uint8_t v;
    br_get(r, &v, 1);
    return v;
}

static uint32_t br_u32(br_t *r)
{
    uint8_t b[4];
    br_get(r, b, 4);
    return sc2_load_be32(b);
}

static uint64_t br_u64(br_t *r)
{
    uint8_t b[8];
    br_get(r, b, 8);
    return sc2_load_be64(b);
}

static int br_done(const br_t *r)
{
    if (r->err) {
        return r->err;
    }
    return r->pos == r->len ? SC2_OK : SC2_ERR_FORMAT;
}

static void *ws_alloc(const sc2e_t *e, size_t len)
{
    return e->pf.alloc(e->pf.ctx, len);
}

static void ws_release(const sc2e_t *e, void *ptr, size_t len)
{
    if (ptr != NULL) {
        e->pf.release(e->pf.ctx, ptr, len);
    }
}

static uint64_t now_s(const sc2e_t *e)
{
    return e->pf.now ? e->pf.now(e->pf.ctx) : 0u;
}

static int parse_id(const char *hex, uint8_t out[SC2_DEVICE_ID_LEN])
{
    if (hex == NULL || strlen(hex) != SC2_DEVICE_ID_HEX_LEN) {
        return SC2_ERR_ARG;
    }
    return sc2_hex_decode(out, SC2_DEVICE_ID_LEN, hex, SC2_DEVICE_ID_HEX_LEN) == SC2_OK ? SC2_OK : SC2_ERR_ARG;
}

static void id_hex(char out[SC2_DEVICE_ID_HEX_LEN + 1], const uint8_t id[SC2_DEVICE_ID_LEN])
{
    (void)sc2_hex_encode(out, SC2_DEVICE_ID_HEX_LEN + 1, id, SC2_DEVICE_ID_LEN);
}

static void rec_name(char out[SC2E_NAME_MAX], const char *dir, const uint8_t id[SC2_DEVICE_ID_LEN])
{
    size_t n = strlen(dir);
    memcpy(out, dir, n);
    out[n] = '/';
    id_hex(out + n + 1, id);
}

static const char *opt_rid(const char *rid)
{
    return (rid != NULL && rid[0] != '\0') ? rid : NULL;
}

/* ---------------------------------------------------------------------- */
/* Records: "SC2E" | type:u8 | version:u8 | 0:u16 | len:u32 | payload | crc32 */
/* ---------------------------------------------------------------------- */

int sc2e_record_wrap(uint8_t *out, size_t cap, size_t *out_len, uint8_t type, uint8_t version, const uint8_t *payload,
                     size_t payload_len)
{
    if (out == NULL || out_len == NULL || (payload == NULL && payload_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (payload_len > UINT32_MAX || cap < payload_len + SC2E_RECORD_OVERHEAD) {
        return SC2_ERR_BOUNDS;
    }
    memcpy(out, REC_MAGIC, 4);
    out[4] = type;
    out[5] = version;
    out[6] = 0;
    out[7] = 0;
    sc2_store_be32(out + 8, (uint32_t)payload_len);
    if (payload_len) {
        memmove(out + 12, payload, payload_len);
    }
    uint32_t crc = sc2_crc32(0, out, 12u + payload_len);
    sc2_store_be32(out + 12 + payload_len, crc);
    *out_len = payload_len + SC2E_RECORD_OVERHEAD;
    return SC2_OK;
}

int sc2e_record_unwrap(const uint8_t *in, size_t len, uint8_t type, uint8_t version, const uint8_t **payload,
                       size_t *payload_len)
{
    if (in == NULL || payload == NULL || payload_len == NULL) {
        return SC2_ERR_ARG;
    }
    if (len < SC2E_RECORD_OVERHEAD || memcmp(in, REC_MAGIC, 4) != 0 || in[6] != 0 || in[7] != 0) {
        return SC2_ERR_FORMAT;
    }
    uint32_t n = sc2_load_be32(in + 8);
    if ((size_t)n != len - SC2E_RECORD_OVERHEAD) {
        return SC2_ERR_FORMAT;
    }
    if (sc2_crc32(0, in, 12u + n) != sc2_load_be32(in + 12 + n)) {
        return SC2_ERR_FORMAT;
    }
    if (in[4] != type || in[5] != version) {
        return SC2_ERR_FORMAT;
    }
    *payload = in + 12;
    *payload_len = n;
    return SC2_OK;
}

/* Load and unwrap record `name` into buf (cap bytes). */
static int rec_load(const sc2e_t *e, const char *name, uint8_t type, uint8_t *buf, size_t cap, const uint8_t **payload,
                    size_t *payload_len)
{
    size_t len = 0;
    int rc = e->pf.load(e->pf.ctx, name, buf, cap, &len);
    if (rc != SC2_OK) {
        return rc;
    }
    if (len > cap) {
        return SC2_ERR_BOUNDS;
    }
    return sc2e_record_unwrap(buf, len, type, REC_VERSION, payload, payload_len);
}

/* Wrap payload (which lives at buf + 12) in place and save it. */
static int rec_save_inplace(const sc2e_t *e, const char *name, uint8_t type, uint8_t *buf, size_t cap,
                            size_t payload_len)
{
    size_t total = 0;
    int rc = sc2e_record_wrap(buf, cap, &total, type, REC_VERSION, buf + 12, payload_len);
    if (rc == SC2_OK) {
        rc = e->pf.save(e->pf.ctx, name, buf, total);
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Identity and prekeys                                                    */
/* ---------------------------------------------------------------------- */

static int identity_save(const sc2e_t *e, uint64_t created)
{
    uint8_t buf[IDENTITY_REC_MAX];
    bw_t w = {buf + 12, sizeof(buf) - SC2E_RECORD_OVERHEAD, 0, 0};
    bw_put(&w, e->id.ik_seed, SC2_KEY_LEN);
    bw_put(&w, e->id.dk_priv, SC2_KEY_LEN);
    bw_u64(&w, created);
    int rc = w.err ? w.err : rec_save_inplace(e, "identity", SC2E_REC_IDENTITY, buf, sizeof(buf), w.len);
    sc2_memzero(buf, sizeof(buf));
    return rc;
}

static int identity_load(sc2e_t *e)
{
    uint8_t buf[IDENTITY_REC_MAX];
    uint8_t ik[SC2_KEY_LEN];
    uint8_t dk[SC2_KEY_LEN];
    const uint8_t *p;
    size_t n;
    int rc = rec_load(e, "identity", SC2E_REC_IDENTITY, buf, sizeof(buf), &p, &n);
    if (rc == SC2_OK) {
        br_t r = {p, n, 0, 0};
        br_get(&r, ik, SC2_KEY_LEN);
        br_get(&r, dk, SC2_KEY_LEN);
        (void)br_u64(&r);
        rc = br_done(&r);
        if (rc == SC2_OK) {
            rc = sc2_identity_from_secrets(&e->id, ik, dk);
        }
    }
    sc2_memzero(ik, sizeof(ik));
    sc2_memzero(dk, sizeof(dk));
    sc2_memzero(buf, sizeof(buf));
    return rc;
}

/* Serialise and save the prekeys using buf (PREKEYS_REC_MAX bytes) as the
 * record buffer, so callers that already hold a work area can lend it. */
static int prekeys_save_into(const sc2e_t *e, uint8_t *buf)
{
    bw_t w = {buf + 12, PREKEYS_REC_MAX - SC2E_RECORD_OVERHEAD, 0, 0};
    const sc2e_prekeys_t *pk = &e->pre;
    bw_u32(&w, pk->next_spk_id);
    bw_u32(&w, pk->next_opk_id);
    for (size_t i = 0; i < 2; i++) {
        const sc2e_spk_slot_t *s = &pk->spk[i];
        bw_u8(&w, s->valid);
        bw_u32(&w, s->spk.id);
        bw_u64(&w, s->created);
        bw_put(&w, s->spk.priv, SC2_KEY_LEN);
        bw_put(&w, s->spk.pub, SC2_KEY_LEN);
        bw_put(&w, s->spk.sig, SC2_SIG_LEN);
    }
    bw_u32(&w, pk->opk_count);
    for (uint32_t i = 0; i < pk->opk_count && i < SC2E_OPK_MAX; i++) {
        bw_u32(&w, pk->opk[i].id);
        bw_put(&w, pk->opk[i].priv, SC2_KEY_LEN);
        bw_put(&w, pk->opk[i].pub, SC2_KEY_LEN);
    }
    int rc = w.err ? w.err : rec_save_inplace(e, "prekeys", SC2E_REC_PREKEYS, buf, PREKEYS_REC_MAX, w.len);
    sc2_memzero(buf, PREKEYS_REC_MAX);
    return rc;
}

static int prekeys_save(const sc2e_t *e)
{
    uint8_t *buf = ws_alloc(e, PREKEYS_REC_MAX);
    if (buf == NULL) {
        return SC2_ERR_BOUNDS;
    }
    int rc = prekeys_save_into(e, buf);
    ws_release(e, buf, PREKEYS_REC_MAX);
    return rc;
}

static int prekeys_load(sc2e_t *e)
{
    uint8_t *buf = ws_alloc(e, PREKEYS_REC_MAX);
    const uint8_t *p;
    size_t n;
    if (buf == NULL) {
        return SC2_ERR_BOUNDS;
    }
    int rc = rec_load(e, "prekeys", SC2E_REC_PREKEYS, buf, PREKEYS_REC_MAX, &p, &n);
    if (rc == SC2_OK) {
        sc2e_prekeys_t *pk = &e->pre;
        br_t r = {p, n, 0, 0};
        memset(pk, 0, sizeof(*pk));
        pk->next_spk_id = br_u32(&r);
        pk->next_opk_id = br_u32(&r);
        for (size_t i = 0; i < 2; i++) {
            sc2e_spk_slot_t *s = &pk->spk[i];
            s->valid = br_u8(&r);
            s->spk.id = br_u32(&r);
            s->created = br_u64(&r);
            br_get(&r, s->spk.priv, SC2_KEY_LEN);
            br_get(&r, s->spk.pub, SC2_KEY_LEN);
            br_get(&r, s->spk.sig, SC2_SIG_LEN);
            if (s->valid > 1) {
                r.err = SC2_ERR_FORMAT;
            }
        }
        pk->opk_count = br_u32(&r);
        if (pk->opk_count > SC2E_OPK_MAX) {
            r.err = SC2_ERR_FORMAT;
        }
        for (uint32_t i = 0; i < pk->opk_count && !r.err; i++) {
            pk->opk[i].id = br_u32(&r);
            br_get(&r, pk->opk[i].priv, SC2_KEY_LEN);
            br_get(&r, pk->opk[i].pub, SC2_KEY_LEN);
        }
        rc = br_done(&r);
        if (rc == SC2_OK && !pk->spk[0].valid) {
            rc = SC2_ERR_FORMAT;
        }
        for (size_t i = 0; i < 2 && rc == SC2_OK; i++) {
            if (pk->spk[i].valid &&
                sc2_spk_verify(pk->spk[i].spk.sig, e->id.ik_pub, pk->spk[i].spk.id, pk->spk[i].spk.pub) != SC2_OK) {
                rc = SC2_ERR_FORMAT;
            }
        }
        if (rc != SC2_OK) {
            sc2_memzero(pk, sizeof(*pk));
        }
    }
    ws_release(e, buf, PREKEYS_REC_MAX);
    return rc;
}

static int spk_rotate(sc2e_t *e)
{
    sc2e_prekeys_t *pk = &e->pre;
    sc2_spk_t fresh;
    uint32_t id = pk->next_spk_id ? pk->next_spk_id : 1u;
    int rc = sc2_spk_generate(&fresh, id, &e->id, &e->pf.rng);
    if (rc == SC2_OK) {
        pk->spk[1] = pk->spk[0];
        pk->spk[0].spk = fresh;
        pk->spk[0].created = now_s(e);
        pk->spk[0].valid = 1;
        pk->next_spk_id = id + 1u;
    }
    sc2_memzero(&fresh, sizeof(fresh));
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Open / close / identity                                                 */
/* ---------------------------------------------------------------------- */

int sc2e_open(sc2e_t *e, const sc2e_platform_t *pf)
{
    if (e == NULL || pf == NULL || pf->load == NULL || pf->save == NULL || pf->remove == NULL ||
        pf->alloc == NULL || pf->release == NULL || pf->rng.fn == NULL) {
        return SC2_ERR_ARG;
    }
    memset(e, 0, sizeof(*e));
    e->pf = *pf;
    int rc = identity_load(e);
    if (rc == SC2_ERR_NOT_FOUND) {
        rc = sc2_identity_generate(&e->id, &e->pf.rng);
        if (rc == SC2_OK) {
            rc = identity_save(e, now_s(e));
        }
        if (rc == SC2_OK) {
            e->created = true;
            (void)e->pf.remove(e->pf.ctx, "prekeys");
        }
    }
    if (rc == SC2_OK) {
        rc = sc2_device_id(e->self_id, e->id.ik_pub);
    }
    if (rc == SC2_OK) {
        id_hex(e->self_hex, e->self_id);
        rc = prekeys_load(e);
        if (rc == SC2_ERR_NOT_FOUND) {
            memset(&e->pre, 0, sizeof(e->pre));
            e->pre.next_spk_id = 1;
            e->pre.next_opk_id = 1;
            rc = spk_rotate(e);
            if (rc == SC2_OK) {
                rc = prekeys_save(e);
            }
        }
    }
    if (rc != SC2_OK) {
        sc2e_close(e);
        return rc;
    }
    e->open = true;
    return SC2_OK;
}

void sc2e_close(sc2e_t *e)
{
    if (e != NULL) {
        sc2_memzero(e, sizeof(*e));
    }
}

int sc2e_identity(const sc2e_t *e, sc2e_identity_info_t *out)
{
    if (e == NULL || out == NULL || !e->open) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->device_id, e->self_hex, sizeof(out->device_id));
    int rc = sc2_b64_encode(out->ik_b64, sizeof(out->ik_b64), NULL, e->id.ik_pub, SC2_KEY_LEN);
    if (rc == SC2_OK) {
        rc = sc2_b64_encode(out->dk_b64, sizeof(out->dk_b64), NULL, e->id.dk_pub, SC2_KEY_LEN);
    }
    if (rc == SC2_OK) {
        rc = sc2_fingerprint_short(out->fingerprint, e->id.ik_pub);
    }
    out->spk_id = e->pre.spk[0].spk.id;
    out->spk_created = e->pre.spk[0].created;
    out->opk_count = e->pre.opk_count;
    return rc;
}

int sc2e_counts(const sc2e_t *e, sc2e_counts_t *out)
{
    if (e == NULL || out == NULL || !e->open) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (e->pf.count == NULL) {
        return SC2_ERR_NOT_FOUND;
    }
    int rc = e->pf.count(e->pf.ctx, SC2E_DIR_PEER, &out->peers);
    if (rc == SC2_OK) {
        rc = e->pf.count(e->pf.ctx, SC2E_DIR_DM, &out->sessions);
    }
    if (rc == SC2_OK) {
        rc = e->pf.count(e->pf.ctx, SC2E_DIR_ROOM, &out->rooms);
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Peer bundles                                                            */
/* ---------------------------------------------------------------------- */

static int peer_save(const sc2e_t *e, const sc2_bundle_t *b)
{
    uint8_t buf[PEER_REC_MAX];
    char name[SC2E_NAME_MAX];
    uint8_t id[SC2_DEVICE_ID_LEN];
    int rc = sc2_device_id(id, b->ik);
    if (rc != SC2_OK) {
        return rc;
    }
    rec_name(name, SC2E_DIR_PEER, id);
    bw_t w = {buf + 12, sizeof(buf) - SC2E_RECORD_OVERHEAD, 0, 0};
    bw_put(&w, b->ik, SC2_KEY_LEN);
    bw_put(&w, b->dk, SC2_KEY_LEN);
    bw_put(&w, b->dk_sig, SC2_SIG_LEN);
    bw_u32(&w, b->spk_id);
    bw_put(&w, b->spk, SC2_KEY_LEN);
    bw_put(&w, b->spk_sig, SC2_SIG_LEN);
    bw_u32(&w, b->opk_id);
    bw_put(&w, b->opk, SC2_KEY_LEN);
    bw_u64(&w, now_s(e));
    return w.err ? w.err : rec_save_inplace(e, name, SC2E_REC_PEER, buf, sizeof(buf), w.len);
}

static int peer_load(const sc2e_t *e, const uint8_t id[SC2_DEVICE_ID_LEN], sc2_bundle_t *b)
{
    uint8_t buf[PEER_REC_MAX];
    char name[SC2E_NAME_MAX];
    uint8_t check[SC2_DEVICE_ID_LEN];
    const uint8_t *p;
    size_t n;
    rec_name(name, SC2E_DIR_PEER, id);
    int rc = rec_load(e, name, SC2E_REC_PEER, buf, sizeof(buf), &p, &n);
    if (rc != SC2_OK) {
        return rc;
    }
    br_t r = {p, n, 0, 0};
    memset(b, 0, sizeof(*b));
    br_get(&r, b->ik, SC2_KEY_LEN);
    br_get(&r, b->dk, SC2_KEY_LEN);
    br_get(&r, b->dk_sig, SC2_SIG_LEN);
    b->spk_id = br_u32(&r);
    br_get(&r, b->spk, SC2_KEY_LEN);
    br_get(&r, b->spk_sig, SC2_SIG_LEN);
    b->opk_id = br_u32(&r);
    br_get(&r, b->opk, SC2_KEY_LEN);
    (void)br_u64(&r);
    rc = br_done(&r);
    if (rc == SC2_OK) {
        rc = sc2_device_id(check, b->ik);
    }
    if (rc == SC2_OK && memcmp(check, id, SC2_DEVICE_ID_LEN) != 0) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        rc = sc2_bundle_verify(b) == SC2_OK ? SC2_OK : SC2_ERR_FORMAT;
    }
    return rc;
}

static int get_obj(const sc2_json_val_t *root, const char *key, sc2_json_val_t *out)
{
    int rc = sc2_json_get(root, key, out);
    if (rc == SC2_OK && out->type != SC2_JSON_OBJECT) {
        rc = SC2_ERR_FORMAT;
    }
    return rc;
}

static int get_u32(const sc2_json_val_t *obj, const char *key, uint32_t max, uint32_t *out)
{
    uint64_t v = 0;
    int rc = sc2_json_get_u64(obj, key, max, &v);
    *out = (uint32_t)v;
    return rc;
}

static int required(int rc)
{
    return rc == SC2_ERR_NOT_FOUND ? SC2_ERR_FORMAT : rc;
}

int sc2e_add_bundle(sc2e_t *e, const char *line, size_t len, char device_out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    sc2_json_val_t root, spk, opk;
    char type[SC2_TYPE_MAX + 1];
    uint8_t device[SC2_DEVICE_ID_LEN];
    uint8_t check[SC2_DEVICE_ID_LEN];
    sc2_bundle_t b;
    if (e == NULL || !e->open || line == NULL) {
        return SC2_ERR_ARG;
    }
    memset(&b, 0, sizeof(b));
    b.opk_id = SC2_NO_OPK;
    int rc = sc2_frame_peek(line, len, &root, type, NULL);
    if (rc == SC2_OK && strcmp(type, "keys.bundle") != 0) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_hex_exact(&root, "device", device, SC2_DEVICE_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_b64_exact(&root, "ik", b.ik, SC2_KEY_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_b64_exact(&root, "dk", b.dk, SC2_KEY_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_b64_exact(&root, "dk_sig", b.dk_sig, SC2_SIG_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(get_obj(&root, "spk", &spk));
    }
    if (rc == SC2_OK) {
        rc = required(get_u32(&spk, "id", UINT32_MAX, &b.spk_id));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_b64_exact(&spk, "pub", b.spk, SC2_KEY_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_b64_exact(&spk, "sig", b.spk_sig, SC2_SIG_LEN));
    }
    if (rc == SC2_OK) {
        rc = get_obj(&root, "opk", &opk);
        if (rc == SC2_OK) {
            rc = required(get_u32(&opk, "id", SC2_NO_OPK - 1u, &b.opk_id));
            if (rc == SC2_OK) {
                rc = required(sc2_json_get_b64_exact(&opk, "pub", b.opk, SC2_KEY_LEN));
            }
        } else if (rc == SC2_ERR_NOT_FOUND) {
            rc = SC2_OK;
        }
    }
    if (rc == SC2_OK) {
        rc = sc2_device_id(check, b.ik);
    }
    if (rc == SC2_OK && memcmp(check, device, SC2_DEVICE_ID_LEN) != 0) {
        rc = SC2_ERR_AUTH;
    }
    if (rc == SC2_OK && memcmp(device, e->self_id, SC2_DEVICE_ID_LEN) == 0) {
        rc = SC2_ERR_ARG;
    }
    if (rc == SC2_OK) {
        rc = sc2_bundle_verify(&b);
    }
    if (rc == SC2_OK) {
        rc = peer_save(e, &b);
    }
    if (rc == SC2_OK && device_out != NULL) {
        id_hex(device_out, device);
    }
    return rc;
}

int sc2e_peer_info(sc2e_t *e, const char *peer_hex, sc2e_peer_info_t *out)
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    char name[SC2E_NAME_MAX];
    sc2_bundle_t b;
    if (e == NULL || !e->open || out == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    int rc = parse_id(peer_hex, id);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = peer_load(e, id, &b);
    if (rc == SC2_ERR_NOT_FOUND) {
        return SC2_OK;
    }
    if (rc != SC2_OK) {
        return rc;
    }
    out->known = true;
    out->opk_available = b.opk_id != SC2_NO_OPK;
    out->spk_id = b.spk_id;
    rc = sc2_fingerprint_short(out->fingerprint, b.ik);
    if (rc == SC2_OK) {
        uint8_t *buf = ws_alloc(e, DM_REC_MAX);
        size_t n = 0;
        if (buf == NULL) {
            return SC2_ERR_BOUNDS;
        }
        rec_name(name, SC2E_DIR_DM, id);
        int lrc = e->pf.load(e->pf.ctx, name, buf, DM_REC_MAX, &n);
        out->has_session = lrc == SC2_OK;
        ws_release(e, buf, DM_REC_MAX);
    }
    return rc;
}

int sc2e_safety(sc2e_t *e, const char *peer_hex, sc2e_safety_t *out)
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    uint8_t h[32];
    sc2_bundle_t b;
    if (e == NULL || !e->open || out == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    int rc = parse_id(peer_hex, id);
    if (rc == SC2_OK) {
        rc = peer_load(e, id, &b);
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_hash(h, e->id.ik_pub, b.ik);
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_short(out->short_form, h);
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_full(out->full_form, h);
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_words(out->words, sizeof(out->words), h, ' ');
    }
    return rc;
}

int sc2e_forget_peer(sc2e_t *e, const char *peer_hex)
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    char name[SC2E_NAME_MAX];
    if (e == NULL || !e->open) {
        return SC2_ERR_ARG;
    }
    int rc = parse_id(peer_hex, id);
    if (rc != SC2_OK) {
        return rc;
    }
    rec_name(name, SC2E_DIR_DM, id);
    rc = e->pf.remove(e->pf.ctx, name);
    if (rc == SC2_OK) {
        rec_name(name, SC2E_DIR_PEER, id);
        rc = e->pf.remove(e->pf.ctx, name);
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* keys.publish                                                            */
/* ---------------------------------------------------------------------- */

int sc2e_keys_publish(sc2e_t *e, const char *rid, uint32_t opk_target, char *out, size_t cap, size_t *out_len)
{
    sc2_jw_t w;
    if (e == NULL || !e->open || out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    const bool with_opks = opk_target != SC2E_OPK_NONE;
    if (!with_opks) {
        opk_target = 0;
    } else if (opk_target > SC2E_OPK_MAX) {
        opk_target = SC2E_OPK_MAX;
    }
    sc2e_prekeys_t *pk = &e->pre;
    bool changed = false;
    int rc = SC2_OK;
    uint64_t now = now_s(e);
    if (now != 0 && pk->spk[0].created != 0 && now > pk->spk[0].created &&
        now - pk->spk[0].created >= SC2E_SPK_ROTATE_SECONDS) {
        rc = spk_rotate(e);
        changed = rc == SC2_OK;
    }
    while (rc == SC2_OK && pk->opk_count < opk_target) {
        uint32_t id = pk->next_opk_id;
        if (id == SC2_NO_OPK || id == 0) {
            id = 1;
        }
        rc = sc2_opk_generate(&pk->opk[pk->opk_count], id, &e->pf.rng);
        if (rc == SC2_OK) {
            pk->opk_count++;
            pk->next_opk_id = id + 1u;
            changed = true;
        }
    }
    if (rc == SC2_OK && changed) {
        rc = prekeys_save(e);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    const char *r = opt_rid(rid);
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "keys.publish");
    if (r != NULL) {
        sc2_jw_key(&w, "rid");
        sc2_jw_cstring(&w, r);
    }
    sc2_jw_key(&w, "dk");
    sc2_jw_b64(&w, e->id.dk_pub, SC2_KEY_LEN);
    sc2_jw_key(&w, "dk_sig");
    sc2_jw_b64(&w, e->id.dk_sig, SC2_SIG_LEN);
    sc2_jw_key(&w, "spk");
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "id");
    sc2_jw_u64(&w, pk->spk[0].spk.id);
    sc2_jw_key(&w, "pub");
    sc2_jw_b64(&w, pk->spk[0].spk.pub, SC2_KEY_LEN);
    sc2_jw_key(&w, "sig");
    sc2_jw_b64(&w, pk->spk[0].spk.sig, SC2_SIG_LEN);
    sc2_jw_object_end(&w);
    sc2_jw_key(&w, "opks");
    sc2_jw_array_begin(&w);
    for (uint32_t i = 0; with_opks && i < pk->opk_count; i++) {
        sc2_jw_object_begin(&w);
        sc2_jw_key(&w, "id");
        sc2_jw_u64(&w, pk->opk[i].id);
        sc2_jw_key(&w, "pub");
        sc2_jw_b64(&w, pk->opk[i].pub, SC2_KEY_LEN);
        sc2_jw_object_end(&w);
    }
    sc2_jw_array_end(&w);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

/* ---------------------------------------------------------------------- */
/* Inner plaintext checks                                                  */
/* ---------------------------------------------------------------------- */

static int inner_type(const char *inner, size_t len, sc2_json_val_t *root, char *t, size_t t_cap)
{
    if (inner == NULL || len == 0 || len > SC2E_INNER_MAX) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_json_root(inner, len, root);
    if (rc == SC2_OK && root->type != SC2_JSON_OBJECT) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_string(root, "t", t, t_cap, NULL));
        if (rc == SC2_ERR_BOUNDS) {
            rc = SC2_ERR_FORMAT;
        }
    }
    return rc;
}

static int is_control_type(const char *t)
{
    return strcmp(t, "epoch_key") == 0 || strcmp(t, "history") == 0 || strcmp(t, "removed") == 0;
}

int sc2e_inner_is_control(const char *inner, size_t len)
{
    sc2_json_val_t root;
    char t[24];
    if (inner_type(inner, len, &root, t, sizeof(t)) != SC2_OK) {
        return 0;
    }
    return is_control_type(t);
}

static int check_user_inner(const char *inner, size_t len)
{
    sc2_json_val_t root;
    char t[24];
    int rc = inner_type(inner, len, &root, t, sizeof(t));
    if (rc == SC2_OK && is_control_type(t)) {
        rc = SC2_ERR_ARG;
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* DM sessions                                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
    sc2_ratchet_t st;
    sc2_ratchet_t fresh;
    sc2_ratchet_t work;
    uint8_t rec[DM_REC_MAX];
    /* ciphertext in, plaintext out: decryption runs in place (the tag is
     * checked before anything is written, so a failed try leaves ct intact) */
    uint8_t ct[SC2_DM_MAX_CIPHERTEXT];
    uint8_t seen_ek[DM_SEEN_EK][SC2_KEY_LEN];
    uint8_t seen_count;
    sc2_bundle_t bundle;
} dm_ws_t;

static int dm_load(const sc2e_t *e, const uint8_t peer[SC2_DEVICE_ID_LEN], dm_ws_t *ws)
{
    char name[SC2E_NAME_MAX];
    const uint8_t *p;
    size_t n;
    rec_name(name, SC2E_DIR_DM, peer);
    int rc = rec_load(e, name, SC2E_REC_DM, ws->rec, sizeof(ws->rec), &p, &n);
    if (rc != SC2_OK) {
        return rc;
    }
    if (n < 3) {
        return SC2_ERR_FORMAT;
    }
    size_t blob_len = ((size_t)p[0] << 8) | p[1];
    if (blob_len + 3 > n) {
        return SC2_ERR_FORMAT;
    }
    uint8_t seen = p[2 + blob_len];
    if (seen > DM_SEEN_EK || n != 3 + blob_len + (size_t)seen * SC2_KEY_LEN) {
        return SC2_ERR_FORMAT;
    }
    rc = sc2_ratchet_deserialize(&ws->st, p + 2, blob_len);
    if (rc == SC2_OK && (memcmp(ws->st.peer_id, peer, SC2_DEVICE_ID_LEN) != 0 ||
                         memcmp(ws->st.self_id, e->self_id, SC2_DEVICE_ID_LEN) != 0)) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        ws->seen_count = seen;
        memcpy(ws->seen_ek, p + 3 + blob_len, (size_t)seen * SC2_KEY_LEN);
    }
    return rc;
}

static int dm_save(const sc2e_t *e, const uint8_t peer[SC2_DEVICE_ID_LEN], dm_ws_t *ws)
{
    char name[SC2E_NAME_MAX];
    size_t blob_len = 0;
    uint64_t now = now_s(e);
    if (now != 0) {
        (void)sc2_ratchet_expire(&ws->st, now);
    }
    int rc = sc2_ratchet_serialize(&ws->st, ws->rec + 14, sizeof(ws->rec) - 14 - SC2E_RECORD_OVERHEAD, &blob_len);
    if (rc != SC2_OK) {
        return rc;
    }
    size_t payload = 2 + blob_len + 1 + (size_t)ws->seen_count * SC2_KEY_LEN;
    if (payload + SC2E_RECORD_OVERHEAD > sizeof(ws->rec)) {
        return SC2_ERR_BOUNDS;
    }
    ws->rec[12] = (uint8_t)(blob_len >> 8);
    ws->rec[13] = (uint8_t)blob_len;
    ws->rec[14 + blob_len] = ws->seen_count;
    memcpy(ws->rec + 15 + blob_len, ws->seen_ek, (size_t)ws->seen_count * SC2_KEY_LEN);
    rec_name(name, SC2E_DIR_DM, peer);
    return rec_save_inplace(e, name, SC2E_REC_DM, ws->rec, sizeof(ws->rec), payload);
}

/* Seal inner bytes to peer with the stored or a new session. */
static int dm_seal(sc2e_t *e, const uint8_t peer[SC2_DEVICE_ID_LEN], const char *rid, const uint8_t *inner,
                   size_t inner_len, char *out, size_t cap, size_t *out_len)
{
    uint8_t sk[SC2_KEY_LEN];
    uint8_t ad[SC2_AD_LEN];
    sc2_x3dh_hdr_t xh;
    sc2_dm_env_t env;
    dm_ws_t *ws = ws_alloc(e, sizeof(*ws));
    bool consumed_opk = false;
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    if (memcmp(peer, e->self_id, SC2_DEVICE_ID_LEN) == 0) {
        ws_release(e, ws, sizeof(*ws));
        return SC2_ERR_ARG;
    }
    int rc = dm_load(e, peer, ws);
    if (rc == SC2_ERR_NOT_FOUND) {
        rc = peer_load(e, peer, &ws->bundle);
        if (rc == SC2_OK) {
            rc = sc2_x3dh_initiate(sk, ad, &xh, &e->id, &ws->bundle, &e->pf.rng);
        }
        if (rc == SC2_OK) {
            rc = sc2_ratchet_init_initiator(&ws->st, sk, ad, e->self_id, peer, ws->bundle.spk, &xh, &e->pf.rng);
        }
        consumed_opk = rc == SC2_OK && ws->bundle.opk_id != SC2_NO_OPK;
        ws->seen_count = 0;
    }
    memset(&env, 0, sizeof(env));
    if (rc == SC2_OK) {
        rc = sc2_ratchet_encrypt(&ws->st, &env.hdr, env.nonce, ws->ct, sizeof(ws->ct), &env.ct_len, inner, inner_len,
                                 &e->pf.rng);
    }
    if (rc == SC2_OK) {
        env.suite = SC2_SUITE_1;
        memcpy(env.from, e->self_id, SC2_DEVICE_ID_LEN);
        env.ct = ws->ct;
        rc = sc2_frame_dm_send(out, cap, out_len, opt_rid(rid), peer, &env);
    }
    if (rc == SC2_OK) {
        rc = dm_save(e, peer, ws);
    }
    if (rc == SC2_OK && consumed_opk) {
        /* An OPK is single use: never start a second session with it. */
        ws->bundle.opk_id = SC2_NO_OPK;
        memset(ws->bundle.opk, 0, SC2_KEY_LEN);
        rc = peer_save(e, &ws->bundle);
    }
    if (rc != SC2_OK && out != NULL && cap > 0) {
        sc2_memzero(out, cap < SC2E_FRAME_MAX ? cap : SC2E_FRAME_MAX);
        if (out_len) {
            *out_len = 0;
        }
    }
    sc2_memzero(sk, sizeof(sk));
    sc2_memzero(ad, sizeof(ad));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_dm_encrypt(sc2e_t *e, const char *peer_hex, const char *rid, const char *inner, size_t inner_len,
                    char *out, size_t cap, size_t *out_len)
{
    uint8_t peer[SC2_DEVICE_ID_LEN];
    if (e == NULL || !e->open || out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = parse_id(peer_hex, peer);
    if (rc == SC2_OK) {
        rc = check_user_inner(inner, inner_len);
    }
    if (rc == SC2_OK) {
        rc = dm_seal(e, peer, rid, (const uint8_t *)inner, inner_len, out, cap, out_len);
    }
    return rc;
}

static int find_opk(const sc2e_prekeys_t *pk, uint32_t id)
{
    for (uint32_t i = 0; i < pk->opk_count; i++) {
        if (pk->opk[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static const sc2_spk_t *find_spk(const sc2e_prekeys_t *pk, uint32_t id)
{
    for (size_t i = 0; i < 2; i++) {
        if (pk->spk[i].valid && pk->spk[i].spk.id == id) {
            return &pk->spk[i].spk;
        }
    }
    return NULL;
}

static int ek_seen(const dm_ws_t *ws, const uint8_t ek[SC2_KEY_LEN])
{
    for (uint8_t i = 0; i < ws->seen_count; i++) {
        if (sc2_ct_eq(ws->seen_ek[i], ek, SC2_KEY_LEN)) {
            return 1;
        }
    }
    return 0;
}

static void ek_remember(dm_ws_t *ws, const uint8_t ek[SC2_KEY_LEN])
{
    if (ws->seen_count == DM_SEEN_EK) {
        memmove(ws->seen_ek[0], ws->seen_ek[1], (DM_SEEN_EK - 1u) * SC2_KEY_LEN);
        ws->seen_count--;
    }
    memcpy(ws->seen_ek[ws->seen_count++], ek, SC2_KEY_LEN);
}

static int room_accept_key(sc2e_t *e, const uint8_t from[SC2_DEVICE_ID_LEN], const char *inner, size_t len,
                           sc2e_dm_result_t *res);
static int room_accept_history(sc2e_t *e, const uint8_t from[SC2_DEVICE_ID_LEN], const char *inner, size_t len,
                               sc2e_dm_result_t *res);

static int dm_open(sc2e_t *e, const char *line, size_t len, sc2e_dm_result_t *res, char *pt, size_t pt_cap,
                    size_t *pt_len, bool apply_controls)
{
    sc2_dm_env_t env;
    uint8_t sk[SC2_KEY_LEN];
    uint8_t ad[SC2_AD_LEN];
    size_t n = 0;
    int opk_index = -1;
    if (e == NULL || !e->open || line == NULL || res == NULL || pt == NULL || pt_len == NULL || pt_cap == 0) {
        return SC2_ERR_ARG;
    }
    memset(res, 0, sizeof(*res));
    *pt_len = 0;
    pt[0] = '\0';
    dm_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    uint64_t now = now_s(e);
    int rc = sc2_frame_dm_msg_decode(line, len, &res->seq, &env, ws->ct, sizeof(ws->ct));
    if (rc == SC2_OK && memcmp(env.from, e->self_id, SC2_DEVICE_ID_LEN) == 0) {
        rc = SC2_ERR_ARG;
    }
    bool have = false;
    if (rc == SC2_OK) {
        id_hex(res->from, env.from);
        int lrc = dm_load(e, env.from, ws);
        if (lrc == SC2_OK) {
            have = true;
        } else if (lrc != SC2_ERR_NOT_FOUND) {
            rc = lrc;
        }
    }
    int drc = SC2_ERR_STATE;
    if (rc == SC2_OK && have) {
        drc = sc2_ratchet_decrypt(&ws->st, &ws->work, &env.hdr, env.nonce, env.ct, env.ct_len, ws->ct,
                                  sizeof(ws->ct) - 1u, &n, now, &e->pf.rng);
    }
    if (rc == SC2_OK && drc != SC2_OK) {
        bool try_fresh = env.hdr.has_x3dh && (!have || drc == SC2_ERR_AUTH || drc == SC2_ERR_STATE);
        if (!try_fresh) {
            rc = drc;
        } else if (have && ek_seen(ws, env.hdr.x3dh.ek)) {
            rc = SC2_ERR_REPLAY;
        } else {
            const sc2_spk_t *spk = find_spk(&e->pre, env.hdr.x3dh.spk_id);
            const sc2_opk_t *opk = NULL;
            rc = spk ? SC2_OK : SC2_ERR_KEY;
            if (rc == SC2_OK && env.hdr.x3dh.opk_id != SC2_NO_OPK) {
                opk_index = find_opk(&e->pre, env.hdr.x3dh.opk_id);
                if (opk_index < 0) {
                    rc = SC2_ERR_REPLAY; /* one-time prekey already used or unknown */
                } else {
                    opk = &e->pre.opk[opk_index];
                }
            }
            if (rc == SC2_OK) {
                rc = peer_load(e, env.from, &ws->bundle);
            }
            if (rc == SC2_OK) {
                rc = sc2_x3dh_respond(sk, ad, &e->id, spk, opk, ws->bundle.ik, ws->bundle.dk, ws->bundle.dk_sig,
                                      &env.hdr.x3dh);
            }
            if (rc == SC2_OK) {
                rc = sc2_ratchet_init_responder(&ws->fresh, sk, ad, e->self_id, env.from, spk);
            }
            if (rc == SC2_OK) {
                rc = sc2_ratchet_decrypt(&ws->fresh, &ws->work, &env.hdr, env.nonce, env.ct, env.ct_len, ws->ct,
                                         sizeof(ws->ct) - 1u, &n, now, &e->pf.rng);
            }
            if (rc == SC2_OK) {
                ws->st = ws->fresh;
                if (!have) {
                    ws->seen_count = 0;
                }
                ek_remember(ws, env.hdr.x3dh.ek);
                res->new_session = true;
            } else {
                opk_index = -1;
            }
        }
    }
    if (rc == SC2_OK) {
        rc = dm_save(e, env.from, ws);
    }
    if (rc == SC2_OK && opk_index >= 0) {
        sc2e_prekeys_t *pk = &e->pre;
        sc2_memzero(&pk->opk[opk_index], sizeof(pk->opk[0]));
        memmove(&pk->opk[opk_index], &pk->opk[opk_index + 1],
                (pk->opk_count - (uint32_t)opk_index - 1u) * sizeof(pk->opk[0]));
        pk->opk_count--;
        sc2_memzero(&pk->opk[pk->opk_count], sizeof(pk->opk[0]));
        rc = prekeys_save_into(e, ws->rec); /* dm_save is done with ws->rec */
    }
    /* Key-carrying control messages are applied after the DM work area is
     * released, from a copy sized to the message, so a DM work area and a
     * room work area are never held at the same time. */
    char *ctl = NULL;
    int ctl_kind = 0; /* 1 epoch_key, 2 history */
    if (rc == SC2_OK) {
        sc2_json_val_t root;
        char t[24];
        ws->ct[n] = '\0';
        int trc = inner_type((const char *)ws->ct, n, &root, t, sizeof(t));
        if (trc == SC2_OK && strcmp(t, "epoch_key") == 0) {
            ctl_kind = 1;
        } else if (trc == SC2_OK && strcmp(t, "history") == 0) {
            ctl_kind = 2;
        }
        if (ctl_kind != 0 && apply_controls) {
            ctl = ws_alloc(e, n + 1u);
            if (ctl == NULL) {
                rc = SC2_ERR_BOUNDS;
            } else {
                memcpy(ctl, ws->ct, n + 1u);
            }
        } else if (n + 1u > pt_cap) {
            rc = SC2_ERR_BOUNDS;
        } else {
            res->kind = SC2E_DM_MESSAGE;
            memcpy(pt, ws->ct, n + 1u);
            *pt_len = n;
        }
    }
    sc2_memzero(sk, sizeof(sk));
    sc2_memzero(ad, sizeof(ad));
    ws_release(e, ws, sizeof(*ws));
    if (ctl != NULL) {
        rc = ctl_kind == 1 ? room_accept_key(e, env.from, ctl, n, res) : room_accept_history(e, env.from, ctl, n, res);
        ws_release(e, ctl, n + 1u);
    }
    return rc;
}

int sc2e_dm_decrypt(sc2e_t *e, const char *line, size_t len, sc2e_dm_result_t *res, char *pt, size_t pt_cap,
                    size_t *pt_len)
{
    return dm_open(e, line, len, res, pt, pt_cap, pt_len, true);
}

int sc2e_dm_open(sc2e_t *e, const char *line, size_t len, sc2e_dm_result_t *res, char *pt, size_t pt_cap,
                size_t *pt_len)
{
    return dm_open(e, line, len, res, pt, pt_cap, pt_len, false);
}

int sc2e_dm_apply_control(sc2e_t *e, sc2e_dm_result_t *res, const char *inner, size_t len)
{
    sc2_json_val_t root;
    char t[24];
    uint8_t from[SC2_DEVICE_ID_LEN];
    if (!e || !e->open || !res || parse_id(res->from, from) != SC2_OK ||
        inner_type(inner, len, &root, t, sizeof(t)) != SC2_OK) return SC2_ERR_ARG;
    if (strcmp(t, "epoch_key") == 0) return room_accept_key(e, from, inner, len, res);
    if (strcmp(t, "history") == 0) return room_accept_history(e, from, inner, len, res);
    return SC2_ERR_ARG;
}

int sc2e_peer_identity(sc2e_t *e, const char *peer_hex, uint8_t ik[SC2_KEY_LEN])
{
    uint8_t peer[SC2_DEVICE_ID_LEN];
    sc2_bundle_t bundle;
    if (!e || !e->open || !ik || parse_id(peer_hex, peer) != SC2_OK) return SC2_ERR_ARG;
    int rc = peer_load(e, peer, &bundle);
    if (rc == SC2_OK) memcpy(ik, bundle.ik, SC2_KEY_LEN);
    sc2_memzero(&bundle, sizeof(bundle));
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Rooms                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t sender[SC2_DEVICE_ID_LEN];
    sc2_room_chain_t chain;
} room_recv_t;

typedef struct {
    uint8_t valid;
    uint32_t epoch;
    uint8_t key[SC2_KEY_LEN];
    sc2_room_chain_t own;
    uint8_t nrecv;
    room_recv_t recv[SC2E_ROOM_SENDERS];
} room_slot_t;

typedef struct {
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t admin[SC2_DEVICE_ID_LEN];
    room_slot_t slot[SC2E_ROOM_EPOCH_SLOTS];
    uint8_t rec[ROOM_REC_MAX];
    uint8_t ct[SC2_DM_MAX_CIPHERTEXT]; /* opened in place, like dm_ws_t */
    sc2_bundle_t bundle;
} room_ws_t;

static int room_load(const sc2e_t *e, const uint8_t room[SC2_ROOM_ID_LEN], room_ws_t *ws)
{
    char name[SC2E_NAME_MAX];
    const uint8_t *p;
    size_t n;
    rec_name(name, SC2E_DIR_ROOM, room);
    int rc = rec_load(e, name, SC2E_REC_ROOM, ws->rec, sizeof(ws->rec), &p, &n);
    if (rc != SC2_OK) {
        return rc;
    }
    br_t r = {p, n, 0, 0};
    br_get(&r, ws->room, SC2_ROOM_ID_LEN);
    br_get(&r, ws->admin, SC2_DEVICE_ID_LEN);
    for (size_t s = 0; s < SC2E_ROOM_EPOCH_SLOTS; s++) {
        room_slot_t *sl = &ws->slot[s];
        sl->valid = br_u8(&r);
        sl->epoch = br_u32(&r);
        br_get(&r, sl->key, SC2_KEY_LEN);
        br_get(&r, sl->own.ck, SC2_KEY_LEN);
        sl->own.next = br_u32(&r);
        sl->nrecv = br_u8(&r);
        if (sl->valid > 1 || sl->nrecv > SC2E_ROOM_SENDERS) {
            r.err = SC2_ERR_FORMAT;
            break;
        }
        for (size_t i = 0; i < sl->nrecv; i++) {
            br_get(&r, sl->recv[i].sender, SC2_DEVICE_ID_LEN);
            br_get(&r, sl->recv[i].chain.ck, SC2_KEY_LEN);
            sl->recv[i].chain.next = br_u32(&r);
        }
    }
    rc = br_done(&r);
    if (rc == SC2_OK && memcmp(ws->room, room, SC2_ROOM_ID_LEN) != 0) {
        rc = SC2_ERR_FORMAT;
    }
    return rc;
}

static int room_save(const sc2e_t *e, room_ws_t *ws)
{
    char name[SC2E_NAME_MAX];
    bw_t w = {ws->rec + 12, sizeof(ws->rec) - SC2E_RECORD_OVERHEAD, 0, 0};
    bw_put(&w, ws->room, SC2_ROOM_ID_LEN);
    bw_put(&w, ws->admin, SC2_DEVICE_ID_LEN);
    for (size_t s = 0; s < SC2E_ROOM_EPOCH_SLOTS; s++) {
        const room_slot_t *sl = &ws->slot[s];
        bw_u8(&w, sl->valid);
        bw_u32(&w, sl->epoch);
        bw_put(&w, sl->key, SC2_KEY_LEN);
        bw_put(&w, sl->own.ck, SC2_KEY_LEN);
        bw_u32(&w, sl->own.next);
        bw_u8(&w, sl->nrecv);
        for (size_t i = 0; i < sl->nrecv; i++) {
            bw_put(&w, sl->recv[i].sender, SC2_DEVICE_ID_LEN);
            bw_put(&w, sl->recv[i].chain.ck, SC2_KEY_LEN);
            bw_u32(&w, sl->recv[i].chain.next);
        }
    }
    if (w.err) {
        return w.err;
    }
    rec_name(name, SC2E_DIR_ROOM, ws->room);
    return rec_save_inplace(e, name, SC2E_REC_ROOM, ws->rec, sizeof(ws->rec), w.len);
}

/* Install a new current epoch, keeping the previous one for late messages. */
static int room_install_epoch(const sc2e_t *e, room_ws_t *ws, uint32_t epoch, const uint8_t key[SC2_KEY_LEN])
{
    room_slot_t *cur = &ws->slot[0];
    if (cur->valid) {
        sc2_memzero(&ws->slot[1], sizeof(ws->slot[1]));
        ws->slot[1] = *cur;
    }
    sc2_memzero(cur, sizeof(*cur));
    cur->valid = 1;
    cur->epoch = epoch;
    memcpy(cur->key, key, SC2_KEY_LEN);
    return sc2_room_chain_init(&cur->own, key, ws->room, epoch, e->self_id);
}

int sc2e_room_create(sc2e_t *e, const char *room_hex, char room_out[SC2_DEVICE_ID_HEX_LEN + 1], uint32_t *epoch)
{
    uint8_t key[SC2_KEY_LEN];
    if (e == NULL || !e->open) {
        return SC2_ERR_ARG;
    }
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    int rc = SC2_OK;
    if (room_hex != NULL && room_hex[0] != '\0') {
        rc = parse_id(room_hex, ws->room);
    } else {
        rc = sc2_random(&e->pf.rng, ws->room, SC2_ROOM_ID_LEN);
    }
    if (rc == SC2_OK) {
        /* Refuse an existing room. The probe loads into ws itself (no second
         * work area), which is then wiped back to just the new room ID. */
        uint8_t id[SC2_ROOM_ID_LEN];
        memcpy(id, ws->room, sizeof(id));
        int lrc = room_load(e, id, ws);
        rc = lrc == SC2_ERR_NOT_FOUND ? SC2_OK : (lrc == SC2_OK ? SC2_ERR_STATE : lrc);
        sc2_memzero(ws, sizeof(*ws));
        memcpy(ws->room, id, sizeof(id));
    }
    if (rc == SC2_OK) {
        memcpy(ws->admin, e->self_id, SC2_DEVICE_ID_LEN);
        rc = sc2_random(&e->pf.rng, key, sizeof(key));
    }
    if (rc == SC2_OK) {
        rc = room_install_epoch(e, ws, 1u, key);
    }
    if (rc == SC2_OK) {
        rc = room_save(e, ws);
    }
    if (rc == SC2_OK) {
        if (room_out) {
            id_hex(room_out, ws->room);
        }
        if (epoch) {
            *epoch = 1u;
        }
    }
    sc2_memzero(key, sizeof(key));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_room_rotate(sc2e_t *e, const char *room_hex, uint32_t *epoch)
{
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t key[SC2_KEY_LEN];
    if (e == NULL || !e->open) {
        return SC2_ERR_ARG;
    }
    int rc = parse_id(room_hex, room);
    if (rc != SC2_OK) {
        return rc;
    }
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    rc = room_load(e, room, ws);
    if (rc == SC2_OK && memcmp(ws->admin, e->self_id, SC2_DEVICE_ID_LEN) != 0) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK && (!ws->slot[0].valid || ws->slot[0].epoch == UINT32_MAX)) {
        rc = SC2_ERR_STATE;
    }
    uint32_t next = rc == SC2_OK ? ws->slot[0].epoch + 1u : 0u;
    if (rc == SC2_OK) {
        rc = sc2_random(&e->pf.rng, key, sizeof(key));
    }
    if (rc == SC2_OK) {
        rc = room_install_epoch(e, ws, next, key);
    }
    if (rc == SC2_OK) {
        rc = room_save(e, ws);
    }
    if (rc == SC2_OK && epoch) {
        *epoch = next;
    }
    sc2_memzero(key, sizeof(key));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_room_share(sc2e_t *e, const char *room_hex, const char *peer_hex, const char *rid, char *out, size_t cap,
                    size_t *out_len)
{
    return sc2e_room_share_roster(e, room_hex, peer_hex, rid, NULL, out, cap, out_len);
}

int sc2e_room_share_roster(sc2e_t *e, const char *room_hex, const char *peer_hex, const char *rid,
                          const sc2_roster_t *roster, char *out, size_t cap, size_t *out_len)
{
    return sc2e_room_share_named(e, room_hex, peer_hex, rid, roster, NULL, out, cap, out_len);
}

bool sc2e_label_valid(const char *text, size_t max_bytes)
{
    if (!text) return false;
    size_t len = strlen(text);
    if (!len || len > max_bytes || !sc2_utf8_valid(text, len)) return false;
    bool visible = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch == 0x7f) return false;
        if (ch != ' ') visible = true;
    }
    return visible;
}

int sc2e_room_share_named(sc2e_t *e, const char *room_hex, const char *peer_hex, const char *rid,
                         const sc2_roster_t *roster, const char *title, char *out, size_t cap, size_t *out_len)
{
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t peer[SC2_DEVICE_ID_LEN];
    sc2_jw_t w;
    size_t inner_len = 0;
    if (e == NULL || !e->open || out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    if (title && (!roster || !sc2e_label_valid(title, SC2E_ROOM_TITLE_MAX))) return SC2_ERR_ARG;
    int rc = parse_id(room_hex, room);
    if (rc == SC2_OK) {
        rc = parse_id(peer_hex, peer);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    /* dm_seal encrypts into its private ciphertext buffer before writing
     * out. Reuse the caller's output for the inner JSON until that point. */
    char *inner = out;
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    rc = room_load(e, room, ws);
    if (rc == SC2_OK && (memcmp(ws->admin, e->self_id, SC2_DEVICE_ID_LEN) != 0 || !ws->slot[0].valid)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK && roster && (roster->count == 0 || roster->count > SC2_ROSTER_CAP ||
        memcmp(roster->room, room, sizeof(room)) || roster->epoch != ws->slot[0].epoch ||
        !sc2_roster_is_admin(roster, e->self_id) || sc2_roster_find(roster, peer) < 0)) rc = SC2_ERR_STATE;
    if (rc == SC2_OK) {
        sc2_jw_init(&w, inner, cap < ROOM_SHARE_INNER_MAX ? cap : ROOM_SHARE_INNER_MAX);
        sc2_jw_object_begin(&w);
        sc2_jw_key(&w, "t");
        sc2_jw_cstring(&w, "epoch_key");
        sc2_jw_key(&w, "room");
        sc2_jw_hex(&w, room, SC2_ROOM_ID_LEN);
        sc2_jw_key(&w, "epoch");
        sc2_jw_u64(&w, ws->slot[0].epoch);
        sc2_jw_key(&w, "key");
        sc2_jw_b64(&w, ws->slot[0].key, SC2_KEY_LEN);
        if (roster) {
            uint8_t members[17u * SC2_ROSTER_CAP];
            for (unsigned i = 0; i < roster->count; i++) {
                memcpy(members + 17u * i, roster->m[i].id, SC2_DEVICE_ID_LEN);
                members[17u * i + SC2_DEVICE_ID_LEN] = roster->m[i].role;
            }
            sc2_jw_key(&w, "head");
            sc2_jw_b64(&w, roster->head, SC2_COMMIT_ID_LEN);
            sc2_jw_key(&w, "roster");
            sc2_jw_b64(&w, members, 17u * roster->count);
            if (title) {
                sc2_jw_key(&w, "title");
                sc2_jw_cstring(&w, title);
            }
            sc2_memzero(members, sizeof(members));
        }
        sc2_jw_object_end(&w);
        rc = sc2_jw_finish(&w, &inner_len);
    }
    ws_release(e, ws, sizeof(*ws));
    if (rc == SC2_OK) {
        rc = dm_seal(e, peer, rid, (const uint8_t *)inner, inner_len, out, cap, out_len);
    }
    if (rc != SC2_OK) {
        sc2_memzero(out, cap < SC2E_FRAME_MAX ? cap : SC2E_FRAME_MAX);
        *out_len = 0;
    }
    return rc;
}

/* t:"epoch_key" policy (until signed commits exist): a room we do not know is
 * created with the sender as its admin; a known room only accepts newer
 * epochs from its admin. Anything else is ignored. */
static int room_accept_key(sc2e_t *e, const uint8_t from[SC2_DEVICE_ID_LEN], const char *inner, size_t len,
                           sc2e_dm_result_t *res)
{
    sc2_json_val_t root;
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t key[SC2_KEY_LEN];
    uint32_t epoch = 0;
    res->kind = SC2E_DM_IGNORED;
    int rc = sc2_json_root(inner, len, &root);
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_hex_exact(&root, "room", room, SC2_ROOM_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(get_u32(&root, "epoch", UINT32_MAX, &epoch));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_b64_exact(&root, "key", key, SC2_KEY_LEN));
    }
    if (rc == SC2_OK && epoch == 0) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc != SC2_OK) {
        sc2_memzero(key, sizeof(key));
        return SC2_OK; /* malformed control message: consumed, ignored */
    }
    id_hex(res->room, room);
    res->epoch = epoch;
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        sc2_memzero(key, sizeof(key));
        return SC2_ERR_BOUNDS;
    }
    rc = room_load(e, room, ws);
    bool accept = false;
    if (rc == SC2_ERR_NOT_FOUND) {
        memset(ws->slot, 0, sizeof(ws->slot));
        memcpy(ws->room, room, SC2_ROOM_ID_LEN);
        memcpy(ws->admin, from, SC2_DEVICE_ID_LEN);
        accept = true;
        rc = SC2_OK;
    } else if (rc == SC2_OK) {
        if (memcmp(ws->admin, from, SC2_DEVICE_ID_LEN) == 0 && ws->slot[0].valid) {
            if (epoch > ws->slot[0].epoch) {
                accept = true;
            } else if (epoch == ws->slot[0].epoch && sc2_ct_eq(key, ws->slot[0].key, SC2_KEY_LEN)) {
                res->kind = SC2E_DM_EPOCH_KEY; /* duplicate delivery */
            }
        }
    }
    if (rc == SC2_OK && accept) {
        rc = room_install_epoch(e, ws, epoch, key);
        if (rc == SC2_OK) {
            rc = room_save(e, ws);
        }
        if (rc == SC2_OK) {
            res->kind = SC2E_DM_EPOCH_KEY;
        }
    }
    sc2_memzero(key, sizeof(key));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

/* t:"history": opt-in share of older epoch keys for a room we already know.
 * Only the most recent older epoch fits (slot 1); it is installed if that
 * slot is empty or older. */
static int room_accept_history(sc2e_t *e, const uint8_t from[SC2_DEVICE_ID_LEN], const char *inner, size_t len,
                               sc2e_dm_result_t *res)
{
    sc2_json_val_t root, arr, item;
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t key[SC2_KEY_LEN];
    uint8_t best_key[SC2_KEY_LEN];
    uint32_t best_epoch = 0;
    size_t pos = 0;
    res->kind = SC2E_DM_IGNORED;
    int rc = sc2_json_root(inner, len, &root);
    if (rc == SC2_OK) {
        rc = required(sc2_json_get_hex_exact(&root, "room", room, SC2_ROOM_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = required(sc2_json_get(&root, "epochs", &arr));
        if (rc == SC2_OK && arr.type != SC2_JSON_ARRAY) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc != SC2_OK) {
        return SC2_OK;
    }
    id_hex(res->room, room);
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    rc = room_load(e, room, ws);
    if (rc == SC2_ERR_NOT_FOUND) {
        ws_release(e, ws, sizeof(*ws));
        return SC2_OK;
    }
    if (rc == SC2_OK && memcmp(ws->admin, from, SC2_DEVICE_ID_LEN) != 0) {
        ws_release(e, ws, sizeof(*ws));
        return SC2_OK;
    }
    while (rc == SC2_OK && sc2_json_array_next(&arr, &pos, &item) == SC2_OK) {
        uint32_t ep = 0;
        if (item.type != SC2_JSON_OBJECT || get_u32(&item, "epoch", UINT32_MAX, &ep) != SC2_OK ||
            sc2_json_get_b64_exact(&item, "key", key, SC2_KEY_LEN) != SC2_OK || ep == 0) {
            continue;
        }
        if (ws->slot[0].valid && ep < ws->slot[0].epoch && ep > best_epoch &&
            (!ws->slot[1].valid || ep > ws->slot[1].epoch)) {
            best_epoch = ep;
            memcpy(best_key, key, SC2_KEY_LEN);
        }
    }
    if (rc == SC2_OK && best_epoch != 0) {
        room_slot_t *old = &ws->slot[1];
        sc2_memzero(old, sizeof(*old));
        old->valid = 1;
        old->epoch = best_epoch;
        memcpy(old->key, best_key, SC2_KEY_LEN);
        rc = sc2_room_chain_init(&old->own, best_key, ws->room, best_epoch, e->self_id);
        if (rc == SC2_OK) {
            rc = room_save(e, ws);
        }
        if (rc == SC2_OK) {
            res->kind = SC2E_DM_HISTORY;
            res->epoch = best_epoch;
        }
    }
    sc2_memzero(key, sizeof(key));
    sc2_memzero(best_key, sizeof(best_key));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_room_encrypt(sc2e_t *e, const char *room_hex, const char *rid, const char *inner, size_t inner_len,
                      char *out, size_t cap, size_t *out_len)
{
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t mk[SC2_KEY_LEN];
    uint8_t hdr[SC2_ROOM_HDR_LEN];
    sc2_room_env_t env;
    if (e == NULL || !e->open || out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = parse_id(room_hex, room);
    if (rc == SC2_OK) {
        rc = check_user_inner(inner, inner_len);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    memset(&env, 0, sizeof(env));
    rc = room_load(e, room, ws);
    room_slot_t *cur = &ws->slot[0];
    if (rc == SC2_OK && !cur->valid) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2_room_chain_next(&cur->own, mk, &env.n);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_header(hdr, room, cur->epoch, e->self_id, env.n);
    }
    if (rc == SC2_OK) {
        rc = sc2_random(&e->pf.rng, env.nonce, SC2_NONCE_LEN);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_seal(ws->ct, sizeof(ws->ct), &env.ct_len, env.sig, mk, env.nonce, hdr, (const uint8_t *)inner,
                           inner_len, e->id.ik_seed);
    }
    if (rc == SC2_OK) {
        env.suite = SC2_SUITE_1;
        env.epoch = cur->epoch;
        memcpy(env.from, e->self_id, SC2_DEVICE_ID_LEN);
        env.ct = ws->ct;
        rc = sc2_frame_room_send(out, cap, out_len, opt_rid(rid), room, &env);
    }
    if (rc == SC2_OK) {
        rc = room_save(e, ws);
    }
    if (rc != SC2_OK && cap > 0) {
        out[0] = '\0';
        *out_len = 0;
    }
    sc2_memzero(mk, sizeof(mk));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_room_decrypt(sc2e_t *e, const char *line, size_t len, sc2e_room_result_t *res, char *pt, size_t pt_cap,
                      size_t *pt_len)
{
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t mk[SC2_KEY_LEN];
    uint8_t hdr[SC2_ROOM_HDR_LEN];
    const uint8_t *ik = NULL;
    sc2_room_env_t env;
    sc2_room_chain_t tmp;
    size_t n = 0;
    if (e == NULL || !e->open || line == NULL || res == NULL || pt == NULL || pt_len == NULL || pt_cap == 0) {
        return SC2_ERR_ARG;
    }
    memset(res, 0, sizeof(*res));
    *pt_len = 0;
    pt[0] = '\0';
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    int rc = sc2_frame_room_msg_decode(line, len, room, &res->seq, &env, ws->ct, sizeof(ws->ct));
    if (rc == SC2_OK) {
        id_hex(res->room, room);
        id_hex(res->from, env.from);
        res->epoch = env.epoch;
        res->n = env.n;
        rc = room_load(e, room, ws);
    }
    room_slot_t *sl = NULL;
    for (size_t s = 0; rc == SC2_OK && s < SC2E_ROOM_EPOCH_SLOTS; s++) {
        if (ws->slot[s].valid && ws->slot[s].epoch == env.epoch) {
            sl = &ws->slot[s];
        }
    }
    if (rc == SC2_OK && sl == NULL) {
        rc = SC2_ERR_STATE;
    }
    bool self = rc == SC2_OK && memcmp(env.from, e->self_id, SC2_DEVICE_ID_LEN) == 0;
    if (rc == SC2_OK) {
        if (self) {
            ik = e->id.ik_pub;
        } else {
            rc = peer_load(e, env.from, &ws->bundle);
            ik = ws->bundle.ik;
        }
    }
    room_recv_t *rv = NULL;
    if (rc == SC2_OK && !self) {
        for (size_t i = 0; i < sl->nrecv; i++) {
            if (memcmp(sl->recv[i].sender, env.from, SC2_DEVICE_ID_LEN) == 0) {
                rv = &sl->recv[i];
            }
        }
        if (rv == NULL) {
            if (sl->nrecv >= SC2E_ROOM_SENDERS) {
                rc = SC2_ERR_BOUNDS;
            } else {
                rc = sc2_room_chain_init(&tmp, sl->key, room, env.epoch, env.from);
            }
        } else {
            tmp = rv->chain;
        }
    } else if (rc == SC2_OK) {
        /* Our own message echoed back: derive from the epoch key without
         * touching the sending chain. */
        rc = sc2_room_chain_init(&tmp, sl->key, room, env.epoch, env.from);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_chain_key(&tmp, env.n, mk, 1);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_header(hdr, room, env.epoch, env.from, env.n);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_open(ws->ct, sizeof(ws->ct) - 1u, &n, mk, env.nonce, hdr, env.ct, env.ct_len, env.sig, ik);
    }
    if (rc == SC2_OK && !self) {
        if (rv == NULL) {
            rv = &sl->recv[sl->nrecv++];
            memcpy(rv->sender, env.from, SC2_DEVICE_ID_LEN);
        }
        rv->chain = tmp;
        rc = room_save(e, ws);
    }
    if (rc == SC2_OK) {
        if (n + 1u > pt_cap) {
            rc = SC2_ERR_BOUNDS;
        } else {
            ws->ct[n] = '\0';
            memcpy(pt, ws->ct, n + 1u);
            *pt_len = n;
        }
    }
    sc2_memzero(&tmp, sizeof(tmp));
    sc2_memzero(mk, sizeof(mk));
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_room_info(sc2e_t *e, const char *room_hex, sc2e_room_info_t *out)
{
    uint8_t room[SC2_ROOM_ID_LEN];
    if (e == NULL || !e->open || out == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    int rc = parse_id(room_hex, room);
    if (rc != SC2_OK) {
        return rc;
    }
    room_ws_t *ws = ws_alloc(e, sizeof(*ws));
    if (ws == NULL) {
        return SC2_ERR_BOUNDS;
    }
    rc = room_load(e, room, ws);
    if (rc == SC2_OK) {
        out->known = true;
        out->is_admin = memcmp(ws->admin, e->self_id, SC2_DEVICE_ID_LEN) == 0;
        id_hex(out->admin, ws->admin);
        out->epoch = ws->slot[0].valid ? ws->slot[0].epoch : 0u;
        out->sent = ws->slot[0].own.next;
        out->senders = ws->slot[0].nrecv;
    } else if (rc == SC2_ERR_NOT_FOUND) {
        rc = SC2_OK;
    }
    ws_release(e, ws, sizeof(*ws));
    return rc;
}

int sc2e_room_forget(sc2e_t *e, const char *room_hex)
{
    uint8_t room[SC2_ROOM_ID_LEN];
    char name[SC2E_NAME_MAX];
    if (e == NULL || !e->open) {
        return SC2_ERR_ARG;
    }
    int rc = parse_id(room_hex, room);
    if (rc == SC2_OK) {
        rec_name(name, SC2E_DIR_ROOM, room);
        rc = e->pf.remove(e->pf.ctx, name);
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Public posts                                                            */
/* ---------------------------------------------------------------------- */

int sc2e_post_sign(sc2e_t *e, const char *relay_id, const char *room, const char *name, const char *body,
                   size_t body_len, uint64_t ts, const char *ref_hex, bool deleted, const char *rid, char *out,
                   size_t cap, size_t *out_len)
{
    sc2_post_t post;
    if (e == NULL || !e->open || relay_id == NULL || room == NULL || out == NULL || out_len == NULL ||
        (body == NULL && body_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (name == NULL) {
        name = "";
    }
    size_t name_len = strlen(name);
    if (name_len > SC2_POST_NAME_MAX || body_len > SC2_POST_BODY_MAX || strlen(room) > SC2_ROOM_NAME_MAX ||
        strlen(relay_id) > SC2_RELAY_ID_MAX || !sc2_utf8_valid(name, name_len) ||
        (body_len && !sc2_utf8_valid(body, body_len))) {
        return SC2_ERR_ARG;
    }
    memset(&post, 0, sizeof(post));
    post.suite = SC2_SUITE_1;
    post.t = deleted ? SC2_POST_T_DEL : SC2_POST_T_TEXT;
    post.ts = ts;
    memcpy(post.name, name, name_len);
    post.name_len = name_len;
    post.body = body_len ? body : "";
    post.body_len = body_len;
    int rc = SC2_OK;
    if (ref_hex != NULL && ref_hex[0] != '\0') {
        if (strlen(ref_hex) != 2u * SC2_MSG_ID_LEN ||
            sc2_hex_decode(post.ref, SC2_MSG_ID_LEN, ref_hex, 2u * SC2_MSG_ID_LEN) != SC2_OK) {
            return SC2_ERR_ARG;
        }
        post.has_ref = true;
    } else if (deleted) {
        return SC2_ERR_ARG; /* a retraction must reference the post */
    }
    rc = sc2_random(&e->pf.rng, post.id, SC2_MSG_ID_LEN);
    if (rc == SC2_OK) {
        rc = sc2_post_sign(&post, relay_id, room, e->id.ik_seed);
    }
    if (rc == SC2_OK) {
        rc = sc2_frame_room_post(out, cap, out_len, opt_rid(rid), room, &post);
    }
    return rc;
}

int sc2e_post_verify(const char *relay_id, const char *line, size_t len, size_t max_body, sc2e_post_result_t *res,
                     char *body_buf, size_t body_cap)
{
    sc2_post_t post;
    if (relay_id == NULL || line == NULL || res == NULL || body_buf == NULL || body_cap == 0) {
        return SC2_ERR_ARG;
    }
    memset(res, 0, sizeof(*res));
    if (max_body == 0 || max_body > SC2_POST_BODY_MAX) {
        max_body = SC2_POST_BODY_MAX;
    }
    int rc = sc2_frame_room_pub_decode(line, len, res->room, &res->seq, &post, body_buf, body_cap);
    if (rc != SC2_OK) {
        return rc;
    }
    id_hex(res->from, post.from);
    (void)sc2_hex_encode(res->id, sizeof(res->id), post.id, SC2_MSG_ID_LEN);
    if (post.has_ref) {
        (void)sc2_hex_encode(res->ref, sizeof(res->ref), post.ref, SC2_MSG_ID_LEN);
    }
    memcpy(res->name, post.name, post.name_len);
    res->name[post.name_len] = '\0';
    res->ts = post.ts;
    res->deleted = post.t == SC2_POST_T_DEL;
    res->body = body_buf;
    res->body_len = post.body_len;
    rc = sc2_post_verify(&post, relay_id, res->room, max_body);
    res->valid = rc == SC2_OK;
    return rc;
}
