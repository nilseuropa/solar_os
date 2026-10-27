/*
 * SC2 portable core: private-room membership commits, rosters and invites.
 * See sc2_commit.h for the byte layouts.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_commit.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"
#include "sc2_hash.h"
#include "sc2_identity.h"

static const uint8_t COMMIT_LABEL[11] = {'S', 'C', '2', '-', 'C', 'O', 'M', 'M', 'I', 'T', 0x01};
static const uint8_t INVITE_LABEL[11] = {'S', 'C', '2', '-', 'I', 'N', 'V', 'I', 'T', 'E', 0x01};
static const uint8_t JOIN_LABEL[9] = {'S', 'C', '2', '-', 'J', 'O', 'I', 'N', 0x01};
static const uint8_t INVITE_MAGIC[4] = {'S', 'C', '2', 'I'};

/* Record offsets. */
enum {
    OFF_SUITE = 11,
    OFF_ROOM = 12,
    OFF_EPOCH = 28,
    OFF_PREV = 32,
    OFF_BY = 64,
    OFF_OP = 80,
    OFF_TARGET = 81,
    OFF_COUNT = 97,
    OFF_HASH = 98
};

static int is_zero(const uint8_t *p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

static int shape_ok(const sc2_commit_t *c)
{
    if (c->op < SC2_COMMIT_CREATE || c->op > SC2_COMMIT_ROTATE) {
        return 0;
    }
    if (c->count == 0 || c->count > SC2_ROOM_MEMBERS_MAX || c->epoch == 0) {
        return 0;
    }
    if (is_zero(c->by, SC2_DEVICE_ID_LEN)) {
        return 0;
    }
    if (c->op == SC2_COMMIT_CREATE) {
        return c->epoch == 1 && c->count == 1 && is_zero(c->prev, sizeof(c->prev)) &&
               memcmp(c->target, c->by, SC2_DEVICE_ID_LEN) == 0;
    }
    if (c->epoch == 1) {
        return 0; /* only a genesis commit may start the chain */
    }
    if (c->op == SC2_COMMIT_ROTATE) {
        return is_zero(c->target, SC2_DEVICE_ID_LEN);
    }
    return !is_zero(c->target, SC2_DEVICE_ID_LEN);
}

int sc2_commit_encode(uint8_t out[SC2_COMMIT_LEN], const sc2_commit_t *c)
{
    if (out == NULL || c == NULL || !shape_ok(c)) {
        return SC2_ERR_ARG;
    }
    memcpy(out, COMMIT_LABEL, sizeof(COMMIT_LABEL));
    out[OFF_SUITE] = (uint8_t)SC2_SUITE_1;
    memcpy(out + OFF_ROOM, c->room, SC2_ROOM_ID_LEN);
    sc2_store_be32(out + OFF_EPOCH, c->epoch);
    memcpy(out + OFF_PREV, c->prev, SC2_COMMIT_ID_LEN);
    memcpy(out + OFF_BY, c->by, SC2_DEVICE_ID_LEN);
    out[OFF_OP] = c->op;
    memcpy(out + OFF_TARGET, c->target, SC2_DEVICE_ID_LEN);
    out[OFF_COUNT] = c->count;
    memcpy(out + OFF_HASH, c->roster_hash, 32);
    return SC2_OK;
}

int sc2_commit_decode(sc2_commit_t *c, const uint8_t rec[SC2_COMMIT_LEN])
{
    if (c == NULL || rec == NULL) {
        return SC2_ERR_ARG;
    }
    if (memcmp(rec, COMMIT_LABEL, sizeof(COMMIT_LABEL)) != 0 || rec[OFF_SUITE] != SC2_SUITE_1) {
        return SC2_ERR_FORMAT;
    }
    memcpy(c->room, rec + OFF_ROOM, SC2_ROOM_ID_LEN);
    c->epoch = sc2_load_be32(rec + OFF_EPOCH);
    memcpy(c->prev, rec + OFF_PREV, SC2_COMMIT_ID_LEN);
    memcpy(c->by, rec + OFF_BY, SC2_DEVICE_ID_LEN);
    c->op = rec[OFF_OP];
    memcpy(c->target, rec + OFF_TARGET, SC2_DEVICE_ID_LEN);
    c->count = rec[OFF_COUNT];
    memcpy(c->roster_hash, rec + OFF_HASH, 32);
    return shape_ok(c) ? SC2_OK : SC2_ERR_FORMAT;
}

int sc2_commit_id(uint8_t out[SC2_COMMIT_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN])
{
    if (out == NULL || rec == NULL) {
        return SC2_ERR_ARG;
    }
    return sc2_sha256(out, rec, SC2_COMMIT_LEN);
}

int sc2_commit_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t rec[SC2_COMMIT_LEN], const uint8_t ik_seed[SC2_KEY_LEN])
{
    if (sig == NULL || rec == NULL || ik_seed == NULL) {
        return SC2_ERR_ARG;
    }
    return sc2_ed25519_sign(sig, rec, SC2_COMMIT_LEN, ik_seed);
}

int sc2_commit_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                      const uint8_t ik_pub[SC2_KEY_LEN])
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    if (sig == NULL || rec == NULL || ik_pub == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_device_id(id, ik_pub);
    if (rc != SC2_OK) {
        return rc;
    }
    if (!sc2_ct_eq(id, rec + OFF_BY, SC2_DEVICE_ID_LEN)) {
        return SC2_ERR_AUTH;
    }
    return sc2_ed25519_verify(sig, rec, SC2_COMMIT_LEN, ik_pub);
}

/* ---------------------------------------------------------------- roster */

void sc2_roster_init(sc2_roster_t *r, const uint8_t room[SC2_ROOM_ID_LEN])
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    if (room != NULL) {
        memcpy(r->room, room, SC2_ROOM_ID_LEN);
    }
}

int sc2_roster_find(const sc2_roster_t *r, const uint8_t id[SC2_DEVICE_ID_LEN])
{
    if (r == NULL || id == NULL) {
        return -1;
    }
    size_t lo = 0;
    size_t hi = r->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2u;
        int d = memcmp(r->m[mid].id, id, SC2_DEVICE_ID_LEN);
        if (d == 0) {
            return (int)mid;
        }
        if (d < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return -1;
}

int sc2_roster_is_admin(const sc2_roster_t *r, const uint8_t id[SC2_DEVICE_ID_LEN])
{
    int i = sc2_roster_find(r, id);
    return i >= 0 && r->m[i].role == SC2_ROLE_ADMIN;
}

static unsigned admin_count(const sc2_roster_t *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < r->count; i++) {
        n += r->m[i].role == SC2_ROLE_ADMIN;
    }
    return n;
}

static int hash_entry(sc2_sha256_ctx *h, const uint8_t id[SC2_DEVICE_ID_LEN], uint8_t role)
{
    int rc = sc2_sha256_update(h, id, SC2_DEVICE_ID_LEN);
    return rc == SC2_OK ? sc2_sha256_update(h, &role, 1) : rc;
}

/* Stream the roster hash as it would be after (op, target), without
 * modifying r. *count receives the resulting member count. The op must
 * already be valid for r. */
static int hash_after(const sc2_roster_t *r, uint8_t op, const uint8_t target[SC2_DEVICE_ID_LEN], uint8_t out[32],
                      unsigned *count)
{
    sc2_sha256_ctx h;
    int inserted = op != SC2_COMMIT_ADD && op != SC2_COMMIT_CREATE;
    unsigned n = 0;
    int rc = sc2_sha256_init(&h);
    for (unsigned i = 0; rc == SC2_OK && i < r->count; i++) {
        const sc2_member_t *e = &r->m[i];
        int d = memcmp(e->id, target, SC2_DEVICE_ID_LEN);
        if (!inserted && d > 0) {
            rc = hash_entry(&h, target, op == SC2_COMMIT_CREATE ? SC2_ROLE_ADMIN : SC2_ROLE_MEMBER);
            inserted = 1;
            n++;
            if (rc != SC2_OK) {
                break;
            }
        }
        if (d == 0 && op == SC2_COMMIT_REMOVE) {
            continue;
        }
        uint8_t role = e->role;
        if (d == 0 && op == SC2_COMMIT_PROMOTE) {
            role = SC2_ROLE_ADMIN;
        } else if (d == 0 && op == SC2_COMMIT_DEMOTE) {
            role = SC2_ROLE_MEMBER;
        }
        rc = hash_entry(&h, e->id, role);
        n++;
    }
    if (rc == SC2_OK && !inserted) {
        rc = hash_entry(&h, target, op == SC2_COMMIT_CREATE ? SC2_ROLE_ADMIN : SC2_ROLE_MEMBER);
        n++;
    }
    if (rc != SC2_OK) {
        uint8_t scrap[32];
        (void)sc2_sha256_final(&h, scrap);
        return rc;
    }
    *count = n;
    return sc2_sha256_final(&h, out);
}

/* Rule checks shared by make and check (everything except count/hash). */
static int op_allowed(const sc2_roster_t *r, uint8_t op, const uint8_t by[SC2_DEVICE_ID_LEN],
                      const uint8_t target[SC2_DEVICE_ID_LEN])
{
    if (op == SC2_COMMIT_CREATE) {
        return r->epoch == 0 && r->count == 0 && memcmp(by, target, SC2_DEVICE_ID_LEN) == 0 ? SC2_OK
                                                                                           : SC2_ERR_STATE;
    }
    if (r->epoch == 0 || r->epoch == UINT32_MAX || !sc2_roster_is_admin(r, by)) {
        return SC2_ERR_STATE;
    }
    int idx = op == SC2_COMMIT_ROTATE ? -1 : sc2_roster_find(r, target);
    switch (op) {
    case SC2_COMMIT_ADD:
        if (idx >= 0) {
            return SC2_ERR_STATE;
        }
        if (r->count >= SC2_ROOM_MEMBERS_MAX) {
            return SC2_ERR_STATE;
        }
        return r->count >= SC2_ROSTER_CAP ? SC2_ERR_BOUNDS : SC2_OK;
    case SC2_COMMIT_REMOVE:
        if (idx < 0 || (r->m[idx].role == SC2_ROLE_ADMIN && admin_count(r) <= 1u)) {
            return SC2_ERR_STATE;
        }
        return SC2_OK;
    case SC2_COMMIT_PROMOTE:
        return idx >= 0 && r->m[idx].role != SC2_ROLE_ADMIN ? SC2_OK : SC2_ERR_STATE;
    case SC2_COMMIT_DEMOTE:
        return idx >= 0 && r->m[idx].role == SC2_ROLE_ADMIN && admin_count(r) > 1u ? SC2_OK : SC2_ERR_STATE;
    case SC2_COMMIT_ROTATE:
        return SC2_OK;
    default:
        return SC2_ERR_ARG;
    }
}

int sc2_roster_hash(const sc2_roster_t *r, uint8_t out[32])
{
    static const uint8_t zero[SC2_DEVICE_ID_LEN];
    unsigned n;
    if (r == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    return hash_after(r, SC2_COMMIT_ROTATE, zero, out, &n);
}

int sc2_commit_make(sc2_commit_t *c, const sc2_roster_t *r, const uint8_t by[SC2_DEVICE_ID_LEN], uint8_t op,
                    const uint8_t target[SC2_DEVICE_ID_LEN])
{
    static const uint8_t zero[SC2_DEVICE_ID_LEN];
    unsigned n = 0;
    if (c == NULL || r == NULL || by == NULL) {
        return SC2_ERR_ARG;
    }
    if (op == SC2_COMMIT_CREATE) {
        target = by;
    } else if (op == SC2_COMMIT_ROTATE) {
        target = zero;
    } else if (target == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = op_allowed(r, op, by, target);
    if (rc != SC2_OK) {
        return rc;
    }
    memset(c, 0, sizeof(*c));
    memcpy(c->room, r->room, SC2_ROOM_ID_LEN);
    c->epoch = r->epoch + 1u;
    memcpy(c->prev, r->head, SC2_COMMIT_ID_LEN);
    memcpy(c->by, by, SC2_DEVICE_ID_LEN);
    c->op = op;
    memcpy(c->target, target, SC2_DEVICE_ID_LEN);
    rc = hash_after(r, op, target, c->roster_hash, &n);
    c->count = (uint8_t)n;
    return rc;
}

int sc2_roster_check(const sc2_roster_t *r, const sc2_commit_t *c)
{
    uint8_t hash[32];
    unsigned n = 0;
    if (r == NULL || c == NULL || !shape_ok(c)) {
        return SC2_ERR_ARG;
    }
    if (memcmp(c->room, r->room, SC2_ROOM_ID_LEN) != 0) {
        return SC2_ERR_STATE;
    }
    if (c->epoch != r->epoch + 1u || memcmp(c->prev, r->head, SC2_COMMIT_ID_LEN) != 0) {
        return SC2_ERR_STATE;
    }
    int rc = op_allowed(r, c->op, c->by, c->target);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = hash_after(r, c->op, c->target, hash, &n);
    if (rc != SC2_OK) {
        return rc;
    }
    if (n != c->count || !sc2_ct_eq(hash, c->roster_hash, sizeof(hash))) {
        return SC2_ERR_STATE;
    }
    return SC2_OK;
}

int sc2_roster_apply(sc2_roster_t *r, const uint8_t rec[SC2_COMMIT_LEN])
{
    sc2_commit_t c;
    uint8_t head[SC2_COMMIT_ID_LEN];
    if (r == NULL || rec == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_commit_decode(&c, rec);
    if (rc == SC2_OK) {
        rc = sc2_roster_check(r, &c);
    }
    if (rc == SC2_OK) {
        rc = sc2_commit_id(head, rec);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    if (c.op == SC2_COMMIT_ADD || c.op == SC2_COMMIT_CREATE) {
        unsigned pos = 0;
        while (pos < r->count && memcmp(r->m[pos].id, c.target, SC2_DEVICE_ID_LEN) < 0) {
            pos++;
        }
        memmove(&r->m[pos + 1u], &r->m[pos], (r->count - pos) * sizeof(r->m[0]));
        memcpy(r->m[pos].id, c.target, SC2_DEVICE_ID_LEN);
        r->m[pos].role = c.op == SC2_COMMIT_CREATE ? SC2_ROLE_ADMIN : SC2_ROLE_MEMBER;
        r->count++;
    } else if (c.op != SC2_COMMIT_ROTATE) {
        int idx = sc2_roster_find(r, c.target);
        if (c.op == SC2_COMMIT_REMOVE) {
            memmove(&r->m[idx], &r->m[idx + 1], (r->count - (unsigned)idx - 1u) * sizeof(r->m[0]));
            r->count--;
            memset(&r->m[r->count], 0, sizeof(r->m[0]));
        } else {
            r->m[idx].role = c.op == SC2_COMMIT_PROMOTE ? SC2_ROLE_ADMIN : SC2_ROLE_MEMBER;
        }
    }
    r->epoch = c.epoch;
    memcpy(r->head, head, sizeof(head));
    return SC2_OK;
}

/* ---------------------------------------------------------------- invites */

int sc2_invite_key(uint8_t out[SC2_INVITE_KEY_LEN], const uint8_t secret[SC2_INVITE_SECRET_LEN],
                   const uint8_t room[SC2_ROOM_ID_LEN])
{
    sc2_hmac_ctx h;
    if (out == NULL || secret == NULL || room == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_hmac_init(&h, secret, SC2_INVITE_SECRET_LEN);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_hmac_update(&h, INVITE_LABEL, sizeof(INVITE_LABEL));
    if (rc == SC2_OK) {
        rc = sc2_hmac_update(&h, room, SC2_ROOM_ID_LEN);
    }
    uint8_t mac[32];
    int rc2 = sc2_hmac_final(&h, mac);
    if (rc == SC2_OK) {
        rc = rc2;
    }
    if (rc == SC2_OK) {
        memcpy(out, mac, sizeof(mac));
    }
    sc2_memzero(mac, sizeof(mac));
    return rc;
}

int sc2_invite_proof(uint8_t out[32], const uint8_t invite_key[SC2_INVITE_KEY_LEN],
                     const uint8_t joiner[SC2_DEVICE_ID_LEN])
{
    sc2_hmac_ctx h;
    if (out == NULL || invite_key == NULL || joiner == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_hmac_init(&h, invite_key, SC2_INVITE_KEY_LEN);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_hmac_update(&h, JOIN_LABEL, sizeof(JOIN_LABEL));
    if (rc == SC2_OK) {
        rc = sc2_hmac_update(&h, joiner, SC2_DEVICE_ID_LEN);
    }
    int rc2 = sc2_hmac_final(&h, out);
    return rc == SC2_OK ? rc2 : rc;
}

static int url_ok(const char *url, size_t len)
{
    if (len == 0 || len > SC2_INVITE_URL_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)url[i] < 0x21u || (unsigned char)url[i] > 0x7eu) {
            return 0;
        }
    }
    return (len > 7 && memcmp(url, "chat://", 7) == 0) || (len > 8 && memcmp(url, "chats://", 8) == 0);
}

int sc2_invite_encode(uint8_t *out, size_t cap, size_t *out_len, const sc2_invite_t *inv)
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    if (out == NULL || out_len == NULL || inv == NULL) {
        return SC2_ERR_ARG;
    }
    if (!url_ok(inv->url, inv->url_len)) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_device_id(id, inv->admin_ik);
    if (rc != SC2_OK) {
        return rc;
    }
    if (memcmp(id, inv->admin_id, SC2_DEVICE_ID_LEN) != 0) {
        return SC2_ERR_ARG;
    }
    size_t need = SC2_INVITE_FIXED_LEN + inv->url_len;
    if (cap < need) {
        return SC2_ERR_BOUNDS;
    }
    memcpy(out, INVITE_MAGIC, 4);
    out[4] = 1;
    memcpy(out + 5, inv->room, SC2_ROOM_ID_LEN);
    memcpy(out + 21, inv->secret, SC2_INVITE_SECRET_LEN);
    memcpy(out + 37, inv->admin_id, SC2_DEVICE_ID_LEN);
    memcpy(out + 53, inv->admin_ik, SC2_KEY_LEN);
    out[85] = (uint8_t)inv->url_len;
    memcpy(out + 86, inv->url, inv->url_len);
    *out_len = need;
    return SC2_OK;
}

int sc2_invite_decode(sc2_invite_t *inv, const uint8_t *in, size_t len)
{
    uint8_t id[SC2_DEVICE_ID_LEN];
    if (inv == NULL || in == NULL) {
        return SC2_ERR_ARG;
    }
    if (len < SC2_INVITE_FIXED_LEN + 1u || len > SC2_INVITE_CODE_MAX || memcmp(in, INVITE_MAGIC, 4) != 0 ||
        in[4] != 1 || (size_t)in[85] + SC2_INVITE_FIXED_LEN != len) {
        return SC2_ERR_FORMAT;
    }
    if (!url_ok((const char *)in + 86, in[85])) {
        return SC2_ERR_FORMAT;
    }
    int rc = sc2_device_id(id, in + 53);
    if (rc != SC2_OK) {
        return rc;
    }
    if (memcmp(id, in + 37, SC2_DEVICE_ID_LEN) != 0) {
        return SC2_ERR_FORMAT;
    }
    memset(inv, 0, sizeof(*inv));
    memcpy(inv->room, in + 5, SC2_ROOM_ID_LEN);
    memcpy(inv->secret, in + 21, SC2_INVITE_SECRET_LEN);
    memcpy(inv->admin_id, in + 37, SC2_DEVICE_ID_LEN);
    memcpy(inv->admin_ik, in + 53, SC2_KEY_LEN);
    inv->url_len = in[85];
    memcpy(inv->url, in + 86, inv->url_len);
    inv->url[inv->url_len] = '\0';
    return SC2_OK;
}

int sc2_invite_to_text(char *out, size_t cap, size_t *out_len, const sc2_invite_t *inv)
{
    uint8_t rec[SC2_INVITE_CODE_MAX];
    size_t n = 0;
    size_t b = 0;
    const size_t plen = sizeof(SC2_INVITE_TEXT_PREFIX) - 1u;
    if (out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_invite_encode(rec, sizeof(rec), &n, inv);
    if (rc == SC2_OK) {
        if (cap < plen + 1u) {
            rc = SC2_ERR_BOUNDS;
        } else {
            memcpy(out, SC2_INVITE_TEXT_PREFIX, plen);
            rc = sc2_b64_encode(out + plen, cap - plen, &b, rec, n);
        }
    }
    sc2_memzero(rec, sizeof(rec));
    if (rc == SC2_OK) {
        *out_len = plen + b;
    }
    return rc;
}

int sc2_invite_from_text(sc2_invite_t *inv, const char *text, size_t len)
{
    uint8_t rec[SC2_INVITE_CODE_MAX];
    size_t n = 0;
    const size_t plen = sizeof(SC2_INVITE_TEXT_PREFIX) - 1u;
    if (inv == NULL || text == NULL) {
        return SC2_ERR_ARG;
    }
    if (len <= plen || memcmp(text, SC2_INVITE_TEXT_PREFIX, plen) != 0) {
        return SC2_ERR_FORMAT;
    }
    int rc = sc2_b64_decode(rec, sizeof(rec), &n, text + plen, len - plen);
    if (rc == SC2_OK) {
        rc = sc2_invite_decode(inv, rec, n);
    } else if (rc == SC2_ERR_BOUNDS) {
        rc = SC2_ERR_FORMAT;
    }
    sc2_memzero(rec, sizeof(rec));
    return rc;
}
