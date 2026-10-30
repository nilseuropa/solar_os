/*
 * SC2 portable core: minimal relay. See sc2_minirelay.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sc2_minirelay.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"
#include "sc2_frame.h"
#include "sc2_json.h"
#include "sc2_relaykey.h"

#define MR_RELAY_ID_MAX 64u
#define MR_TX_MAX (SC2_MR_DM_MAX + 256u)
#define MR_SCRATCH_MAX SC2_MR_DM_MAX
#define MR_OPK_WIRE_MAX 100u
#define MR_SYNC_MAX 500u
#define MR_LINE_LIMIT 16384u

enum { ST_FREE = 0, ST_NEW, ST_WELCOMED, ST_AUTHED, ST_V1 };

typedef struct {
    uint8_t used;
    uint8_t has_keys;
    uint8_t opk_n;
    uint8_t id[SC2_DEVICE_ID_LEN];
    char hex[SC2_DEVICE_ID_HEX_LEN + 1];
    uint8_t ik[SC2_KEY_LEN];
    uint8_t dk[SC2_KEY_LEN];
    uint8_t dk_sig[SC2_SIG_LEN];
    uint32_t spk_id;
    uint8_t spk[SC2_KEY_LEN];
    uint8_t spk_sig[SC2_SIG_LEN];
    uint32_t opk_id[SC2_MR_OPKS];
    uint8_t opk[SC2_MR_OPKS][SC2_KEY_LEN];
    uint64_t inbox_seq; /* last mailbox seq given out */
    uint64_t seen;
} mr_dev_t;

typedef struct {
    uint8_t used;
    uint8_t dev;
    uint16_t len;
    uint32_t n;
    uint64_t seq;
    uint8_t from[SC2_DEVICE_ID_LEN];
    uint8_t rk[SC2_KEY_LEN];
    char env[SC2_MR_DM_MAX + 1];
} mr_inbox_t;

typedef struct {
    uint64_t seq; /* 0: empty */
    uint8_t from[SC2_DEVICE_ID_LEN];
    uint8_t id[SC2_MSG_ID_LEN];
    uint16_t len;
    char post[SC2_MR_POST_MAX + 1];
} mr_post_t;

typedef struct {
    uint8_t state;
    uint8_t live;
    uint8_t joined;
    int8_t dev;
    uint64_t opened; /* slot clock at sc2_mr_open() */
    uint64_t active; /* slot clock at the last frame other than ping/pong */
    uint8_t challenge[SC2_AUTH_CHALLENGE_LEN];
    char admit[SC2_MR_ADMIT_MAX + 1];
} mr_sess_t;

struct sc2_mr {
    char relay_id[MR_RELAY_ID_MAX + 1];
    char room[SC2_ROOM_NAME_MAX + 1];
    char admit[SC2_MR_ADMIT_MAX + 1];
    uint8_t seed[SC2_KEY_LEN];
    uint8_t pub[SC2_KEY_LEN];
    sc2_rng_t rng;
    uint64_t (*now_s)(void *ctx);
    uint64_t (*mono_s)(void *ctx);
    int (*send)(void *ctx, unsigned session, const char *line, size_t len);
    void (*kick)(void *ctx, unsigned session);
    void (*release)(void *ctx, void *ptr, size_t len);
    void *ctx;
    bool legacy_v1;
    bool has_admit;
    unsigned nsess;
    unsigned idle_s;
    uint64_t pub_seq;
    unsigned pub_head; /* next ring slot */
    uint64_t dm_total, posts_total, refused_total;
    uint64_t replaced_total, idle_total, evicted_total;
    mr_sess_t sess[SC2_MR_SESSIONS_MAX];
    mr_dev_t dev[SC2_MR_DEVICES];
    mr_inbox_t inbox[SC2_MR_INBOX];
    mr_post_t posts[SC2_MR_PUB_HISTORY];
    char tx[MR_TX_MAX];
    uint8_t scratch[MR_SCRATCH_MAX + 1];
};

size_t sc2_mr_size(void)
{
    return sizeof(sc2_mr_t);
}

/* ---------------- output ---------------- */

typedef struct {
    char *b;
    size_t cap, n;
    bool ok;
} ob_t;

static void ob_init(ob_t *o, sc2_mr_t *mr)
{
    o->b = mr->tx;
    o->cap = sizeof(mr->tx);
    o->n = 0;
    o->ok = true;
}

static void ob_raw(ob_t *o, const char *p, size_t len)
{
    if (!o->ok || o->n + len >= o->cap) {
        o->ok = false;
        return;
    }
    memcpy(o->b + o->n, p, len);
    o->n += len;
    o->b[o->n] = '\0';
}

/* Bounded NUL-terminated copy (sources are validated, truncation is a bug
 * the caller already ruled out). */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1U;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void ob_s(ob_t *o, const char *s)
{
    ob_raw(o, s, strlen(s));
}

static void ob_u64(ob_t *o, uint64_t v)
{
    char t[24];
    size_t n = sizeof(t);
    do {
        t[--n] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while (v != 0U);
    ob_raw(o, t + n, sizeof(t) - n);
}

static void ob_seq(ob_t *o, uint64_t v)
{
    ob_s(o, "\"");
    ob_u64(o, v);
    ob_s(o, "\"");
}

static void ob_b64(ob_t *o, const uint8_t *d, size_t len)
{
    size_t need = SC2_B64_ENCODED_LEN(len) + 3u;
    size_t w = 0;
    if (!o->ok || o->n + need >= o->cap) {
        o->ok = false;
        return;
    }
    o->b[o->n++] = '"';
    if (sc2_b64_encode(o->b + o->n, o->cap - o->n, &w, d, len) != SC2_OK) {
        o->ok = false;
        return;
    }
    o->n += w;
    o->b[o->n++] = '"';
    o->b[o->n] = '\0';
}

/* {"type":"T"[,"rid":"R"] -- the caller continues with ,"k":v and closes with } */
static void ob_head(ob_t *o, sc2_mr_t *mr, const char *type, const char *rid)
{
    ob_init(o, mr);
    ob_s(o, "{\"type\":\"");
    ob_s(o, type);
    ob_s(o, "\"");
    if (rid != NULL && rid[0] != '\0') {
        ob_s(o, ",\"rid\":\""); /* sc2_mr_line checked it (rid_safe) */
        ob_s(o, rid);
        ob_s(o, "\"");
    }
}

static void ob_send(sc2_mr_t *mr, unsigned s, ob_t *o)
{
    if (o->ok && mr->sess[s].state != ST_FREE) {
        (void)mr->send(mr->ctx, s, o->b, o->n);
    }
}

static void send_simple(sc2_mr_t *mr, unsigned s, const char *type, const char *rid)
{
    ob_t o;
    ob_head(&o, mr, type, rid);
    ob_s(&o, "}");
    ob_send(mr, s, &o);
}

static void err_frame(sc2_mr_t *mr, unsigned s, const char *rid, unsigned code, const char *err, const char *text)
{
    ob_t o;
    ob_head(&o, mr, "error", rid);
    ob_s(&o, ",\"code\":");
    ob_u64(&o, code);
    ob_s(&o, ",\"err\":\"");
    ob_s(&o, err);
    ob_s(&o, "\",\"text\":\"");
    ob_s(&o, text);
    ob_s(&o, "\"}");
    ob_send(mr, s, &o);
}

/* Error frame; returns SC2_MR_CLOSE when close is set. */
static int send_err(sc2_mr_t *mr, unsigned s, const char *rid, unsigned code, const char *err, const char *text,
                    bool close)
{
    err_frame(mr, s, rid, code, err, text);
    if (code == 403 || code == 503 || code == 507 || code == 413) {
        mr->refused_total++;
    }
    return close ? SC2_MR_CLOSE : SC2_MR_OK;
}

/* ---------------- connection slots ---------------- */

static bool has_clock(const sc2_mr_t *mr)
{
    return mr->mono_s != NULL || mr->now_s != NULL;
}

static uint64_t slot_clock(const sc2_mr_t *mr)
{
    return mr->mono_s != NULL ? mr->mono_s(mr->ctx) : mr->now_s != NULL ? mr->now_s(mr->ctx) : 0;
}

static uint64_t since(uint64_t now, uint64_t then)
{
    return now > then ? now - then : 0; /* a clock that went back counts as no time */
}

/* Close session k on the relay's own initiative: final error frame (no
 * rid), release the slot, tell the host. */
static void kick(sc2_mr_t *mr, unsigned k, unsigned code, const char *err, const char *text)
{
    err_frame(mr, k, NULL, code, err, text);
    sc2_memzero(&mr->sess[k], sizeof(mr->sess[k]));
    mr->sess[k].dev = -1;
    if (mr->kick != NULL) {
        mr->kick(mr->ctx, k);
    }
}

/* ---------------- lookups ---------------- */

static int dev_find(sc2_mr_t *mr, const uint8_t id[SC2_DEVICE_ID_LEN])
{
    for (unsigned i = 0; i < SC2_MR_DEVICES; i++) {
        if (mr->dev[i].used && memcmp(mr->dev[i].id, id, SC2_DEVICE_ID_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int dev_find_hex(sc2_mr_t *mr, const sc2_json_val_t *root, const char *key)
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    if (sc2_json_get_hex_exact(root, key, id, sizeof(id)) != SC2_OK) {
        return -2;
    }
    return dev_find(mr, id);
}

/* Each live session of device d (except one). */
static void push_dev(sc2_mr_t *mr, int d, int except, ob_t *o)
{
    if (!o->ok) {
        return;
    }
    for (unsigned i = 0; i < mr->nsess; i++) {
        if ((int)i != except && mr->sess[i].state == ST_AUTHED && mr->sess[i].live && mr->sess[i].dev == d) {
            (void)mr->send(mr->ctx, i, o->b, o->n);
        }
    }
}

static unsigned inbox_count(const sc2_mr_t *mr, int d)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SC2_MR_INBOX; i++) {
        n += mr->inbox[i].used && mr->inbox[i].dev == (uint8_t)d;
    }
    return n;
}

static void keys_low(sc2_mr_t *mr, int d, int s_only)
{
    ob_t o;
    ob_head(&o, mr, "keys.low", NULL);
    ob_s(&o, ",\"remaining\":");
    ob_u64(&o, mr->dev[d].opk_n);
    ob_s(&o, "}");
    if (s_only >= 0) {
        ob_send(mr, (unsigned)s_only, &o);
    } else {
        push_dev(mr, d, -1, &o);
    }
}

static void dm_frame(sc2_mr_t *mr, ob_t *o, const mr_inbox_t *in)
{
    ob_head(o, mr, "dm.msg", NULL);
    ob_s(o, ",\"env\":");
    ob_raw(o, in->env, in->len);
    ob_s(o, ",\"seq\":");
    ob_seq(o, in->seq);
    ob_s(o, "}");
}

static void post_frame(sc2_mr_t *mr, ob_t *o, const mr_post_t *p)
{
    ob_head(o, mr, "room.pub", NULL);
    ob_s(o, ",\"room\":\"");
    ob_s(o, mr->room);
    ob_s(o, "\",\"seq\":");
    ob_seq(o, p->seq);
    ob_s(o, ",\"post\":");
    ob_raw(o, p->post, p->len);
    ob_s(o, "}");
}

static uint64_t pub_oldest(const sc2_mr_t *mr)
{
    uint64_t lo = 0;
    for (unsigned i = 0; i < SC2_MR_PUB_HISTORY; i++) {
        uint64_t q = mr->posts[i].seq;
        if (q != 0 && (lo == 0 || q < lo)) {
            lo = q;
        }
    }
    return lo;
}

static const mr_post_t *pub_after(const sc2_mr_t *mr, uint64_t after)
{
    const mr_post_t *best = NULL;
    for (unsigned i = 0; i < SC2_MR_PUB_HISTORY; i++) {
        const mr_post_t *p = &mr->posts[i];
        if (p->seq > after && (best == NULL || p->seq < best->seq)) {
            best = p;
        }
    }
    return best;
}

static const mr_inbox_t *inbox_after(const sc2_mr_t *mr, int d, uint64_t after)
{
    const mr_inbox_t *best = NULL;
    for (unsigned i = 0; i < SC2_MR_INBOX; i++) {
        const mr_inbox_t *in = &mr->inbox[i];
        if (in->used && in->dev == (uint8_t)d && in->seq > after && (best == NULL || in->seq < best->seq)) {
            best = in;
        }
    }
    return best;
}

static unsigned room_members(const sc2_mr_t *mr)
{
    unsigned n = 0;
    for (unsigned i = 0; i < mr->nsess; i++) {
        n += mr->sess[i].state == ST_AUTHED && mr->sess[i].joined;
    }
    return n;
}

static bool get_seq(const sc2_json_val_t *obj, const char *key, uint64_t def, uint64_t *out)
{
    sc2_json_val_t v;
    if (sc2_json_get(obj, key, &v) != SC2_OK) {
        *out = def;
        return true;
    }
    return v.type == SC2_JSON_STRING && sc2_json_u64_str(&v, out) == SC2_OK;
}

/* ---------------- handshake ---------------- */

static int h_hello(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    mr_sess_t *ss = &mr->sess[s];
    sc2_json_val_t v;
    if (sc2_json_get(root, "proto", &v) != SC2_OK || !sc2_json_string_eq(&v, "sc2")) {
        if (mr->legacy_v1) {
            ss->state = ST_V1;
            return SC2_MR_LEGACY;
        }
        return send_err(mr, s, rid, 426, "upgrade_required", "this relay speaks SC2 v2 only", true);
    }
    uint64_t ver = 0;
    if (sc2_json_get_u64(root, "v", 255, &ver) != SC2_OK || ver < 1) {
        return send_err(mr, s, rid, 400, "bad_request", "v must be 1..255", true);
    }
    if (sc2_json_get(root, "suites", &v) == SC2_OK) {
        sc2_json_val_t it;
        size_t pos = 0;
        bool ours = false;
        while (v.type == SC2_JSON_ARRAY && sc2_json_array_next(&v, &pos, &it) == SC2_OK) {
            uint64_t su = 0;
            ours = ours || (sc2_json_u64(&it, 255, &su) == SC2_OK && su == SC2_SUITE_1);
        }
        if (!ours) {
            return send_err(mr, s, rid, 501, "not_supported", "no common suite", true);
        }
    }
    ss->admit[0] = '\0';
    if (sc2_json_get(root, "admit", &v) == SC2_OK && v.type == SC2_JSON_STRING &&
        sc2_json_string(&v, ss->admit, sizeof(ss->admit), NULL) != SC2_OK) {
        return send_err(mr, s, rid, 400, "bad_request", "admit too long", true);
    }
    uint8_t nonce[SC2_HELLO_NONCE_LEN];
    bool has_nonce = false;
    if (sc2_json_get(root, "nonce", &v) == SC2_OK && v.type != SC2_JSON_NULL) {
        if (sc2_json_b64_exact(&v, nonce, sizeof(nonce)) != SC2_OK) {
            return send_err(mr, s, rid, 400, "bad_request", "nonce must be 16 bytes", true);
        }
        has_nonce = true;
    }
    if (sc2_random(&mr->rng, ss->challenge, sizeof(ss->challenge)) != SC2_OK) {
        return send_err(mr, s, rid, 503, "server_full", "no randomness", true);
    }
    uint8_t sig[SC2_SIG_LEN];
    if (has_nonce && sc2_welcome_sign(sig, mr->seed, mr->relay_id, nonce, ss->challenge) != SC2_OK) {
        return send_err(mr, s, rid, 503, "server_full", "signing failed", true);
    }
    ob_t o;
    ob_head(&o, mr, "welcome", rid);
    ob_s(&o, ",\"v\":2,\"proto\":\"sc2\",\"suite\":1,\"relay\":{\"id\":\"");
    ob_s(&o, mr->relay_id);
    ob_s(&o, "\",\"key\":");
    ob_b64(&o, mr->pub, sizeof(mr->pub));
    ob_s(&o, "},\"challenge\":");
    ob_b64(&o, ss->challenge, sizeof(ss->challenge));
    ob_s(&o, mr->legacy_v1 ? ",\"legacy_v1\":true" : ",\"legacy_v1\":false");
    ob_s(&o, ",\"limits\":{\"line\":");
    ob_u64(&o, MR_LINE_LIMIT);
    ob_s(&o, ",\"msg\":");
    ob_u64(&o, SC2_MR_PUB_BODY_MAX);
    ob_s(&o, ",\"rooms\":0,\"members\":0,\"queue\":");
    ob_u64(&o, SC2_MR_INBOX_PER_DEVICE);
    ob_s(&o, ",\"queue_bytes\":");
    ob_u64(&o, (uint64_t)SC2_MR_INBOX_PER_DEVICE * SC2_MR_DM_MAX);
    ob_s(&o, ",\"ttl_days\":0}");
    if (has_nonce) {
        ob_s(&o, ",\"relay_sig\":");
        ob_b64(&o, sig, sizeof(sig));
    }
    ob_s(&o, "}");
    ob_send(mr, s, &o);
    ss->state = ST_WELCOMED;
    return SC2_MR_OK;
}

static int h_auth(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    mr_sess_t *ss = &mr->sess[s];
    if (ss->state != ST_WELCOMED) {
        return send_err(mr, s, rid, 400, "bad_request", "already authenticated", false);
    }
    uint8_t ik[SC2_KEY_LEN], sig[SC2_SIG_LEN], id[SC2_DEVICE_ID_LEN];
    if (sc2_json_get_b64_exact(root, "ik", ik, sizeof(ik)) != SC2_OK ||
        sc2_json_get_b64_exact(root, "sig", sig, sizeof(sig)) != SC2_OK) {
        return send_err(mr, s, rid, 400, "bad_request", "ik and sig required", true);
    }
    if (sc2_auth_verify(sig, ik, mr->relay_id, strlen(mr->relay_id), ss->challenge) != SC2_OK ||
        sc2_device_id(id, ik) != SC2_OK) {
        return send_err(mr, s, rid, 401, "bad_sig", "challenge signature invalid", true);
    }
    int d = dev_find(mr, id);
    bool fresh = d < 0;
    if (fresh) {
        if (mr->has_admit && (strlen(ss->admit) != strlen(mr->admit) ||
                              !sc2_ct_eq(ss->admit, mr->admit, strlen(mr->admit)))) {
            return send_err(mr, s, rid, 403, "admit_required", "admission code required", true);
        }
        for (unsigned i = 0; i < SC2_MR_DEVICES && d < 0; i++) {
            if (!mr->dev[i].used) {
                d = (int)i;
            }
        }
        if (d < 0) {
            return send_err(mr, s, rid, 503, "server_full", "device table full", true);
        }
        mr_dev_t *dv = &mr->dev[d];
        memset(dv, 0, sizeof(*dv));
        dv->used = 1;
        memcpy(dv->id, id, sizeof(id));
        memcpy(dv->ik, ik, sizeof(ik));
        (void)sc2_hex_encode(dv->hex, sizeof(dv->hex), id, sizeof(id));
    }
    /* one live session per device key: a new login replaces the older one
     * (a reconnect after a network change often leaves a half-open socket) */
    for (unsigned i = 0; !fresh && i < mr->nsess; i++) {
        if (i != s && mr->sess[i].state == ST_AUTHED && mr->sess[i].dev == d) {
            kick(mr, i, 409, "replaced", "session replaced by a newer login of this device");
            mr->replaced_total++;
        }
    }
    mr_dev_t *dv = &mr->dev[d];
    uint64_t now = mr->now_s != NULL ? mr->now_s(mr->ctx) : 0;
    dv->seen = now;
    ss->dev = (int8_t)d;
    ss->state = ST_AUTHED;
    ss->live = 0;
    ss->joined = 0;
    sc2_memzero(ss->challenge, sizeof(ss->challenge));
    ob_t o;
    ob_head(&o, mr, "authed", rid);
    ob_s(&o, ",\"device\":\"");
    ob_s(&o, dv->hex);
    ob_s(&o, fresh ? "\",\"new\":true,\"server_time\":" : "\",\"new\":false,\"server_time\":");
    ob_u64(&o, now);
    ob_s(&o, "}");
    ob_send(mr, s, &o);
    if (dv->has_keys && dv->opk_n < SC2_MR_KEYS_LOW) {
        keys_low(mr, d, (int)s);
    }
    return SC2_MR_OK;
}

/* ---------------- keys ---------------- */

static int h_keys_publish(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    mr_dev_t *dv = &mr->dev[mr->sess[s].dev];
    uint8_t dk[SC2_KEY_LEN], dk_sig[SC2_SIG_LEN], spk[SC2_KEY_LEN], spk_sig[SC2_SIG_LEN];
    uint64_t spk_id = 0;
    sc2_json_val_t so, opks, it;
    if (sc2_json_get_b64_exact(root, "dk", dk, sizeof(dk)) != SC2_OK ||
        sc2_json_get_b64_exact(root, "dk_sig", dk_sig, sizeof(dk_sig)) != SC2_OK ||
        sc2_json_get(root, "spk", &so) != SC2_OK || so.type != SC2_JSON_OBJECT ||
        sc2_json_get_u64(&so, "id", UINT32_MAX, &spk_id) != SC2_OK ||
        sc2_json_get_b64_exact(&so, "pub", spk, sizeof(spk)) != SC2_OK ||
        sc2_json_get_b64_exact(&so, "sig", spk_sig, sizeof(spk_sig)) != SC2_OK) {
        return send_err(mr, s, rid, 400, "bad_request", "dk, dk_sig and spk required", false);
    }
    if (sc2_dk_verify(dk_sig, dv->ik, dk) != SC2_OK) {
        return send_err(mr, s, rid, 401, "bad_sig", "dk_sig invalid", false);
    }
    if (sc2_spk_verify(spk_sig, dv->ik, (uint32_t)spk_id, spk) != SC2_OK) {
        return send_err(mr, s, rid, 401, "bad_sig", "spk signature invalid", false);
    }
    /* validate the whole list before changing anything */
    uint32_t ids[MR_OPK_WIRE_MAX];
    size_t count = 0, pos = 0;
    bool have_list = sc2_json_get(root, "opks", &opks) == SC2_OK;
    if (have_list && opks.type != SC2_JSON_ARRAY) {
        return send_err(mr, s, rid, 400, "bad_request", "opks must be a list", false);
    }
    while (have_list && sc2_json_array_next(&opks, &pos, &it) == SC2_OK) {
        uint64_t oid = 0;
        uint8_t pub[SC2_KEY_LEN];
        if (count == MR_OPK_WIRE_MAX) {
            return send_err(mr, s, rid, 413, "too_large", "at most 100 one-time prekeys", false);
        }
        if (it.type != SC2_JSON_OBJECT || sc2_json_get_u64(&it, "id", UINT32_MAX - 1u, &oid) != SC2_OK ||
            sc2_json_get_b64_exact(&it, "pub", pub, sizeof(pub)) != SC2_OK) {
            return send_err(mr, s, rid, 400, "bad_request", "bad opk", false);
        }
        for (size_t j = 0; j < count; j++) {
            if (ids[j] == (uint32_t)oid) {
                return send_err(mr, s, rid, 400, "bad_request", "duplicate opk id", false);
            }
        }
        ids[count++] = (uint32_t)oid;
    }
    memcpy(dv->dk, dk, sizeof(dk));
    memcpy(dv->dk_sig, dk_sig, sizeof(dk_sig));
    dv->spk_id = (uint32_t)spk_id;
    memcpy(dv->spk, spk, sizeof(spk));
    memcpy(dv->spk_sig, spk_sig, sizeof(spk_sig));
    dv->has_keys = 1;
    pos = 0;
    while (have_list && sc2_json_array_next(&opks, &pos, &it) == SC2_OK) {
        uint64_t oid = 0;
        uint8_t pub[SC2_KEY_LEN];
        (void)sc2_json_get_u64(&it, "id", UINT32_MAX - 1u, &oid);
        (void)sc2_json_get_b64_exact(&it, "pub", pub, sizeof(pub));
        unsigned k = 0;
        while (k < dv->opk_n && dv->opk_id[k] != (uint32_t)oid) {
            k++;
        }
        if (k < dv->opk_n) {
            memcpy(dv->opk[k], pub, sizeof(pub)); /* update, like the full relay */
        } else if (dv->opk_n < SC2_MR_OPKS) {
            dv->opk_id[dv->opk_n] = (uint32_t)oid;
            memcpy(dv->opk[dv->opk_n], pub, sizeof(pub));
            dv->opk_n++;
        }
    }
    ob_t o;
    ob_head(&o, mr, "ack", rid);
    ob_s(&o, ",\"opks\":");
    ob_u64(&o, dv->opk_n);
    ob_s(&o, "}");
    ob_send(mr, s, &o);
    return SC2_MR_OK;
}

static int h_keys_claim(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    int d = dev_find_hex(mr, root, "device");
    if (d == -2) {
        return send_err(mr, s, rid, 400, "bad_request", "device must be 32 hex", false);
    }
    if (d < 0 || !mr->dev[d].has_keys) {
        return send_err(mr, s, rid, 404, "unknown_device", "unknown device", false);
    }
    mr_dev_t *dv = &mr->dev[d];
    ob_t o;
    ob_head(&o, mr, "keys.bundle", rid);
    ob_s(&o, ",\"device\":\"");
    ob_s(&o, dv->hex);
    ob_s(&o, "\",\"ik\":");
    ob_b64(&o, dv->ik, SC2_KEY_LEN);
    ob_s(&o, ",\"dk\":");
    ob_b64(&o, dv->dk, SC2_KEY_LEN);
    ob_s(&o, ",\"dk_sig\":");
    ob_b64(&o, dv->dk_sig, SC2_SIG_LEN);
    ob_s(&o, ",\"spk\":{\"id\":");
    ob_u64(&o, dv->spk_id);
    ob_s(&o, ",\"pub\":");
    ob_b64(&o, dv->spk, SC2_KEY_LEN);
    ob_s(&o, ",\"sig\":");
    ob_b64(&o, dv->spk_sig, SC2_SIG_LEN);
    ob_s(&o, "}");
    bool gave = false;
    if (dv->opk_n > 0) {
        unsigned k = 0; /* lowest id first, like the full relay */
        for (unsigned i = 1; i < dv->opk_n; i++) {
            if (dv->opk_id[i] < dv->opk_id[k]) {
                k = i;
            }
        }
        ob_s(&o, ",\"opk\":{\"id\":");
        ob_u64(&o, dv->opk_id[k]);
        ob_s(&o, ",\"pub\":");
        ob_b64(&o, dv->opk[k], SC2_KEY_LEN);
        ob_s(&o, "}");
        if (o.ok) {
            /* handed out once: remove before sending */
            dv->opk_n--;
            dv->opk_id[k] = dv->opk_id[dv->opk_n];
            memcpy(dv->opk[k], dv->opk[dv->opk_n], SC2_KEY_LEN);
            sc2_memzero(dv->opk[dv->opk_n], SC2_KEY_LEN);
            gave = true;
        }
    }
    ob_s(&o, "}");
    ob_send(mr, s, &o);
    if (dv->opk_n < SC2_MR_KEYS_LOW) {
        keys_low(mr, d, -1);
    }
    if (!gave) {
        ob_head(&o, mr, "error", rid);
        ob_s(&o, ",\"code\":410,\"err\":\"prekey_exhausted\",\"text\":\"no one-time prekey left; bundle has the "
                 "signed prekey only\",\"device\":\"");
        ob_s(&o, dv->hex);
        ob_s(&o, "\"}");
        ob_send(mr, s, &o);
    }
    return SC2_MR_OK;
}

/* ---------------- DMs ---------------- */

static int h_dm_send(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    mr_sess_t *ss = &mr->sess[s];
    sc2_json_val_t env_v;
    int to = dev_find_hex(mr, root, "to");
    if (to == -2 || sc2_json_get(root, "env", &env_v) != SC2_OK || env_v.type != SC2_JSON_OBJECT) {
        return send_err(mr, s, rid, 400, "bad_request", "to and env required", false);
    }
    if (env_v.len > SC2_MR_DM_MAX) {
        return send_err(mr, s, rid, 413, "too_large", "message too large for this relay", false);
    }
    sc2_dm_env_t env;
    if (sc2_dm_env_decode(&env_v, &env, mr->scratch, sizeof(mr->scratch)) != SC2_OK) {
        return send_err(mr, s, rid, 400, "bad_request", "bad env", false);
    }
    sc2_memzero(mr->scratch, sizeof(mr->scratch));
    if (memcmp(env.from, mr->dev[ss->dev].id, SC2_DEVICE_ID_LEN) != 0) {
        return send_err(mr, s, rid, 403, "forbidden", "env.from must be this device", false);
    }
    if (to < 0) {
        return send_err(mr, s, rid, 404, "unknown_device", "unknown device", false);
    }
    ob_t o;
    for (unsigned i = 0; i < SC2_MR_INBOX; i++) {
        mr_inbox_t *in = &mr->inbox[i];
        if (in->used && in->dev == (uint8_t)to && in->n == env.hdr.n &&
            memcmp(in->from, env.from, SC2_DEVICE_ID_LEN) == 0 && memcmp(in->rk, env.hdr.dh, SC2_KEY_LEN) == 0) {
            ob_head(&o, mr, "ack", rid); /* resent: same seq */
            ob_s(&o, ",\"seq\":");
            ob_seq(&o, in->seq);
            ob_s(&o, "}");
            ob_send(mr, s, &o);
            return SC2_MR_OK;
        }
    }
    int slot = -1;
    for (unsigned i = 0; i < SC2_MR_INBOX && slot < 0; i++) {
        if (!mr->inbox[i].used) {
            slot = (int)i;
        }
    }
    if (slot < 0 || inbox_count(mr, to) >= SC2_MR_INBOX_PER_DEVICE) {
        ob_head(&o, mr, "error", rid);
        ob_s(&o, ",\"code\":507,\"err\":\"inbox_full\",\"text\":\"recipient inbox is full\",\"to\":\"");
        ob_s(&o, mr->dev[to].hex);
        ob_s(&o, "\"}");
        ob_send(mr, s, &o);
        mr->refused_total++;
        return SC2_MR_OK;
    }
    mr_inbox_t *in = &mr->inbox[slot];
    in->used = 1;
    in->dev = (uint8_t)to;
    in->len = (uint16_t)env_v.len;
    in->n = env.hdr.n;
    memcpy(in->from, env.from, SC2_DEVICE_ID_LEN);
    memcpy(in->rk, env.hdr.dh, SC2_KEY_LEN);
    memcpy(in->env, env_v.ptr, env_v.len);
    in->env[env_v.len] = '\0';
    in->seq = ++mr->dev[to].inbox_seq;
    mr->dm_total++;
    ob_head(&o, mr, "ack", rid);
    ob_s(&o, ",\"seq\":");
    ob_seq(&o, in->seq);
    ob_s(&o, "}");
    ob_send(mr, s, &o);
    dm_frame(mr, &o, in);
    push_dev(mr, to, -1, &o);
    return SC2_MR_OK;
}

static int h_ack(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    uint64_t upto = 0;
    sc2_json_val_t v;
    if (sc2_json_get(root, "inbox", &v) != SC2_OK || !get_seq(root, "inbox", 0, &upto)) {
        return send_err(mr, s, rid, 400, "bad_request", "inbox must be a decimal string", false);
    }
    int d = mr->sess[s].dev;
    for (unsigned i = 0; i < SC2_MR_INBOX; i++) {
        mr_inbox_t *in = &mr->inbox[i];
        if (in->used && in->dev == (uint8_t)d && in->seq <= upto) {
            sc2_memzero(in, sizeof(*in));
        }
    }
    return SC2_MR_OK;
}

/* ---------------- public room ---------------- */

static void room_state(sc2_mr_t *mr, unsigned s, const char *rid)
{
    ob_t o;
    ob_head(&o, mr, "room.state", rid);
    ob_s(&o, ",\"room\":\"");
    ob_s(&o, mr->room);
    ob_s(&o, "\",\"kind\":\"public\",\"title\":\"");
    ob_s(&o, mr->room + 4);
    ob_s(&o, "\",\"members_count\":");
    ob_u64(&o, room_members(mr));
    ob_s(&o, ",\"label\":\"PUBLIC \\u00b7 not encrypted\",\"slow\":false,\"seq\":");
    ob_seq(&o, mr->pub_seq);
    ob_s(&o, ",\"limits\":{\"msg\":");
    ob_u64(&o, SC2_MR_PUB_BODY_MAX);
    ob_s(&o, ",\"per_min\":0}}");
    ob_send(mr, s, &o);
}

static int room_arg(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid, bool *ours)
{
    char room[SC2_ROOM_NAME_MAX + 1];
    *ours = false;
    if (sc2_json_get_string(root, "room", room, sizeof(room), NULL) != SC2_OK) {
        return send_err(mr, s, rid, 400, "bad_request", "room required", false);
    }
    if (strcmp(room, mr->room) == 0) {
        *ours = true;
        return SC2_MR_OK;
    }
    if (strncmp(room, "pub:", 4) == 0) {
        return send_err(mr, s, rid, 404, "unknown_room", "unknown room", false);
    }
    return send_err(mr, s, rid, 501, "not_supported", "private rooms need a full relay", false);
}

static int h_room_join(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid, bool join)
{
    bool ours;
    int rc = room_arg(mr, s, root, rid, &ours);
    if (!ours) {
        return rc;
    }
    mr->sess[s].joined = join;
    if (join) {
        room_state(mr, s, rid);
    } else {
        send_simple(mr, s, "ack", rid);
    }
    return SC2_MR_OK;
}

static int h_room_list(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    sc2_json_val_t v;
    bool pub = sc2_json_get(root, "kind", &v) == SC2_OK && sc2_json_string_eq(&v, "public");
    if (pub) {
        room_state(mr, s, rid);
    }
    ob_t o;
    ob_head(&o, mr, "ack", rid);
    ob_s(&o, pub ? ",\"count\":1}" : ",\"count\":0}");
    ob_send(mr, s, &o);
    return SC2_MR_OK;
}

static int h_room_post(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    mr_sess_t *ss = &mr->sess[s];
    bool ours;
    int rc = room_arg(mr, s, root, rid, &ours);
    if (!ours) {
        return rc;
    }
    if (!ss->joined) {
        return send_err(mr, s, rid, 403, "not_member", "join the room first", false);
    }
    sc2_json_val_t pv;
    if (sc2_json_get(root, "post", &pv) != SC2_OK || pv.type != SC2_JSON_OBJECT) {
        return send_err(mr, s, rid, 400, "bad_request", "post required", false);
    }
    if (pv.len > SC2_MR_POST_MAX) {
        return send_err(mr, s, rid, 413, "too_large", "post too large for this relay", false);
    }
    sc2_post_t post;
    if (sc2_post_decode(&pv, &post, (char *)mr->scratch, sizeof(mr->scratch)) != SC2_OK) {
        return send_err(mr, s, rid, 400, "bad_request", "bad post", false);
    }
    rc = sc2_post_verify(&post, mr->relay_id, mr->room, SC2_MR_PUB_BODY_MAX);
    if (rc == SC2_ERR_BOUNDS) {
        return send_err(mr, s, rid, 413, "too_large", "body over the room limit", false);
    }
    if (rc != SC2_OK) {
        return send_err(mr, s, rid, rc == SC2_ERR_AUTH ? 401 : 400, rc == SC2_ERR_AUTH ? "bad_sig" : "bad_request",
                        rc == SC2_ERR_AUTH ? "post signature invalid" : "bad post", false);
    }
    if (memcmp(post.from, mr->dev[ss->dev].id, SC2_DEVICE_ID_LEN) != 0) {
        return send_err(mr, s, rid, 403, "forbidden", "post must be signed by this device", false);
    }
    /* Post IDs are chosen by the client and unique per author only, so a
     * post is (author, id) in the one room: a resend gets the same seq, and
     * a delete can only name one of the sender's own posts. */
    ob_t o;
    bool ref_found = false;
    for (unsigned i = 0; i < SC2_MR_PUB_HISTORY; i++) {
        const mr_post_t *p = &mr->posts[i];
        if (p->seq == 0 || memcmp(p->from, post.from, SC2_DEVICE_ID_LEN) != 0) {
            continue;
        }
        if (memcmp(p->id, post.id, SC2_MSG_ID_LEN) == 0) {
            ob_head(&o, mr, "ack", rid);
            ob_s(&o, ",\"seq\":");
            ob_seq(&o, p->seq);
            ob_s(&o, "}");
            ob_send(mr, s, &o);
            return SC2_MR_OK;
        }
        if (post.has_ref && memcmp(p->id, post.ref, SC2_MSG_ID_LEN) == 0) {
            ref_found = true;
        }
    }
    if (post.t == SC2_POST_T_DEL && !ref_found) {
        return send_err(mr, s, rid, 400, "bad_request", "unknown ref", false);
    }
    mr_post_t *p = &mr->posts[mr->pub_head];
    mr->pub_head = (mr->pub_head + 1u) % SC2_MR_PUB_HISTORY; /* history cap: the oldest goes */
    p->seq = ++mr->pub_seq;
    memcpy(p->from, post.from, SC2_DEVICE_ID_LEN);
    memcpy(p->id, post.id, SC2_MSG_ID_LEN);
    p->len = (uint16_t)pv.len;
    memcpy(p->post, pv.ptr, pv.len);
    p->post[pv.len] = '\0';
    mr->posts_total++;
    ob_head(&o, mr, "ack", rid);
    ob_s(&o, ",\"seq\":");
    ob_seq(&o, p->seq);
    ob_s(&o, "}");
    ob_send(mr, s, &o);
    post_frame(mr, &o, p);
    for (unsigned i = 0; o.ok && i < mr->nsess; i++) {
        mr_sess_t *x = &mr->sess[i];
        if (i != s && x->state == ST_AUTHED && x->live && x->joined) {
            (void)mr->send(mr->ctx, i, o.b, o.n);
        }
    }
    return SC2_MR_OK;
}

/* ---------------- sync ---------------- */

static int h_sync(sc2_mr_t *mr, unsigned s, const sc2_json_val_t *root, const char *rid)
{
    mr_sess_t *ss = &mr->sess[s];
    uint64_t cursor = 0, budget = 100, room_cur = 0;
    sc2_json_val_t rooms, rv;
    if (!get_seq(root, "inbox", 0, &cursor)) {
        return send_err(mr, s, rid, 400, "bad_request", "inbox must be a decimal string", false);
    }
    if (sc2_json_get(root, "max", &rv) == SC2_OK && (sc2_json_u64(&rv, MR_SYNC_MAX, &budget) != SC2_OK || budget == 0)) {
        return send_err(mr, s, rid, 400, "bad_request", "max must be 1..500", false);
    }
    bool want_room = sc2_json_get(root, "rooms", &rooms) == SC2_OK && rooms.type == SC2_JSON_OBJECT &&
                     sc2_json_get(&rooms, mr->room, &rv) == SC2_OK;
    if (want_room && (rv.type != SC2_JSON_STRING || sc2_json_u64_str(&rv, &room_cur) != SC2_OK)) {
        return send_err(mr, s, rid, 400, "bad_request", "room cursor must be a decimal string", false);
    }
    bool more = false;
    ob_t o;
    const mr_inbox_t *in;
    while ((in = inbox_after(mr, ss->dev, cursor)) != NULL) {
        if (budget == 0) {
            more = true;
            break;
        }
        dm_frame(mr, &o, in);
        ob_send(mr, s, &o);
        cursor = in->seq;
        budget--;
    }
    uint64_t last = room_cur;
    if (want_room) {
        uint64_t lo = pub_oldest(mr), hi = mr->pub_seq;
        uint64_t after = room_cur;
        if (lo != 0 && after < hi && after + 1u < lo) {
            ob_head(&o, mr, "sync.gap", NULL);
            ob_s(&o, ",\"room\":\"");
            ob_s(&o, mr->room);
            ob_s(&o, "\",\"from\":");
            ob_seq(&o, after);
            ob_s(&o, ",\"oldest\":");
            ob_seq(&o, lo);
            ob_s(&o, "}");
            ob_send(mr, s, &o);
            after = lo - 1u;
        }
        last = after;
        const mr_post_t *p;
        while ((p = pub_after(mr, last)) != NULL) {
            if (budget == 0) {
                more = true;
                break;
            }
            post_frame(mr, &o, p);
            ob_send(mr, s, &o);
            last = p->seq;
            budget--;
        }
    }
    ob_head(&o, mr, "sync.done", rid);
    ob_s(&o, ",\"inbox\":");
    ob_seq(&o, cursor);
    ob_s(&o, ",\"rooms\":{");
    if (want_room) {
        ob_s(&o, "\"");
        ob_s(&o, mr->room);
        ob_s(&o, "\":");
        ob_seq(&o, last);
    }
    ob_s(&o, more ? "},\"more\":true}" : "},\"more\":false}");
    ob_send(mr, s, &o);
    if (!more) {
        ss->live = 1;
    }
    return SC2_MR_OK;
}

/* ---------------- dispatch ---------------- */

static bool rid_safe(const char *rid)
{
    for (const char *p = rid; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
            return false;
        }
    }
    return true;
}

int sc2_mr_line(sc2_mr_t *mr, unsigned s, const char *line, size_t len)
{
    if (mr == NULL || s >= mr->nsess || mr->sess[s].state == ST_FREE || mr->sess[s].state == ST_V1) {
        return SC2_MR_CLOSE;
    }
    mr_sess_t *ss = &mr->sess[s];
    sc2_json_val_t root;
    char type[SC2_TYPE_MAX + 1], rid[SC2_RID_MAX + 1];
    if (len > MR_LINE_LIMIT) {
        return send_err(mr, s, NULL, 413, "too_large", "line too long", true);
    }
    if (sc2_frame_peek(line, len, &root, type, rid) != SC2_OK || !rid_safe(rid)) {
        return send_err(mr, s, NULL, 400, "bad_request", "unparsable frame", ss->state == ST_NEW);
    }
    if (strcmp(type, "ping") != 0 && strcmp(type, "pong") != 0) {
        ss->active = slot_clock(mr); /* pings keep NAT state warm but are not activity */
    }
    if (ss->state == ST_NEW) {
        if (strcmp(type, "hello") != 0) {
            return send_err(mr, s, rid, 401, "auth_required", "hello required", true);
        }
        return h_hello(mr, s, &root, rid);
    }
    if (strcmp(type, "ping") == 0) {
        send_simple(mr, s, "pong", rid);
        return SC2_MR_OK;
    }
    if (strcmp(type, "hello") == 0) {
        return send_err(mr, s, rid, 400, "bad_request", "hello already received", false);
    }
    if (strcmp(type, "auth") == 0) {
        return h_auth(mr, s, &root, rid);
    }
    if (ss->state != ST_AUTHED) {
        return send_err(mr, s, rid, 401, "auth_required", "authenticate first", false);
    }
    if (strcmp(type, "keys.publish") == 0) {
        return h_keys_publish(mr, s, &root, rid);
    }
    if (strcmp(type, "keys.claim") == 0) {
        return h_keys_claim(mr, s, &root, rid);
    }
    if (strcmp(type, "dm.send") == 0) {
        return h_dm_send(mr, s, &root, rid);
    }
    if (strcmp(type, "sync") == 0) {
        return h_sync(mr, s, &root, rid);
    }
    if (strcmp(type, "ack") == 0) {
        return h_ack(mr, s, &root, rid);
    }
    if (strcmp(type, "room.join") == 0 || strcmp(type, "room.leave") == 0) {
        return h_room_join(mr, s, &root, rid, type[5] == 'j');
    }
    if (strcmp(type, "room.post") == 0) {
        return h_room_post(mr, s, &root, rid);
    }
    if (strcmp(type, "room.list") == 0) {
        return h_room_list(mr, s, &root, rid);
    }
    if (strcmp(type, "presence.set") == 0) {
        if (rid[0] != '\0') {
            send_simple(mr, s, "ack", rid);
        }
        return SC2_MR_OK;
    }
    if (strncmp(type, "room.", 5) == 0) {
        return send_err(mr, s, rid, 501, "not_supported", "not supported by this relay", false);
    }
    return send_err(mr, s, rid, 400, "bad_request", "unknown type", false);
}

/* ---------------- lifecycle ---------------- */

static bool relay_id_ok(const char *id)
{
    size_t n = id != NULL ? strlen(id) : 0;
    if (n == 0 || n > MR_RELAY_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-' || c == '_' || c == ':')) {
            return false;
        }
    }
    return true;
}

static bool room_ok(const char *room)
{
    size_t n = strlen(room);
    if (n < 5 || n > SC2_ROOM_NAME_MAX || strncmp(room, "pub:", 4) != 0) {
        return false;
    }
    for (size_t i = 4; i < n; i++) {
        char c = room[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

int sc2_mr_init(sc2_mr_t **out, const sc2_mr_config_t *cfg)
{
    if (out == NULL || cfg == NULL || cfg->seed == NULL || cfg->send == NULL || cfg->alloc == NULL ||
        cfg->release == NULL || cfg->rng.fn == NULL || !relay_id_ok(cfg->relay_id) || cfg->sessions == 0 ||
        cfg->sessions > SC2_MR_SESSIONS_MAX || (cfg->admit != NULL && strlen(cfg->admit) > SC2_MR_ADMIT_MAX)) {
        return SC2_ERR_ARG;
    }
    const char *room = cfg->room != NULL ? cfg->room : SC2_MR_ROOM_DEFAULT;
    if (!room_ok(room)) {
        return SC2_ERR_ARG;
    }
    sc2_mr_t *mr = cfg->alloc(cfg->ctx, sizeof(*mr));
    if (mr == NULL) {
        return SC2_ERR_BOUNDS;
    }
    memset(mr, 0, sizeof(*mr));
    copy_str(mr->relay_id, sizeof(mr->relay_id), cfg->relay_id);
    copy_str(mr->room, sizeof(mr->room), room);
    if (cfg->admit != NULL && cfg->admit[0] != '\0') {
        copy_str(mr->admit, sizeof(mr->admit), cfg->admit);
        mr->has_admit = true;
    }
    memcpy(mr->seed, cfg->seed, SC2_KEY_LEN);
    int rc = sc2_ed25519_public(mr->pub, mr->seed);
    if (rc != SC2_OK) {
        sc2_memzero(mr, sizeof(*mr));
        cfg->release(cfg->ctx, mr, sizeof(*mr));
        return rc;
    }
    mr->rng = cfg->rng;
    mr->now_s = cfg->now_s;
    mr->mono_s = cfg->mono_s;
    mr->send = cfg->send;
    mr->kick = cfg->kick;
    mr->release = cfg->release;
    mr->ctx = cfg->ctx;
    mr->legacy_v1 = cfg->legacy_v1;
    mr->nsess = cfg->sessions;
    mr->idle_s = cfg->idle_s != 0 ? cfg->idle_s : SC2_MR_IDLE_S;
    *out = mr;
    return SC2_OK;
}

void sc2_mr_free(sc2_mr_t *mr)
{
    if (mr == NULL) {
        return;
    }
    void (*release)(void *, void *, size_t) = mr->release;
    void *ctx = mr->ctx;
    sc2_memzero(mr, sizeof(*mr));
    release(ctx, mr, sizeof(*mr));
}

void sc2_mr_relay_key(const sc2_mr_t *mr, uint8_t out[SC2_KEY_LEN])
{
    memcpy(out, mr->pub, SC2_KEY_LEN);
}

void sc2_mr_open(sc2_mr_t *mr, unsigned s)
{
    if (mr != NULL && s < mr->nsess) {
        memset(&mr->sess[s], 0, sizeof(mr->sess[s]));
        mr->sess[s].state = ST_NEW;
        mr->sess[s].dev = -1;
        mr->sess[s].opened = mr->sess[s].active = slot_clock(mr);
    }
}

unsigned sc2_mr_tick(sc2_mr_t *mr)
{
    if (mr == NULL || !has_clock(mr)) {
        return 0;
    }
    uint64_t now = slot_clock(mr);
    unsigned n = 0;
    for (unsigned i = 0; i < mr->nsess; i++) {
        const mr_sess_t *ss = &mr->sess[i];
        if ((ss->state == ST_NEW || ss->state == ST_WELCOMED) && since(now, ss->opened) >= SC2_MR_AUTH_S) {
            kick(mr, i, 408, "idle", "login not completed in time");
        } else if (ss->state == ST_AUTHED && since(now, ss->active) >= mr->idle_s) {
            kick(mr, i, 408, "idle", "closed after a period without activity; reconnect to continue");
        } else {
            continue;
        }
        mr->idle_total++;
        n++;
    }
    return n;
}

int sc2_mr_evict(sc2_mr_t *mr)
{
    if (mr == NULL || !has_clock(mr)) {
        return -1;
    }
    uint64_t now = slot_clock(mr);
    int best = -1;
    for (unsigned i = 0; i < mr->nsess; i++) {
        const mr_sess_t *ss = &mr->sess[i];
        if (ss->state == ST_AUTHED && since(now, ss->active) >= SC2_MR_EVICT_MIN_S &&
            (best < 0 || ss->active < mr->sess[best].active)) {
            best = (int)i;
        }
    }
    if (best >= 0) {
        kick(mr, (unsigned)best, 503, "evicted", "relay full; this idle session was closed to admit another");
        mr->evicted_total++;
    }
    return best;
}

void sc2_mr_close(sc2_mr_t *mr, unsigned s)
{
    if (mr != NULL && s < mr->nsess) {
        sc2_memzero(&mr->sess[s], sizeof(mr->sess[s]));
        mr->sess[s].dev = -1;
    }
}

bool sc2_mr_active(const sc2_mr_t *mr, unsigned s)
{
    return mr != NULL && s < mr->nsess && mr->sess[s].state >= ST_NEW && mr->sess[s].state <= ST_AUTHED;
}

void sc2_mr_stats(const sc2_mr_t *mr, sc2_mr_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (mr == NULL) {
        return;
    }
    for (unsigned i = 0; i < SC2_MR_DEVICES; i++) {
        out->devices += mr->dev[i].used;
    }
    for (unsigned i = 0; i < SC2_MR_INBOX; i++) {
        out->inbox += mr->inbox[i].used;
    }
    for (unsigned i = 0; i < SC2_MR_PUB_HISTORY; i++) {
        out->posts += mr->posts[i].seq != 0;
    }
    for (unsigned i = 0; i < mr->nsess; i++) {
        out->sessions += mr->sess[i].state == ST_AUTHED;
    }
    out->dm_total = mr->dm_total;
    out->posts_total = mr->posts_total;
    out->refused_total = mr->refused_total;
    out->replaced_total = mr->replaced_total;
    out->idle_total = mr->idle_total;
    out->evicted_total = mr->evicted_total;
}
