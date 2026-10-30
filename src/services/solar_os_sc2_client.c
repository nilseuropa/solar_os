/*
 * SC2 relay client state machine (see solar_os_sc2_client.h).
 * SPDX-License-Identifier: MIT
 */
#include "solar_os_sc2_client.h"

#include <stdio.h>
#include <string.h>

#include "sc2_codec.h"
#include "sc2_identity.h"
#include "sc2_json.h"

#define OB_MAGIC "SC2O"
#define OB_VERSION 1u
#define OB_RECORD_HDR (4u + 1u + 1u + 4u + 8u + (SC2C_KEY_MAX + 1u) + 4u + 4u)
#define OB_HDR (12u + OB_RECORD_HDR) /* SC2E envelope header, then outbox header */
#define REC_OUTBOX_TYPE 0x32u
#define OB_KIND_SHARE 9u /* epoch key DM owed to a room member (no event) */
#define REC_ROOM_TYPE 0x31u
#define REC_ROOM_VERSION 2u
#define RETRY_MS 5000u
#define CLAIM_RETRY_MS 30000u
#define SYNC_MAX 100u

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static uint64_t now_ms(sc2c_t *c)
{
    return c->host.mono_ms != NULL ? c->host.mono_ms(c->host.ctx) : 0;
}

static sc2e_t *lock(sc2c_t *c)
{
    return c->host.engine_lock(c->host.ctx);
}

static void unlock(sc2c_t *c)
{
    c->host.engine_unlock(c->host.ctx);
}

static void emit(sc2c_t *c, sc2c_event_t *ev)
{
    if (ev->conv == NULL) {
        ev->conv = "";
    }
    if (ev->from == NULL) {
        ev->from = "";
    }
    if (ev->name == NULL) {
        ev->name = "";
    }
    if (ev->body == NULL) {
        ev->body = "";
    }
    if (ev->msg_id == NULL) {
        ev->msg_id = "";
    }
    if (ev->text == NULL) {
        ev->text = "";
    }
    if (c->host.event != NULL) {
        c->host.event(c->host.ctx, ev);
    }
}

static void notice(sc2c_t *c, int code, const char *text)
{
    sc2c_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC2C_EV_NOTICE;
    ev.code = code;
    ev.text = text;
    emit(c, &ev);
}

static int send_frame(sc2c_t *c, const char *line, size_t len)
{
    if (c->state == SC2C_OFFLINE) {
        return SC2_ERR_STATE;
    }
    if (c->host.send(c->host.ctx, line, len) != 0) {
        return SC2_ERR_STATE;
    }
    c->last_tx_ms = now_ms(c);
    return SC2_OK;
}

static void set_state(sc2c_t *c, sc2c_conn_state_t st)
{
    if (c->state == st) {
        return;
    }
    c->state = (uint8_t)st;
    sc2c_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC2C_EV_STATE;
    ev.code = st;
    emit(c, &ev);
}

static bool hex32_ok(const char *s)
{
    if (s == NULL || strlen(s) != SC2_DEVICE_ID_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < SC2_DEVICE_ID_HEX_LEN; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool public_key_ok(const char *s)
{
    size_t n = s != NULL ? strlen(s) : 0;
    return n > 4 && n <= SC2_ROOM_NAME_MAX && strncmp(s, "pub:", 4) == 0;
}

static void hex_of(char out[SC2_DEVICE_ID_HEX_LEN + 1], const uint8_t id[SC2_DEVICE_ID_LEN])
{
    (void)sc2_hex_encode(out, SC2_DEVICE_ID_HEX_LEN + 1, id, SC2_DEVICE_ID_LEN);
}

static int room_index(const sc2c_t *c, const char *key)
{
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        if (c->rooms[i].key[0] != '\0' && strcmp(c->rooms[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void room_event(sc2c_t *c, sc2c_room_t *r, sc2c_room_state_t st, const char *text)
{
    bool changed = r->state != st;
    r->state = (uint8_t)st;
    if (!changed && text == NULL) {
        return;
    }
    sc2c_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC2C_EV_ROOM;
    ev.kind = (sc2c_kind_t)r->kind;
    ev.conv = r->key;
    ev.code = st;
    ev.text = text;
    emit(c, &ev);
}

/* rid "<letter><index>" */
static int rid_index(const char *rid, char letter, unsigned max)
{
    if (rid == NULL || rid[0] != letter || rid[1] == '\0') {
        return -1;
    }
    unsigned v = 0;
    for (const char *p = rid + 1; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || v > 1000) {
            return -1;
        }
        v = v * 10u + (unsigned)(*p - '0');
    }
    return v < max ? (int)v : -1;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t fnv32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* persistent records                                                  */

static void slot_name(const sc2c_t *c, char out[SC2E_NAME_MAX], unsigned slot)
{
    snprintf(out, SC2E_NAME_MAX, "sc2c/%s_o%u", c->tag, slot);
}

static void room_name(const sc2c_t *c, char out[SC2E_NAME_MAX], const char *room)
{
    snprintf(out, SC2E_NAME_MAX, "sc2c/%s_r%s", c->tag, room);
}

/* Load the private room record into c->rec. */
static int rec_load(sc2c_t *c, const char *room)
{
    char name[SC2E_NAME_MAX];
    room_name(c, name, room);
    size_t len = 0;
    const uint8_t *pl = NULL;
    size_t pl_len = 0;
    sc2e_t *e = lock(c);
    int rc = e->pf.load(e->pf.ctx, name, (uint8_t *)c->work, SC2C_WORK_MAX, &len);
    unlock(c);
    if (rc == SC2_OK) {
        uint8_t version = len > 5 ? (uint8_t)c->work[5] : 0;
        rc = version == 1 || version == REC_ROOM_VERSION ?
            sc2e_record_unwrap((const uint8_t *)c->work, len, REC_ROOM_TYPE, version, &pl, &pl_len) : SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK && pl_len != sizeof(c->rec) &&
        !((uint8_t)c->work[5] == 1 && pl_len == offsetof(sc2c_roomrec_t, title))) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        memset(&c->rec, 0, sizeof(c->rec));
        memcpy(&c->rec, pl, pl_len);
        if (!memchr(c->rec.title, 0, sizeof(c->rec.title)) ||
            (c->rec.title[0] && !sc2e_label_valid(c->rec.title, SC2E_ROOM_TITLE_MAX))) rc = SC2_ERR_FORMAT;
    } else {
        memset(&c->rec, 0, sizeof(c->rec));
    }
    sc2_memzero(c->work, len < SC2C_WORK_MAX ? len : SC2C_WORK_MAX);
    return rc;
}

static int rec_save(sc2c_t *c, const char *room)
{
    char name[SC2E_NAME_MAX];
    room_name(c, name, room);
    size_t len = 0;
    int rc = sc2e_record_wrap((uint8_t *)c->work, SC2C_WORK_MAX, &len, REC_ROOM_TYPE, REC_ROOM_VERSION,
                              (const uint8_t *)&c->rec, sizeof(c->rec));
    if (rc == SC2_OK) {
        sc2e_t *e = lock(c);
        rc = e->pf.save(e->pf.ctx, name, (const uint8_t *)c->work, len);
        unlock(c);
    }
    sc2_memzero(c->work, len);
    return rc;
}

static void rec_remove(sc2c_t *c, const char *room)
{
    char name[SC2E_NAME_MAX];
    room_name(c, name, room);
    sc2e_t *e = lock(c);
    (void)e->pf.remove(e->pf.ctx, name);
    unlock(c);
}

/* Outbox record layout in c->tx: header (OB_HDR bytes) then the frame. */
static char *ob_frame(sc2c_t *c)
{
    return c->tx + OB_HDR;
}

static int ob_save(sc2c_t *c, unsigned slot, uint8_t kind, uint32_t token, uint64_t key, const char *target,
                   size_t frame_len)
{
    uint8_t *h = (uint8_t *)c->tx + 12u;
    memset(h, 0, OB_RECORD_HDR);
    memcpy(h, OB_MAGIC, 4);
    h[4] = OB_VERSION;
    h[5] = kind;
    put_u32(h + 6, token);
    put_u32(h + 10, (uint32_t)(key >> 32));
    put_u32(h + 14, (uint32_t)key);
    strncpy((char *)h + 18, target, SC2C_KEY_MAX);
    put_u32(h + 18 + SC2C_KEY_MAX + 1, (uint32_t)frame_len);
    put_u32(h + 22 + SC2C_KEY_MAX + 1, fnv32((const uint8_t *)ob_frame(c), frame_len));
    char name[SC2E_NAME_MAX];
    slot_name(c, name, slot);
    size_t stored_len = 0;
    int rc = sc2e_record_wrap((uint8_t *)c->tx, SC2C_TX_MAX, &stored_len, REC_OUTBOX_TYPE, OB_VERSION,
                              h, OB_RECORD_HDR + frame_len);
    if (rc != SC2_OK) return rc;
    sc2e_t *e = lock(c);
    rc = e->pf.save(e->pf.ctx, name, (const uint8_t *)c->tx, stored_len);
    unlock(c);
    if (rc == SC2_OK) {
        sc2c_slot_t *s = &c->slots[slot];
        s->used = true;
        s->inflight = false;
        s->kind = kind;
        s->token = token;
        s->token_key = key;
        strncpy(s->target, target, SC2C_KEY_MAX);
        s->target[SC2C_KEY_MAX] = '\0';
    }
    return rc;
}

/* Load slot into c->tx; *frame_len gets the frame length. */
static int ob_load(sc2c_t *c, unsigned slot, size_t *frame_len)
{
    char name[SC2E_NAME_MAX];
    slot_name(c, name, slot);
    size_t len = 0;
    sc2e_t *e = lock(c);
    int rc = e->pf.load(e->pf.ctx, name, (uint8_t *)c->tx, SC2C_TX_MAX - 1, &len);
    unlock(c);
    if (rc != SC2_OK) {
        return rc;
    }
    const uint8_t *h = NULL;
    size_t payload_len = 0;
    rc = sc2e_record_unwrap((const uint8_t *)c->tx, len, REC_OUTBOX_TYPE, OB_VERSION, &h, &payload_len);
    if (rc != SC2_OK || payload_len < OB_RECORD_HDR || memcmp(h, OB_MAGIC, 4) != 0 || h[4] != OB_VERSION) {
        return SC2_ERR_FORMAT;
    }
    size_t fl = get_u32(h + 18 + SC2C_KEY_MAX + 1);
    if (fl != payload_len - OB_RECORD_HDR ||
        get_u32(h + 22 + SC2C_KEY_MAX + 1) != fnv32(h + OB_RECORD_HDR, fl)) {
        return SC2_ERR_FORMAT;
    }
    c->tx[len] = '\0';
    *frame_len = fl;
    return SC2_OK;
}

static void ob_drop(sc2c_t *c, unsigned slot)
{
    char name[SC2E_NAME_MAX];
    slot_name(c, name, slot);
    sc2e_t *e = lock(c);
    (void)e->pf.remove(e->pf.ctx, name);
    unlock(c);
    memset(&c->slots[slot], 0, sizeof(c->slots[slot]));
}

static int ob_free_slot(const sc2c_t *c)
{
    for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
        if (!c->slots[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static void ob_transmit(sc2c_t *c, unsigned slot)
{
    size_t fl = 0;
    if (c->state < SC2C_SYNCING) {
        return;
    }
    int rc = ob_load(c, slot, &fl);
    if (rc != SC2_OK) {
        notice(c, 0, "outbox record unreadable; dropped");
        sc2c_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SC2C_EV_FAILED;
        ev.token = c->slots[slot].token;
        ev.token_key = c->slots[slot].token_key;
        ev.code = rc;
        ev.text = "outbox record unreadable";
        uint8_t kind = c->slots[slot].kind;
        ob_drop(c, slot);
        if (kind != OB_KIND_SHARE) {
            emit(c, &ev);
        }
        return;
    }
    if (send_frame(c, ob_frame(c), fl) == SC2_OK) {
        c->slots[slot].inflight = true;
    }
}

/* ------------------------------------------------------------------ */
/* frames                                                              */

static int send_simple(sc2c_t *c, const char *fmt, const char *a, const char *b)
{
    char buf[200];
    int n = snprintf(buf, sizeof(buf), fmt, a, b != NULL ? b : "");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return SC2_ERR_BOUNDS;
    }
    return send_frame(c, buf, (size_t)n);
}

static void send_ack(sc2c_t *c, uint64_t seq)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"ack\",\"inbox\":\"%llu\"}", (unsigned long long)seq);
    (void)send_frame(c, buf, (size_t)n);
}

static int send_keys(sc2c_t *c)
{
    size_t n = 0;
    sc2e_t *e = lock(c);
    int rc = sc2e_keys_publish(e, "kp", c->no_opks ? SC2E_OPK_NONE : SC2C_OPK_TARGET, c->tx, SC2C_TX_MAX, &n);
    unlock(c);
    if (rc == SC2_OK) {
        rc = send_frame(c, c->tx, n);
    }
    return rc;
}

static void send_sync(sc2c_t *c, int only_room)
{
    sc2_jw_t w;
    char rid[16];
    size_t n = 0;
    snprintf(rid, sizeof(rid), only_room >= 0 ? "y%d" : "y", only_room);
    sc2_jw_init(&w, c->tx, SC2C_TX_MAX);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "sync");
    sc2_jw_key(&w, "rid");
    sc2_jw_cstring(&w, rid);
    sc2_jw_key(&w, "inbox");
    sc2_jw_u64_str(&w, c->inbox_done);
    sc2_jw_key(&w, "rooms");
    sc2_jw_object_begin(&w);
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        sc2c_room_t *r = &c->rooms[i];
        bool pick = only_room >= 0 ? (int)i == only_room : (r->key[0] != '\0' && r->synced);
        if (pick) {
            sc2_jw_key(&w, r->key);
            sc2_jw_u64_str(&w, r->cursor);
        }
    }
    sc2_jw_object_end(&w);
    sc2_jw_key(&w, "max");
    sc2_jw_u64(&w, SYNC_MAX);
    sc2_jw_object_end(&w);
    if (sc2_jw_finish(&w, &n) == SC2_OK) {
        (void)send_frame(c, c->tx, n);
    }
}

static int send_claim(sc2c_t *c, const char *device)
{
    uint64_t t = now_ms(c);
    int freeslot = -1;
    for (unsigned i = 0; i < SC2C_CLAIMS_MAX; i++) {
        if (strcmp(c->claims[i].device, device) == 0) {
            if (c->claims[i].asked_ms != 0 && t - c->claims[i].asked_ms < CLAIM_RETRY_MS) {
                return SC2_OK; /* already asked */
            }
            freeslot = (int)i;
            break;
        }
        if (freeslot < 0 && c->claims[i].device[0] == '\0') {
            freeslot = (int)i;
        }
    }
    if (freeslot < 0) {
        return SC2C_AGAIN;
    }
    sc2c_claim_t *cl = &c->claims[freeslot];
    memcpy(cl->device, device, SC2_DEVICE_ID_HEX_LEN + 1);
    cl->asked_ms = t != 0 ? t : 1;
    char rid[16];
    snprintf(rid, sizeof(rid), "k%d", freeslot);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"keys.claim\",\"rid\":\"%s\",\"device\":\"%s\"}", rid, device);
    return send_frame(c, buf, (size_t)n);
}

/* Join one room (public join, private join with invite proof when known). */
static void send_join(sc2c_t *c, unsigned idx)
{
    sc2c_room_t *r = &c->rooms[idx];
    char rid[16];
    snprintf(rid, sizeof(rid), "j%u", idx);
    if (r->kind == SC2C_PUBLIC) {
        if (send_simple(c, "{\"type\":\"room.join\",\"rid\":\"%s\",\"room\":\"%s\"}", rid, r->key) == SC2_OK) {
            room_event(c, r, SC2C_ROOM_JOINING, NULL);
        }
        return;
    }
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t proof[32];
    bool have_proof = false;
    if (sc2_hex_decode(room, sizeof(room), r->key, SC2_DEVICE_ID_HEX_LEN) != SC2_OK) {
        return;
    }
    if (rec_load(c, r->key) == SC2_OK && (c->rec.flags & SC2C_RF_INVITE) != 0 &&
        (c->rec.flags & SC2C_RF_ADMIN) == 0) {
        uint8_t ikey[SC2_INVITE_KEY_LEN];
        have_proof = sc2_invite_key(ikey, c->rec.secret, room) == SC2_OK &&
                     sc2_invite_proof(proof, ikey, c->self_id) == SC2_OK;
        sc2_memzero(ikey, sizeof(ikey));
    }
    size_t n = 0;
    if (sc2_frame_room_join_private(c->tx, SC2C_TX_MAX, &n, rid, room, have_proof ? proof : NULL) == SC2_OK &&
        send_frame(c, c->tx, n) == SC2_OK && r->state != SC2C_ROOM_REQUESTED) {
        room_event(c, r, SC2C_ROOM_JOINING, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* admin: commits and epoch key distribution                           */

static int commit_build(sc2c_t *c, sc2e_t *e, uint8_t op, const uint8_t target[SC2_DEVICE_ID_LEN],
                        uint8_t rec[SC2_COMMIT_LEN], uint8_t sig[SC2_SIG_LEN])
{
    sc2_commit_t cm;
    int rc = sc2_commit_make(&cm, &c->rec.roster, c->self_id, op, target);
    if (rc == SC2_OK) {
        rc = sc2_commit_encode(rec, &cm);
    }
    if (rc == SC2_OK && sig != NULL) {
        rc = sc2_commit_sign(sig, rec, e->id.ik_seed);
    }
    return rc;
}

/* Send the next queued add commit for room idx (c->rec loaded). */
static void commit_next(sc2c_t *c, unsigned idx)
{
    sc2c_room_t *r = &c->rooms[idx];
    while (r->commit_op == 0 && r->add_n > 0) {
        uint8_t target[SC2_DEVICE_ID_LEN];
        memcpy(target, r->add_q[0], sizeof(target));
        r->add_n--;
        memmove(r->add_q[0], r->add_q[1], (size_t)r->add_n * SC2_DEVICE_ID_LEN);
        if (sc2_roster_find(&c->rec.roster, target) >= 0) {
            continue;
        }
        uint8_t rec[SC2_COMMIT_LEN], sig[SC2_SIG_LEN];
        sc2e_t *e = lock(c);
        int rc = commit_build(c, e, SC2_COMMIT_ADD, target, rec, sig);
        unlock(c);
        size_t n = 0;
        char rid[16];
        snprintf(rid, sizeof(rid), "c%u", idx);
        if (rc == SC2_OK) {
            rc = sc2_frame_room_commit(c->tx, SC2C_TX_MAX, &n, rid, c->rec.roster.room, rec, sig);
        }
        if (rc == SC2_OK && send_frame(c, c->tx, n) == SC2_OK) {
            r->commit_op = SC2_COMMIT_ADD;
            memcpy(r->commit_target, target, sizeof(target));
        } else {
            notice(c, rc, "add commit could not be sent");
        }
    }
}

static void queue_add(sc2c_t *c, unsigned idx, const uint8_t dev[SC2_DEVICE_ID_LEN])
{
    sc2c_room_t *r = &c->rooms[idx];
    if (memcmp(r->commit_target, dev, SC2_DEVICE_ID_LEN) == 0 && r->commit_op != 0) {
        return;
    }
    for (unsigned i = 0; i < r->add_n; i++) {
        if (memcmp(r->add_q[i], dev, SC2_DEVICE_ID_LEN) == 0) {
            return;
        }
    }
    if (r->add_n < 4) {
        memcpy(r->add_q[r->add_n++], dev, SC2_DEVICE_ID_LEN);
    }
}

/* Seal and queue the current epoch key for every roster member still owed
 * it in room idx (c->rec loaded). Members without keys get a claim. */
static void shares_flush_room(sc2c_t *c, unsigned idx)
{
    sc2c_room_t *r = &c->rooms[idx];
    const sc2_roster_t *ro = &c->rec.roster;
    if (!sc2_roster_is_admin(ro, c->self_id)) return;
    for (unsigned m = 0; m < ro->count && r->share_pending != 0; m++) {
        if ((r->share_pending & (1u << m)) == 0) {
            continue;
        }
        if (memcmp(ro->m[m].id, c->self_id, SC2_DEVICE_ID_LEN) == 0) {
            r->share_pending &= ~(1u << m);
            continue;
        }
        int slot = ob_free_slot(c);
        if (slot < 0) {
            return;
        }
        char peer[SC2_DEVICE_ID_HEX_LEN + 1];
        hex_of(peer, ro->m[m].id);
        sc2e_peer_info_t pi;
        sc2e_t *e = lock(c);
        int rc = sc2e_peer_info(e, peer, &pi);
        unlock(c);
        if (rc != SC2_OK || (!pi.known && !pi.has_session)) {
            (void)send_claim(c, peer);
            continue;
        }
        char rid[16];
        size_t n = 0;
        snprintf(rid, sizeof(rid), "o%d", slot);
        e = lock(c);
        rc = sc2e_room_share_named(e, r->key, peer, rid, ro, c->rec.title[0] ? c->rec.title : NULL,
                                  ob_frame(c), SC2C_TX_MAX - OB_HDR, &n);
        unlock(c);
        if (rc == SC2_OK) {
            rc = ob_save(c, (unsigned)slot, OB_KIND_SHARE, 0, 0, peer, n);
        }
        if (rc == SC2_OK) {
            r->share_pending &= ~(1u << m);
            if (send_frame(c, ob_frame(c), n) == SC2_OK) {
                c->slots[slot].inflight = true;
            }
        } else {
            notice(c, rc, "epoch key share failed");
            r->share_pending &= ~(1u << m);
        }
    }
}

static void shares_flush(sc2c_t *c)
{
    if (c->state < SC2C_SYNCING) {
        return;
    }
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        sc2c_room_t *r = &c->rooms[i];
        if (r->key[0] != '\0' && r->kind == SC2C_PRIVATE && r->share_pending != 0 && rec_load(c, r->key) == SC2_OK) {
            shares_flush_room(c, i);
        }
    }
}

/* Bring the engine epoch up to the roster epoch (after a commit, or after a
 * restart that lost the rotate) and owe the key to every member. */
static void admin_catch_up(sc2c_t *c, unsigned idx)
{
    sc2c_room_t *r = &c->rooms[idx];
    sc2e_room_info_t info;
    bool rotated = false;
    sc2e_t *e = lock(c);
    int rc = sc2e_room_info(e, r->key, &info);
    while (rc == SC2_OK && info.known && info.epoch < c->rec.roster.epoch) {
        uint32_t ep = 0;
        rc = sc2e_room_rotate(e, r->key, &ep);
        info.epoch = ep;
        rotated = true;
    }
    unlock(c);
    if (rc != SC2_OK || !info.known) {
        notice(c, rc, "admin room key missing from the engine");
        return;
    }
    if (rotated) {
        r->share_pending = c->rec.roster.count >= 32 ? 0xffffffffu : ((1u << c->rec.roster.count) - 1u);
    }
}

/* ------------------------------------------------------------------ */
/* inbound                                                             */

/* Unlike the relay's invite_key, this proof requires the out-of-band
 * secret. Bind it to both the room and the authenticated DM sender. */
static int admission_proof(uint8_t proof[32], const uint8_t secret[SC2_INVITE_SECRET_LEN],
                           const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t device[SC2_DEVICE_ID_LEN])
{
    static const uint8_t label[] = "SC2-ADMIN-JOIN\x01";
    uint8_t data[sizeof(label) - 1u + SC2_ROOM_ID_LEN + SC2_DEVICE_ID_LEN];
    memcpy(data, label, sizeof(label) - 1u);
    memcpy(data + sizeof(label) - 1u, room, SC2_ROOM_ID_LEN);
    memcpy(data + sizeof(label) - 1u + SC2_ROOM_ID_LEN, device, SC2_DEVICE_ID_LEN);
    return sc2_hmac_sha256(proof, secret, SC2_INVITE_SECRET_LEN, data, sizeof(data));
}

static void admissions_flush(sc2c_t *c)
{
    if (c->state < SC2C_SYNCING) return;
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        sc2c_room_t *r = &c->rooms[i];
        uint64_t now = now_ms(c);
        if (!r->want || r->kind != SC2C_PRIVATE ||
            (r->state != SC2C_ROOM_REQUESTED && r->state != SC2C_ROOM_WAITKEY) ||
            (r->admission_ms && now - r->admission_ms < CLAIM_RETRY_MS) ||
            rec_load(c, r->key) != SC2_OK || !(c->rec.flags & SC2C_RF_INVITE) ||
            (c->rec.flags & SC2C_RF_ADMIN)) continue;
        int slot = ob_free_slot(c);
        if (slot < 0) return;
        char admin[SC2_DEVICE_ID_HEX_LEN + 1];
        hex_of(admin, c->rec.admin_id);
        sc2e_peer_info_t pi;
        sc2e_t *e = lock(c);
        int rc = sc2e_peer_info(e, admin, &pi);
        unlock(c);
        if (rc != SC2_OK) continue;
        if (!pi.known) { (void)send_claim(c, admin); continue; }
        uint8_t proof[32];
        rc = admission_proof(proof, c->rec.secret, c->rec.roster.room, c->self_id);
        size_t inner_len = 0, n = 0;
        sc2_jw_t w;
        sc2_jw_init(&w, c->work, SC2C_WORK_MAX);
        sc2_jw_object_begin(&w);
        sc2_jw_key(&w, "t"); sc2_jw_cstring(&w, "room_join");
        sc2_jw_key(&w, "room"); sc2_jw_cstring(&w, r->key);
        sc2_jw_key(&w, "proof"); sc2_jw_b64(&w, proof, sizeof(proof));
        sc2_jw_object_end(&w);
        if (rc == SC2_OK) rc = sc2_jw_finish(&w, &inner_len);
        char rid[16];
        snprintf(rid, sizeof(rid), "o%d", slot);
        if (rc == SC2_OK) {
            e = lock(c);
            rc = sc2e_dm_encrypt(e, admin, rid, c->work, inner_len, ob_frame(c), SC2C_TX_MAX - OB_HDR, &n);
            unlock(c);
        }
        sc2_memzero(proof, sizeof(proof));
        sc2_memzero(c->work, inner_len);
        if (rc == SC2_OK) rc = ob_save(c, (unsigned)slot, OB_KIND_SHARE, 0, 0, admin, n);
        if (rc == SC2_OK) {
            r->admission_ms = now ? now : 1;
            ob_transmit(c, (unsigned)slot);
        }
    }
}

static void on_admission(sc2c_t *c, const sc2e_dm_result_t *res, const sc2_json_val_t *root)
{
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    uint8_t proof[32], expected[32], device[SC2_DEVICE_ID_LEN];
    if (!c->auto_admit || sc2_json_get_string(root, "room", room, sizeof(room), NULL) != SC2_OK ||
        sc2_json_get_b64_exact(root, "proof", proof, sizeof(proof)) != SC2_OK ||
        sc2_hex_decode(device, sizeof(device), res->from, SC2_DEVICE_ID_HEX_LEN) != SC2_OK) return;
    int idx = room_index(c, room);
    if (idx < 0 || !c->rooms[idx].want || c->rooms[idx].kind != SC2C_PRIVATE ||
        rec_load(c, room) != SC2_OK || !(c->rec.flags & SC2C_RF_ADMIN) ||
        !sc2_roster_is_admin(&c->rec.roster, c->self_id) ||
        admission_proof(expected, c->rec.secret, c->rec.roster.room, device) != SC2_OK ||
        !sc2_ct_eq(proof, expected, sizeof(proof))) {
        notice(c, SC2_ERR_AUTH, "private-room admission proof rejected");
        return;
    }
    int member = sc2_roster_find(&c->rec.roster, device);
    if (member >= 0) {
        c->rooms[idx].share_pending |= 1u << (unsigned)member;
        shares_flush_room(c, (unsigned)idx);
    } else {
        queue_add(c, (unsigned)idx, device);
        commit_next(c, (unsigned)idx);
    }
}

/* c->tx holds the opened control throughout authorization. rec_load/save
 * use c->work, so the key cannot be installed before this check completes. */
static bool authorize_control(sc2c_t *c, const sc2e_dm_result_t *res,
                               const sc2_json_val_t *root, bool history)
{
    char room[SC2_DEVICE_ID_HEX_LEN + 1], admin[SC2_DEVICE_ID_HEX_LEN + 1];
    if (sc2_json_get_string(root, "room", room, sizeof(room), NULL) != SC2_OK) return false;
    int idx = room_index(c, room);
    if (idx < 0 || !c->rooms[idx].want || c->rooms[idx].kind != SC2C_PRIVATE ||
        c->rooms[idx].state == SC2C_ROOM_LEFT || rec_load(c, room) != SC2_OK ||
        !(c->rec.flags & SC2C_RF_INVITE) || (c->rec.flags & SC2C_RF_ADMIN)) return false;
    hex_of(admin, c->rec.admin_id);
    if (strcmp(admin, res->from) || (c->rec.roster.count &&
        !sc2_roster_is_admin(&c->rec.roster, c->rec.admin_id))) return false;
    uint8_t ik[SC2_KEY_LEN];
    sc2e_t *e = lock(c);
    int identity_rc = sc2e_peer_identity(e, res->from, ik);
    unlock(c);
    if (identity_rc != SC2_OK || !sc2_ct_eq(ik, c->rec.admin_ik, sizeof(ik))) return false;
    if (history) return c->rec.roster.count && sc2_roster_find(&c->rec.roster, c->self_id) >= 0;

    uint64_t epoch;
    sc2_roster_t roster;
    uint8_t members[17u * SC2_ROSTER_CAP];
    size_t members_len = 0;
    sc2_json_val_t v;
    sc2_roster_init(&roster, c->rec.roster.room);
    if (sc2_json_get_u64(root, "epoch", UINT32_MAX, &epoch) != SC2_OK || !epoch ||
        epoch < c->rec.roster.epoch ||
        sc2_json_get_b64_exact(root, "head", roster.head, sizeof(roster.head)) != SC2_OK ||
        sc2_json_get(root, "roster", &v) != SC2_OK ||
        sc2_json_b64(&v, members, sizeof(members), &members_len) != SC2_OK ||
        !members_len || members_len % 17u) return false;
    roster.epoch = (uint32_t)epoch;
    roster.count = (uint8_t)(members_len / 17u);
    for (unsigned i = 0; i < roster.count; i++) {
        memcpy(roster.m[i].id, members + 17u * i, SC2_DEVICE_ID_LEN);
        roster.m[i].role = members[17u * i + SC2_DEVICE_ID_LEN];
        if (roster.m[i].role > SC2_ROLE_ADMIN ||
            (i && memcmp(roster.m[i - 1].id, roster.m[i].id, SC2_DEVICE_ID_LEN) >= 0)) return false;
    }
    if (!sc2_roster_is_admin(&roster, c->rec.admin_id) || sc2_roster_find(&roster, c->self_id) < 0) return false;
    if (c->rec.roster.count && roster.epoch == c->rec.roster.epoch) {
        uint8_t old_hash[32], new_hash[32];
        if (!sc2_ct_eq(roster.head, c->rec.roster.head, sizeof(roster.head)) ||
            sc2_roster_hash(&roster, new_hash) != SC2_OK ||
            sc2_roster_hash(&c->rec.roster, old_hash) != SC2_OK ||
            !sc2_ct_eq(old_hash, new_hash, sizeof(old_hash))) return false;
    }
    c->rec.roster = roster;
    sc2_json_val_t title_value;
    if (sc2_json_get(root, "title", &title_value) == SC2_OK) {
        char title[SC2E_ROOM_TITLE_MAX + 1];
        if (sc2_json_string(&title_value, title, sizeof(title), NULL) != SC2_OK ||
            !sc2e_label_valid(title, SC2E_ROOM_TITLE_MAX)) return false;
        memcpy(c->rec.title, title, strlen(title) + 1u);
    }
    return rec_save(c, room) == SC2_OK;
}

static void deliver_text(sc2c_t *c, sc2c_kind_t kind, const char *conv, const char *from, uint64_t seq,
                         const char *pt, size_t pl, bool replay)
{
    /* pt is inner JSON in c->work; body is unescaped into c->tx */
    sc2_json_val_t root, v;
    char id[2 * SC2_MSG_ID_LEN + 1] = "";
    char name[SC2C_NICK_MAX + 1] = "";
    uint64_t ts = 0;
    size_t bl = 0;
    if (sc2_json_root(pt, pl, &root) != SC2_OK || root.type != SC2_JSON_OBJECT) {
        notice(c, 0, "message with malformed inner JSON ignored");
        return;
    }
    if (sc2_json_get(&root, "t", &v) != SC2_OK || !sc2_json_string_eq(&v, "text")) {
        return; /* receipts, typing and future types: not shown */
    }
    if (sc2_json_get_string(&root, "body", c->tx, SC2C_TX_MAX, &bl) != SC2_OK) {
        notice(c, 0, "message without a text body ignored");
        return;
    }
    (void)sc2_json_get_string(&root, "id", id, sizeof(id), NULL);
    if (sc2_json_get_string(&root, "name", name, sizeof(name), NULL) != SC2_OK ||
        !sc2e_label_valid(name, SC2C_NICK_MAX)) name[0] = 0;
    (void)sc2_json_get_u64(&root, "ts", UINT64_MAX >> 1, &ts);
    sc2c_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC2C_EV_MESSAGE;
    ev.kind = kind;
    ev.conv = conv;
    ev.from = from;
    ev.name = name;
    ev.body = c->tx;
    ev.body_len = bl;
    ev.msg_id = id;
    ev.seq = seq;
    ev.ts = ts;
    ev.replay = replay;
    emit(c, &ev);
    sc2_memzero(c->tx, bl);
}

/* NOT_FOUND on decrypt: the sender's identity is not known yet. Ask the
 * relay for its bundle unless the relay already said it has none. */
static bool dm_sender_claim(sc2c_t *c, const sc2_json_val_t *root)
{
    sc2_json_val_t env;
    char from[SC2_DEVICE_ID_HEX_LEN + 1];
    if (sc2_json_get(root, "env", &env) != SC2_OK ||
        sc2_json_get_string(&env, "from", from, sizeof(from), NULL) != SC2_OK || !hex32_ok(from)) {
        return false;
    }
    for (unsigned i = 0; i < SC2C_CLAIMS_MAX; i++) {
        if (strcmp(c->claims[i].device, from) == 0 && c->claims[i].asked_ms == UINT64_MAX) {
            return false;
        }
    }
    int rc = send_claim(c, from);
    return rc == SC2_OK || rc == SC2C_AGAIN;
}

static void dm_resync(sc2c_t *c)
{
    if (c->dm_hold != 0) {
        c->dm_hold = 0;
        send_sync(c, -1);
    }
}

static void on_dm(sc2c_t *c, const char *line, size_t len, bool replay)
{
    sc2e_dm_result_t res;
    size_t pl = 0;
    uint64_t seq = 0;
    sc2_json_val_t root, sv;
    if (sc2_json_root(line, len, &root) == SC2_OK && sc2_json_get(&root, "seq", &sv) == SC2_OK) {
        (void)sc2_json_u64_str(&sv, &seq);
    }
    if (seq != 0 && seq <= c->inbox_done) {
        return; /* duplicate of an item handled this session */
    }
    if (c->dm_hold != 0 && seq >= c->dm_hold) {
        return; /* comes again with the resync after the held item */
    }
    memset(&res, 0, sizeof(res));
    sc2e_t *e = lock(c);
    int rc = sc2e_dm_open(e, line, len, &res, c->work, SC2C_WORK_MAX, &pl);
    unlock(c);
    if (rc == SC2_ERR_NOT_FOUND && seq != 0 && dm_sender_claim(c, &root)) {
        /* First message from a device we have no keys for: fetch its bundle,
         * leave this item (and everything after it) unacked and resync once
         * the bundle is here. */
        c->dm_hold = seq;
        c->dm_hold_ms = now_ms(c);
        return;
    }
    if (res.seq == 0) {
        res.seq = seq;
    }
    if (rc != SC2_OK) {
        notice(c, rc, "undecryptable DM dropped");
    } else {
        sc2_json_val_t inner, type;
        bool parsed = sc2_json_root(c->work, pl, &inner) == SC2_OK &&
            sc2_json_get(&inner, "t", &type) == SC2_OK;
        bool history = parsed && sc2_json_string_eq(&type, "history");
        if (parsed && (history || sc2_json_string_eq(&type, "epoch_key"))) {
            memcpy(c->tx, c->work, pl + 1u);
            (void)sc2_json_root(c->tx, pl, &inner);
            if (!authorize_control(c, &res, &inner, history)) {
                notice(c, SC2_ERR_AUTH, "unauthorized room key ignored before installation");
            } else {
                e = lock(c);
                sc2e_room_info_t prior;
                char room[SC2_DEVICE_ID_HEX_LEN + 1];
                if (sc2_json_get_string(&inner, "room", room, sizeof(room), NULL) == SC2_OK &&
                    sc2e_room_info(e, room, &prior) == SC2_OK && prior.known && strcmp(prior.admin, res.from))
                    (void)sc2e_room_forget(e, room);
                rc = sc2e_dm_apply_control(e, &res, c->tx, pl);
                unlock(c);
                int idx = room_index(c, res.room);
                if (rc == SC2_OK && res.kind == SC2E_DM_EPOCH_KEY && idx >= 0) {
                    c->rooms[idx].commit_hold = 0;
                    room_event(c, &c->rooms[idx], SC2C_ROOM_JOINED, NULL);
                } else if (rc != SC2_OK) notice(c, rc, "authorized room key could not be installed");
            }
            sc2_memzero(c->tx, pl);
        } else if (parsed && sc2_json_string_eq(&type, "room_join")) {
            on_admission(c, &res, &inner);
        } else {
            deliver_text(c, SC2C_DM, res.from, res.from, res.seq, c->work, pl, replay);
        }
    }
    sc2_memzero(c->work, pl);
    if (res.seq != 0) {
        c->inbox_done = res.seq;
        send_ack(c, res.seq);
    }
}

static bool room_seq_new(sc2c_t *c, int idx, uint64_t seq)
{
    if (idx < 0) {
        return false;
    }
    if (c->rooms[idx].commit_hold && seq >= c->rooms[idx].commit_hold) return false;
    if (seq != 0 && seq <= c->rooms[idx].cursor) {
        return false;
    }
    if (seq > c->rooms[idx].cursor) {
        c->rooms[idx].cursor = seq;
    }
    return true;
}

static void on_room_msg(sc2c_t *c, const char *line, size_t len, bool replay)
{
    sc2_json_val_t root;
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    uint64_t seq = 0;
    sc2_json_val_t sv;
    if (sc2_json_root(line, len, &root) != SC2_OK ||
        sc2_json_get_string(&root, "room", room, sizeof(room), NULL) != SC2_OK) {
        return;
    }
    if (sc2_json_get(&root, "seq", &sv) == SC2_OK) {
        (void)sc2_json_u64_str(&sv, &seq);
    }
    int idx = room_index(c, room);
    if (!room_seq_new(c, idx, seq)) {
        return;
    }
    sc2e_room_result_t res;
    size_t pl = 0;
    sc2e_t *e = lock(c);
    int rc = sc2e_room_decrypt(e, line, len, &res, c->work, SC2C_WORK_MAX, &pl);
    unlock(c);
    if (rc == SC2_OK) {
        if (strcmp(res.from, c->self_hex) != 0) {
            deliver_text(c, SC2C_PRIVATE, room, res.from, seq, c->work, pl, replay);
        }
    } else {
        /* own messages come back on sync and have no receiver chain */
        if (strstr(line, c->self_hex) == NULL) {
            notice(c, rc, "undecryptable room message dropped");
        }
    }
    sc2_memzero(c->work, pl);
}

static void on_room_pub(sc2c_t *c, const char *line, size_t len, bool replay)
{
    sc2e_post_result_t res;
    memset(&res, 0, sizeof(res));
    int rc = sc2e_post_verify(c->relay_id, line, len, SC2_POST_BODY_MAX, &res, c->work, SC2C_WORK_MAX);
    if (rc != SC2_OK) {
        notice(c, rc, "malformed public post dropped");
        return;
    }
    int idx = room_index(c, res.room);
    if (!room_seq_new(c, idx, res.seq)) {
        return;
    }
    if (!res.valid) {
        notice(c, 0, "public post with a bad signature dropped");
        return;
    }
    if (res.deleted || strcmp(res.from, c->self_hex) == 0) {
        return;
    }
    sc2c_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC2C_EV_MESSAGE;
    ev.kind = SC2C_PUBLIC;
    ev.conv = res.room;
    ev.from = res.from;
    ev.name = res.name;
    ev.body = res.body;
    ev.body_len = res.body_len;
    ev.msg_id = res.id;
    ev.seq = res.seq;
    ev.ts = res.ts;
    ev.replay = replay;
    emit(c, &ev);
}

static void on_commit(sc2c_t *c, const char *line, size_t len)
{
    uint8_t room[SC2_ROOM_ID_LEN], rec[SC2_COMMIT_LEN], sig[SC2_SIG_LEN];
    uint64_t seq = 0;
    char key[SC2_DEVICE_ID_HEX_LEN + 1];
    sc2_commit_t cm;
    if (sc2_frame_room_commit_decode(line, len, room, &seq, rec, sig) != SC2_OK ||
        sc2_commit_decode(&cm, rec) != SC2_OK) {
        notice(c, 0, "malformed commit ignored");
        return;
    }
    hex_of(key, room);
    int idx = room_index(c, key);
    if (idx < 0 || !c->rooms[idx].want || c->rooms[idx].kind != SC2C_PRIVATE ||
        c->rooms[idx].state == SC2C_ROOM_LEFT || memcmp(cm.room, room, sizeof(room)) ||
        (memcmp(cm.by, c->self_id, SC2_DEVICE_ID_LEN) == 0 && c->rooms[idx].commit_op)) {
        return;
    }
    if (rec_load(c, key) != SC2_OK || !c->rec.roster.count ||
        !sc2_roster_is_admin(&c->rec.roster, cm.by)) {
        notice(c, SC2_ERR_AUTH, "commit without a trusted admin roster ignored");
        return;
    }
    uint8_t ik[SC2_KEY_LEN];
    if (memcmp(cm.by, c->rec.admin_id, SC2_DEVICE_ID_LEN) == 0) {
        memcpy(ik, c->rec.admin_ik, sizeof(ik));
    } else {
        char signer[SC2_DEVICE_ID_HEX_LEN + 1];
        hex_of(signer, cm.by);
        sc2e_t *e = lock(c);
        int rc = memcmp(cm.by, c->self_id, SC2_DEVICE_ID_LEN) == 0 ? SC2_OK :
            sc2e_peer_identity(e, signer, ik);
        if (memcmp(cm.by, c->self_id, SC2_DEVICE_ID_LEN) == 0) memcpy(ik, e->id.ik_pub, sizeof(ik));
        unlock(c);
        if (rc != SC2_OK) {
            if (seq && (!c->rooms[idx].commit_hold || seq < c->rooms[idx].commit_hold)) {
                c->rooms[idx].commit_hold = seq;
                if (c->rooms[idx].cursor >= seq) c->rooms[idx].cursor = seq - 1;
            }
            room_event(c, &c->rooms[idx], SC2C_ROOM_WAITKEY, "fetching the membership signer key");
            (void)send_claim(c, signer);
            return;
        }
    }
    if (sc2_commit_verify(sig, rec, ik) != SC2_OK) {
        notice(c, SC2_ERR_AUTH, "commit with an invalid admin signature ignored");
        return;
    }
    uint8_t id[SC2_COMMIT_ID_LEN];
    if (sc2_commit_id(id, rec) == SC2_OK && cm.epoch == c->rec.roster.epoch &&
        sc2_ct_eq(id, c->rec.roster.head, sizeof(id))) return;
    if (sc2_roster_apply(&c->rec.roster, rec) != SC2_OK || rec_save(c, key) != SC2_OK) {
        notice(c, SC2_ERR_STATE, "commit with an invalid chain or roster ignored");
        return;
    }
    if (seq > c->rooms[idx].cursor) c->rooms[idx].cursor = seq;
    c->rooms[idx].commit_hold = 0;
    if (cm.op == SC2_COMMIT_REMOVE && memcmp(cm.target, c->self_id, SC2_DEVICE_ID_LEN) == 0) {
        sc2e_t *e = lock(c);
        (void)sc2e_room_forget(e, key);
        unlock(c);
        c->rooms[idx].want = false;
        room_event(c, &c->rooms[idx], SC2C_ROOM_LEFT, "removed by an admin");
    } else if ((c->rec.flags & SC2C_RF_ADMIN) && sc2_roster_is_admin(&c->rec.roster, c->self_id)) {
        admin_catch_up(c, (unsigned)idx);
        shares_flush_room(c, (unsigned)idx);
    } else {
        room_event(c, &c->rooms[idx], SC2C_ROOM_WAITKEY, "waiting for the authenticated epoch key");
    }
}

static void on_room_event(sc2c_t *c, const sc2_json_val_t *root)
{
    char room[SC2C_KEY_MAX + 1], dev[SC2_DEVICE_ID_HEX_LEN + 1];
    sc2_json_val_t v;
    uint64_t seq = 0;
    bool inbox = sc2_json_get(root, "room_seq", &v) == SC2_OK;
    if (sc2_json_get_string(root, "room", room, sizeof(room), NULL) != SC2_OK ||
        sc2_json_get_string(root, "device", dev, sizeof(dev), NULL) != SC2_OK ||
        sc2_json_get(root, "event", &v) != SC2_OK) {
        return;
    }
    sc2_json_val_t sv;
    if (sc2_json_get(root, "seq", &sv) == SC2_OK) {
        (void)sc2_json_u64_str(&sv, &seq);
    }
    int idx = room_index(c, room);
    if (inbox) {
        if (seq != 0 && seq <= c->inbox_done) {
            return;
        }
        if (seq != 0) {
            c->inbox_done = seq;
            send_ack(c, seq);
        }
        return;
    }
    if (!room_seq_new(c, idx, seq)) {
        return;
    }
    if (sc2_json_string_eq(&v, "join_request")) {
        /* Relay notifications are hints, never admission authority. */
        notice(c, 0, "join request awaits an end-to-end invite proof");
    }
}

static bool json_array_has(const sc2_json_val_t *obj, const char *key, const char *want)
{
    sc2_json_val_t arr, it;
    size_t pos = 0;
    if (sc2_json_get(obj, key, &arr) != SC2_OK || arr.type != SC2_JSON_ARRAY) {
        return false;
    }
    while (sc2_json_array_next(&arr, &pos, &it) == SC2_OK) {
        if (sc2_json_string_eq(&it, want)) {
            return true;
        }
    }
    return false;
}

static void on_room_state(sc2c_t *c, const sc2_json_val_t *root)
{
    char room[SC2C_KEY_MAX + 1];
    if (sc2_json_get_string(root, "room", room, sizeof(room), NULL) != SC2_OK) {
        return;
    }
    int idx = room_index(c, room);
    if (idx < 0) {
        return;
    }
    sc2c_room_t *r = &c->rooms[idx];
    if (r->kind == SC2C_PUBLIC) {
        sc2_json_val_t lim;
        uint64_t mx = 0;
        if (sc2_json_get(root, "limits", &lim) == SC2_OK && sc2_json_get_u64(&lim, "msg", 65535, &mx) == SC2_OK &&
            mx > 0) {
            c->pub_max = (uint32_t)mx;
        }
        char text[160] = "";
        (void)sc2_json_get_string(root, "notice", text, sizeof(text), NULL);
        room_event(c, r, SC2C_ROOM_JOINED, text[0] != '\0' ? text : NULL);
    } else {
        if (!r->want || r->state == SC2C_ROOM_LEFT) return;
        if (!json_array_has(root, "members", c->self_hex)) {
            return;
        }
        bool admin = json_array_has(root, "admins", c->self_hex);
        sc2e_room_info_t info;
        sc2e_t *e = lock(c);
        int rc = sc2e_room_info(e, room, &info);
        unlock(c);
        if (admin && rec_load(c, room) == SC2_OK && (c->rec.flags & SC2C_RF_ADMIN) != 0 &&
            sc2_roster_is_admin(&c->rec.roster, c->self_id)) {
            admin_catch_up(c, (unsigned)idx);
            shares_flush_room(c, (unsigned)idx);
        }
        bool trusted = rec_load(c, room) == SC2_OK && c->rec.roster.count &&
            sc2_roster_find(&c->rec.roster, c->self_id) >= 0;
        room_event(c, r, trusted && !r->commit_hold && rc == SC2_OK && info.known && info.epoch == c->rec.roster.epoch ?
                   SC2C_ROOM_JOINED : SC2C_ROOM_WAITKEY, NULL);
    }
    if (!r->synced) {
        r->synced = true;
        send_sync(c, idx);
    }
}

static void on_sync_done(sc2c_t *c, const sc2_json_val_t *root)
{
    sc2_json_val_t rooms, v;
    bool more = false;
    uint64_t inbox = 0;
    if (sc2_json_get(root, "more", &v) == SC2_OK) {
        (void)sc2_json_bool(&v, &more);
    }
    if (sc2_json_get(root, "inbox", &v) == SC2_OK && sc2_json_u64_str(&v, &inbox) == SC2_OK &&
        c->dm_hold != 0 && inbox >= c->dm_hold) {
        inbox = c->dm_hold - 1; /* never ack past a held item */
    }
    if (inbox > c->inbox_done) {
        c->inbox_done = inbox;
        send_ack(c, inbox);
    }
    int more_room = -1;
    if (sc2_json_get(root, "rooms", &rooms) == SC2_OK && rooms.type == SC2_JSON_OBJECT) {
        for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
            sc2c_room_t *r = &c->rooms[i];
            uint64_t cur = 0;
            if (r->key[0] == '\0' || sc2_json_get(&rooms, r->key, &v) != SC2_OK ||
                sc2_json_u64_str(&v, &cur) != SC2_OK) {
                continue;
            }
            if (r->commit_hold && cur >= r->commit_hold) cur = r->commit_hold - 1;
            if (cur > r->cursor) {
                r->cursor = cur;
            }
            more_room = (int)i;
            sc2c_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SC2C_EV_CURSOR;
            ev.kind = (sc2c_kind_t)r->kind;
            ev.conv = r->key;
            ev.seq = r->cursor;
            emit(c, &ev);
        }
    }
    if (more) {
        send_sync(c, more_room); /* inbox continues from inbox_done */
        return;
    }
    if (c->state == SC2C_SYNCING) {
        set_state(c, SC2C_READY);
    }
}

static void on_ack(sc2c_t *c, const char *rid, const sc2_json_val_t *root)
{
    int i;
    if ((i = rid_index(rid, 'o', SC2C_OUTBOX_SLOTS)) >= 0) {
        sc2c_slot_t s = c->slots[i];
        if (!s.used) {
            return;
        }
        sc2_json_val_t v;
        uint64_t seq = 0;
        if (sc2_json_get(root, "seq", &v) == SC2_OK) {
            (void)sc2_json_u64_str(&v, &seq);
        }
        ob_drop(c, (unsigned)i);
        if (s.kind != OB_KIND_SHARE) {
            sc2c_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SC2C_EV_SENT;
            ev.kind = (sc2c_kind_t)s.kind;
            ev.conv = s.target;
            ev.token = s.token;
            ev.token_key = s.token_key;
            ev.seq = seq;
            emit(c, &ev);
        }
        /* a freed slot may let owed epoch keys out */
        shares_flush(c);
        return;
    }
    if ((i = rid_index(rid, 'j', SC2C_ROOMS_MAX)) >= 0) {
        sc2_json_val_t v;
        if (sc2_json_get(root, "status", &v) == SC2_OK && sc2_json_string_eq(&v, "requested")) {
            room_event(c, &c->rooms[i], SC2C_ROOM_REQUESTED, "waiting for an admin to add this device");
            admissions_flush(c);
        }
        return;
    }
    if ((i = rid_index(rid, 'c', SC2C_ROOMS_MAX)) >= 0) {
        sc2c_room_t *r = &c->rooms[i];
        if (r->commit_op == 0 || rec_load(c, r->key) != SC2_OK) {
            return;
        }
        uint8_t rec[SC2_COMMIT_LEN];
        uint8_t op = r->commit_op;
        sc2e_t *e = lock(c);
        int rc = commit_build(c, e, op, op == SC2_COMMIT_CREATE ? c->self_id : r->commit_target, rec, NULL);
        unlock(c);
        if (rc == SC2_OK) {
            rc = sc2_roster_apply(&c->rec.roster, rec);
        }
        if (rc == SC2_OK) {
            rc = rec_save(c, r->key);
        }
        r->commit_op = 0;
        if (rc != SC2_OK) {
            notice(c, rc, "acked commit could not be applied locally");
            return;
        }
        if (op == SC2_COMMIT_CREATE) {
            uint8_t ikey[SC2_INVITE_KEY_LEN];
            size_t n = 0;
            char irid[16];
            snprintf(irid, sizeof(irid), "i%d", i);
            rc = sc2_invite_key(ikey, c->rec.secret, c->rec.roster.room);
            if (rc == SC2_OK) {
                rc = sc2_frame_room_invite(c->tx, SC2C_TX_MAX, &n, irid, c->rec.roster.room, ikey, false);
            }
            sc2_memzero(ikey, sizeof(ikey));
            if (rc == SC2_OK) {
                (void)send_frame(c, c->tx, n);
            }
        } else {
            admin_catch_up(c, (unsigned)i);
            shares_flush_room(c, (unsigned)i);
            commit_next(c, (unsigned)i);
        }
        return;
    }
    if ((i = rid_index(rid, 'i', SC2C_ROOMS_MAX)) >= 0) {
        sc2c_room_t *r = &c->rooms[i];
        char text[SC2_INVITE_TEXT_MAX];
        if (sc2c_room_invite_text(c, r->key, text, sizeof(text)) == SC2_OK) {
            sc2c_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SC2C_EV_CREATED;
            ev.kind = SC2C_PRIVATE;
            ev.conv = r->key;
            ev.text = text;
            emit(c, &ev);
        }
        send_join(c, (unsigned)i);
        return;
    }
}

static bool code_retryable(uint64_t code)
{
    return code == 429 || code == 503 || code == 507 || code == 500;
}

/* Error frames the relay sends, without a rid, right before it closes a
 * session on purpose. 503 is also "server_full", which stays a failure, and
 * 409 also answers room commits. */
static sc2c_close_t close_reason(uint64_t code, const char *err)
{
    if (code == 408) {
        return SC2C_CLOSE_IDLE;
    }
    if (code == 503 && strcmp(err, "reconnect") == 0) {
        return SC2C_CLOSE_RECONNECT;
    }
    if (code == 503 && strcmp(err, "evicted") == 0) {
        return SC2C_CLOSE_EVICTED;
    }
    if (code == 409 && strcmp(err, "replaced") == 0) {
        return SC2C_CLOSE_REPLACED;
    }
    return SC2C_CLOSE_NONE;
}

static void on_error(sc2c_t *c, const char *rid, const sc2_json_val_t *root)
{
    uint64_t code = 0;
    char err[48] = "", text[128] = "";
    (void)sc2_json_get_u64(root, "code", 999, &code);
    (void)sc2_json_get_string(root, "err", err, sizeof(err), NULL);
    (void)sc2_json_get_string(root, "text", text, sizeof(text), NULL);
    const char *why = text[0] != '\0' ? text : (err[0] != '\0' ? err : "relay error");
    int i;
    /* Only an authenticated session can be closed this way; during login
     * any refusal is a failure and goes through the backoff. */
    sc2c_close_t closing = rid[0] == '\0' && c->state >= SC2C_SYNCING ? close_reason(code, err) : SC2C_CLOSE_NONE;
    if (closing != SC2C_CLOSE_NONE) {
        c->closed = (uint8_t)closing;
        if (closing == SC2C_CLOSE_REPLACED && c->replaced < 16u) {
            c->replaced++;
        }
        notice(c, (int)code, why);
        return;
    }
    if ((i = rid_index(rid, 'o', SC2C_OUTBOX_SLOTS)) >= 0 && c->slots[i].used) {
        sc2c_slot_t s = c->slots[i];
        if (code_retryable(code)) {
            c->slots[i].inflight = false;
            c->retry_ms = now_ms(c) + RETRY_MS;
            return;
        }
        ob_drop(c, (unsigned)i);
        if (s.kind != OB_KIND_SHARE) {
            sc2c_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SC2C_EV_FAILED;
            ev.kind = (sc2c_kind_t)s.kind;
            ev.conv = s.target;
            ev.token = s.token;
            ev.token_key = s.token_key;
            ev.code = (int)code;
            ev.text = why;
            emit(c, &ev);
        } else {
            notice(c, (int)code, why);
        }
        return;
    }
    if ((i = rid_index(rid, 'k', SC2C_CLAIMS_MAX)) >= 0) {
        /* no bundle: keep the entry (asked_ms) so retries are spaced out;
         * a 404 means the device is unknown to this relay */
        if (code == 404) {
            c->claims[i].asked_ms = UINT64_MAX; /* permanent for this session */
        }
        notice(c, (int)code, why);
        dm_resync(c);
        return;
    }
    if ((i = rid_index(rid, 'j', SC2C_ROOMS_MAX)) >= 0 || (i = rid_index(rid, 'c', SC2C_ROOMS_MAX)) >= 0 ||
        (i = rid_index(rid, 'i', SC2C_ROOMS_MAX)) >= 0 || (i = rid_index(rid, 'y', SC2C_ROOMS_MAX)) >= 0) {
        sc2c_room_t *r = &c->rooms[i];
        if (rid[0] == 'c') {
            r->commit_op = 0;
        }
        if (code_retryable(code)) {
            notice(c, (int)code, why);
            return;
        }
        if (rid[0] == 'y') {
            r->synced = false;
        }
        room_event(c, r, SC2C_ROOM_FAILED, why);
        return;
    }
    if (strcmp(rid, "a") == 0 || code == 401) {
        notice(c, (int)code, why);
        return;
    }
    notice(c, (int)code, why);
}

static void on_login(sc2c_t *c)
{
    c->inbox_done = 0;
    (void)send_keys(c);
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        sc2c_room_t *r = &c->rooms[i];
        r->synced = false;
        r->commit_op = 0;
        if (r->key[0] != '\0' && r->want && r->state != SC2C_ROOM_LEFT) {
            send_join(c, i);
        }
    }
    for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
        if (c->slots[i].used) {
            ob_transmit(c, i);
        }
    }
    send_sync(c, -1);
}

/* Pinned relay key: the welcome must be signed over our nonce by the key it
 * presents, and that key must be the pin, or a rotation from the pin signed
 * by it. A mismatch never changes the pin. */
static int check_relay_key(sc2c_t *c, const sc2_welcome_t *w)
{
    if (!w->has_relay_key || !w->has_relay_sig) {
        notice(c, SC2C_NOTICE_RELAY_KEY, "relay key pinned but the relay did not prove it (relay too old?)");
        return SC2_ERR_AUTH;
    }
    if (sc2_welcome_verify(w->relay_sig, w->relay_key, w->relay_id, c->hello_nonce, w->challenge) != SC2_OK) {
        notice(c, SC2C_NOTICE_RELAY_KEY, "relay key proof does not verify");
        return SC2_ERR_AUTH;
    }
    if (sc2_ct_eq(w->relay_key, c->relay_key, SC2_KEY_LEN)) {
        return SC2_OK;
    }
    if (w->has_rotation && sc2_ct_eq(w->rotation_from, c->relay_key, SC2_KEY_LEN) &&
        sc2_rotation_verify(w->rotation_sig, c->relay_key, w->relay_id, w->relay_key) == SC2_OK) {
        memcpy(c->relay_key, w->relay_key, SC2_KEY_LEN);
        sc2c_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SC2C_EV_RELAY_KEY;
        ev.text = "relay key rotated, signed by the pinned key";
        emit(c, &ev);
        return SC2_OK;
    }
    notice(c, SC2C_NOTICE_RELAY_KEY,
           "relay key differs from the pinned key; refusing (the pin is not changed: re-add the relay to accept it)");
    return SC2_ERR_AUTH;
}

static int on_welcome(sc2c_t *c, const char *line, size_t len)
{
    sc2_welcome_t w;
    uint8_t sig[SC2_SIG_LEN];
    size_t n = 0;
    int rc = sc2_frame_welcome_decode(line, len, &w);
    if (rc != SC2_OK) {
        return rc;
    }
    if (c->expect_relay[0] != '\0' && strcmp(c->expect_relay, w.relay_id) != 0) {
        notice(c, 0, "relay id differs from the configured one");
        return SC2_ERR_AUTH;
    }
    if (c->has_relay_key) {
        rc = check_relay_key(c, &w);
        if (rc != SC2_OK) {
            return rc;
        }
    }
    memcpy(c->relay_id, w.relay_id, sizeof(c->relay_id));
    sc2e_t *e = lock(c);
    rc = sc2_auth_sign(sig, &e->id, w.relay_id, strlen(w.relay_id), w.challenge);
    uint8_t ik[SC2_KEY_LEN];
    memcpy(ik, e->id.ik_pub, sizeof(ik));
    unlock(c);
    if (rc == SC2_OK) {
        rc = sc2_frame_auth(c->tx, SC2C_TX_MAX, &n, "a", ik, sig, false);
    }
    if (rc == SC2_OK) {
        rc = send_frame(c, c->tx, n);
    }
    if (rc == SC2_OK) {
        set_state(c, SC2C_AUTH);
    }
    return rc;
}

static int process_line(sc2c_t *c, const char *line, size_t len)
{
    sc2_json_val_t root;
    char type[SC2_TYPE_MAX + 1], rid[SC2_RID_MAX + 1];
    if (sc2_frame_peek(line, len, &root, type, rid) != SC2_OK) {
        notice(c, 0, "unparsable frame from the relay ignored");
        return SC2_OK;
    }
    bool replay = c->state != SC2C_READY;
    if (strcmp(type, "welcome") == 0) {
        return c->state == SC2C_HELLO ? on_welcome(c, line, len) : SC2_OK;
    }
    if (strcmp(type, "error") == 0) {
        uint64_t code = 0;
        (void)sc2_json_get_u64(&root, "code", 999, &code);
        on_error(c, rid, &root);
        if (c->state < SC2C_SYNCING && (code == 401 || code == 403)) {
            return SC2_ERR_AUTH; /* refused at login: drop and back off */
        }
        return SC2_OK;
    }
    if (strcmp(type, "authed") == 0) {
        if (c->state == SC2C_AUTH) {
            set_state(c, SC2C_SYNCING);
            on_login(c);
        }
        return SC2_OK;
    }
    if (c->state < SC2C_SYNCING) {
        return SC2_OK;
    }
    if (strcmp(type, "dm.msg") == 0) {
        on_dm(c, line, len, replay);
    } else if (strcmp(type, "room.msg") == 0) {
        on_room_msg(c, line, len, replay);
    } else if (strcmp(type, "room.pub") == 0) {
        on_room_pub(c, line, len, replay);
    } else if (strcmp(type, "room.commit") == 0) {
        on_commit(c, line, len);
    } else if (strcmp(type, "room.event") == 0) {
        on_room_event(c, &root);
    } else if (strcmp(type, "room.state") == 0) {
        on_room_state(c, &root);
    } else if (strcmp(type, "ack") == 0) {
        on_ack(c, rid, &root);
    } else if (strcmp(type, "sync.done") == 0) {
        on_sync_done(c, &root);
    } else if (strcmp(type, "sync.gap") == 0) {
        notice(c, 0, "older room history expired on the relay");
    } else if (strcmp(type, "keys.bundle") == 0) {
        char dev[SC2_DEVICE_ID_HEX_LEN + 1] = "";
        sc2e_t *e = lock(c);
        int rc = sc2e_add_bundle(e, line, len, dev);
        unlock(c);
        int i = rid_index(rid, 'k', SC2C_CLAIMS_MAX);
        if (i >= 0) {
            memset(&c->claims[i], 0, sizeof(c->claims[i]));
        }
        if (rc != SC2_OK) {
            notice(c, rc, "peer key bundle rejected");
        }
        shares_flush(c);
        dm_resync(c);
        admissions_flush(c);
        if (rc == SC2_OK) {
            for (unsigned j = 0; j < SC2C_ROOMS_MAX; j++) c->rooms[j].commit_hold = 0;
            send_sync(c, -1);
        }
    } else if (strcmp(type, "keys.low") == 0) {
        (void)send_keys(c);
    }
    /* pong, presence, room.mod and unknown types need no action */
    return SC2_OK;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

int sc2c_init(sc2c_t *c, const sc2c_host_t *host, const sc2c_config_t *cfg)
{
    if (c == NULL || host == NULL || cfg == NULL || host->send == NULL || host->engine_lock == NULL ||
        host->engine_unlock == NULL || host->alloc == NULL || host->release == NULL || cfg->tag == NULL) {
        return SC2_ERR_ARG;
    }
    size_t tl = strlen(cfg->tag);
    if (tl == 0 || tl > SC2C_TAG_MAX) {
        return SC2_ERR_ARG;
    }
    for (size_t i = 0; i < tl; i++) {
        char ch = cfg->tag[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
            return SC2_ERR_ARG;
        }
    }
    memset(c, 0, sizeof(*c));
    c->host = *host;
    memcpy(c->tag, cfg->tag, tl);
    snprintf(c->nick, sizeof(c->nick), "%s", cfg->nick != NULL && cfg->nick[0] != '\0' ? cfg->nick : "SolarOS");
    if (cfg->admit != NULL) {
        snprintf(c->admit, sizeof(c->admit), "%s", cfg->admit);
    }
    if (cfg->expect_relay != NULL) {
        snprintf(c->expect_relay, sizeof(c->expect_relay), "%s", cfg->expect_relay);
    }
    if (cfg->invite_url != NULL) {
        snprintf(c->invite_url, sizeof(c->invite_url), "%s", cfg->invite_url);
    }
    c->auto_admit = cfg->auto_admit;
    c->no_opks = cfg->no_opks;
    if (cfg->relay_key != NULL) {
        memcpy(c->relay_key, cfg->relay_key, SC2_KEY_LEN);
        c->has_relay_key = true;
    }
    c->pub_max = 1024;
    c->rx = host->alloc(host->ctx, sizeof(sc2_lines_t));
    c->tx = host->alloc(host->ctx, SC2C_TX_MAX);
    c->work = host->alloc(host->ctx, SC2C_WORK_MAX);
    if (c->rx == NULL || c->tx == NULL || c->work == NULL) {
        sc2c_free(c);
        return SC2_ERR_BOUNDS;
    }
    sc2_lines_init(c->rx);
    sc2e_t *e = lock(c);
    memcpy(c->self_id, e->self_id, sizeof(c->self_id));
    memcpy(c->self_hex, e->self_hex, sizeof(c->self_hex));
    unlock(c);
    /* outbox index: header of every persisted slot */
    for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
        size_t fl = 0;
        int rc = ob_load(c, i, &fl);
        if (rc == SC2_ERR_NOT_FOUND) {
            continue;
        }
        if (rc != SC2_OK) {
            ob_drop(c, i);
            continue;
        }
        const uint8_t *h = (const uint8_t *)c->tx + 12u;
        sc2c_slot_t *s = &c->slots[i];
        s->used = true;
        s->kind = h[5];
        s->token = get_u32(h + 6);
        s->token_key = ((uint64_t)get_u32(h + 10) << 32) | get_u32(h + 14);
        memcpy(s->target, h + 18, SC2C_KEY_MAX);
        s->target[SC2C_KEY_MAX] = '\0';
    }
    sc2_memzero(c->tx, SC2C_TX_MAX);
    return SC2_OK;
}

void sc2c_free(sc2c_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->rx != NULL) {
        c->host.release(c->host.ctx, c->rx, sizeof(sc2_lines_t));
    }
    if (c->tx != NULL) {
        c->host.release(c->host.ctx, c->tx, SC2C_TX_MAX);
    }
    if (c->work != NULL) {
        c->host.release(c->host.ctx, c->work, SC2C_WORK_MAX);
    }
    c->rx = NULL;
    c->tx = NULL;
    c->work = NULL;
    sc2_memzero(&c->rec, sizeof(c->rec));
}

int sc2c_on_connected(sc2c_t *c)
{
    size_t n = 0;
    sc2_lines_init(c->rx);
    c->closed = SC2C_CLOSE_NONE;
    c->state = SC2C_HELLO; /* set directly: send_frame refuses OFFLINE */
    c->last_rx_ms = now_ms(c);
    /* A fresh nonce on every connection: the relay signs it with its key. */
    sc2e_t *e = lock(c);
    int rc = sc2_random(&e->pf.rng, c->hello_nonce, sizeof(c->hello_nonce));
    unlock(c);
    if (rc == SC2_OK) {
        rc = sc2_frame_hello(c->tx, SC2C_TX_MAX, &n, "solaros-sc2-sync/1", c->admit[0] != '\0' ? c->admit : NULL,
                             c->hello_nonce);
    }
    if (rc == SC2_OK) {
        rc = send_frame(c, c->tx, n);
    }
    if (rc != SC2_OK) {
        c->state = SC2C_OFFLINE;
        return rc;
    }
    c->state = SC2C_OFFLINE;
    set_state(c, SC2C_HELLO);
    return SC2_OK;
}

void sc2c_on_disconnected(sc2c_t *c)
{
    for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
        c->slots[i].inflight = false;
    }
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        c->rooms[i].synced = false;
        c->rooms[i].commit_op = 0;
        if (c->rooms[i].state == SC2C_ROOM_JOINING || c->rooms[i].state == SC2C_ROOM_CREATING) {
            c->rooms[i].state = SC2C_ROOM_IDLE;
        }
    }
    memset(c->claims, 0, sizeof(c->claims));
    c->dm_hold = 0;
    if (c->closed != SC2C_CLOSE_REPLACED) {
        c->replaced = 0; /* the streak ends with any other kind of close */
    }
    sc2_lines_init(c->rx);
    set_state(c, SC2C_OFFLINE);
}

int sc2c_feed(sc2c_t *c, const uint8_t *data, size_t len)
{
    c->last_rx_ms = now_ms(c);
    while (len > 0) {
        size_t used = 0;
        const char *line = NULL;
        size_t ll = 0;
        int rc = sc2_lines_feed(c->rx, data, len, &used, &line, &ll);
        data += used;
        len -= used;
        if (rc == SC2_ERR_BOUNDS) {
            notice(c, 413, "oversized line from the relay dropped");
            continue;
        }
        if (rc < 0) {
            return rc;
        }
        if (rc == 1) {
            rc = process_line(c, line, ll);
            if (rc != SC2_OK) {
                return rc;
            }
            if (c->state == SC2C_OFFLINE) {
                return SC2_ERR_STATE;
            }
        }
        if (used == 0 && rc == 0) {
            break;
        }
    }
    return SC2_OK;
}

int sc2c_tick(sc2c_t *c)
{
    if (c->state == SC2C_OFFLINE) {
        return SC2_OK;
    }
    uint64_t t = now_ms(c);
    if (t - c->last_rx_ms > SC2C_DEAD_MS) {
        return SC2_ERR_STATE;
    }
    if (t - c->last_tx_ms > SC2C_PING_MS && t - c->last_rx_ms > SC2C_PING_MS / 2) {
        (void)send_frame(c, "{\"type\":\"ping\",\"rid\":\"p\"}", 25);
    }
    if (c->dm_hold != 0 && t - c->dm_hold_ms > CLAIM_RETRY_MS) {
        dm_resync(c); /* claim answer lost or slow: try the held item again */
    }
    if (c->state >= SC2C_SYNCING && c->retry_ms != 0 && t >= c->retry_ms) {
        c->retry_ms = 0;
        for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
            if (c->slots[i].used && !c->slots[i].inflight) {
                ob_transmit(c, i);
            }
        }
        shares_flush(c);
    }
    admissions_flush(c);
    return SC2_OK;
}

bool sc2c_ready(const sc2c_t *c)
{
    return c->state == SC2C_READY;
}

static int room_slot_new(sc2c_t *c, const char *key, sc2c_kind_t kind, uint64_t cursor)
{
    int idx = room_index(c, key);
    if (idx >= 0) {
        c->rooms[idx].want = true;
        if (cursor > c->rooms[idx].cursor) {
            c->rooms[idx].cursor = cursor;
        }
        return idx;
    }
    for (unsigned i = 0; i < SC2C_ROOMS_MAX; i++) {
        sc2c_room_t *r = &c->rooms[i];
        if (r->key[0] == '\0') {
            memset(r, 0, sizeof(*r));
            snprintf(r->key, sizeof(r->key), "%s", key);
            r->kind = (uint8_t)kind;
            r->want = true;
            r->cursor = cursor;
            return (int)i;
        }
    }
    return SC2_ERR_BOUNDS;
}

int sc2c_room_add(sc2c_t *c, const char *key, uint64_t cursor)
{
    sc2c_kind_t kind;
    if (public_key_ok(key)) {
        kind = SC2C_PUBLIC;
    } else if (hex32_ok(key)) {
        kind = SC2C_PRIVATE;
    } else {
        return SC2_ERR_ARG;
    }
    int had = room_index(c, key);
    int idx = room_slot_new(c, key, kind, cursor);
    if (idx < 0) {
        return idx;
    }
    /* A configured room survives removal in the profile. Its authenticated
     * roster is the durable authority: only an explicit invite join resets it. */
    if (kind == SC2C_PRIVATE && rec_load(c, key) == SC2_OK && c->rec.roster.count &&
        sc2_roster_find(&c->rec.roster, c->self_id) < 0) {
        c->rooms[idx].want = false;
        room_event(c, &c->rooms[idx], SC2C_ROOM_LEFT, "removed by an admin");
        return SC2_OK;
    }
    if (c->state >= SC2C_SYNCING && (had < 0 || c->rooms[idx].state == SC2C_ROOM_IDLE ||
                                     c->rooms[idx].state == SC2C_ROOM_FAILED || c->rooms[idx].state == SC2C_ROOM_LEFT)) {
        send_join(c, (unsigned)idx);
    }
    return SC2_OK;
}

int sc2c_room_remove(sc2c_t *c, const char *key)
{
    int idx = room_index(c, key);
    if (idx < 0) {
        return SC2_ERR_NOT_FOUND;
    }
    char rid[16];
    snprintf(rid, sizeof(rid), "l%d", idx);
    if (c->state >= SC2C_SYNCING) {
        (void)send_simple(c, "{\"type\":\"room.leave\",\"rid\":\"%s\",\"room\":\"%s\"}", rid, key);
    }
    if (c->rooms[idx].kind == SC2C_PRIVATE) {
        rec_remove(c, key);
    }
    memset(&c->rooms[idx], 0, sizeof(c->rooms[idx]));
    return SC2_OK;
}

const sc2c_room_t *sc2c_room_get(const sc2c_t *c, const char *key)
{
    int idx = room_index(c, key);
    return idx >= 0 ? &c->rooms[idx] : NULL;
}

int sc2c_room_create(sc2c_t *c, char room_out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    return sc2c_room_create_named(c, NULL, room_out);
}

int sc2c_room_get_title(sc2c_t *c, const char *room, char *out, size_t cap)
{
    if (!out || !cap) return SC2_ERR_ARG;
    out[0] = 0;
    int rc = rec_load(c, room);
    if (rc == SC2_OK) {
        size_t len = strlen(c->rec.title);
        if (len >= cap) return SC2_ERR_BOUNDS;
        memcpy(out, c->rec.title, len + 1u);
    }
    return rc;
}

int sc2c_room_create_named(sc2c_t *c, const char *title, char room_out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    if (title && !sc2e_label_valid(title, SC2E_ROOM_TITLE_MAX)) return SC2_ERR_ARG;
    if (c->state != SC2C_READY) {
        return SC2C_AGAIN;
    }
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    uint32_t epoch = 0;
    sc2e_t *e = lock(c);
    int rc = sc2e_room_create(e, NULL, room, &epoch);
    if (rc == SC2_OK) {
        memset(&c->rec, 0, sizeof(c->rec));
        c->rec.flags = SC2C_RF_ADMIN | SC2C_RF_INVITE;
        if (title) memcpy(c->rec.title, title, strlen(title) + 1u);
        rc = sc2_random(&e->pf.rng, c->rec.secret, sizeof(c->rec.secret));
        memcpy(c->rec.admin_id, e->self_id, SC2_DEVICE_ID_LEN);
        memcpy(c->rec.admin_ik, e->id.ik_pub, SC2_KEY_LEN);
    }
    unlock(c);
    uint8_t rid_room[SC2_ROOM_ID_LEN];
    if (rc == SC2_OK) {
        rc = sc2_hex_decode(rid_room, sizeof(rid_room), room, SC2_DEVICE_ID_HEX_LEN);
    }
    if (rc == SC2_OK) {
        sc2_roster_init(&c->rec.roster, rid_room);
        rc = rec_save(c, room);
    }
    int idx = rc == SC2_OK ? room_slot_new(c, room, SC2C_PRIVATE, 0) : rc;
    if (idx < 0) {
        return idx;
    }
    uint8_t rec[SC2_COMMIT_LEN], sig[SC2_SIG_LEN];
    size_t n = 0;
    char rid[16];
    snprintf(rid, sizeof(rid), "c%d", idx);
    e = lock(c);
    rc = commit_build(c, e, SC2_COMMIT_CREATE, c->self_id, rec, sig);
    unlock(c);
    if (rc == SC2_OK) {
        rc = sc2_frame_room_create_private(c->tx, SC2C_TX_MAX, &n, rid, rid_room, rec, sig);
    }
    if (rc == SC2_OK) {
        rc = send_frame(c, c->tx, n);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    c->rooms[idx].commit_op = SC2_COMMIT_CREATE;
    room_event(c, &c->rooms[idx], SC2C_ROOM_CREATING, NULL);
    if (room_out != NULL) {
        memcpy(room_out, room, sizeof(room));
    }
    return SC2_OK;
}

int sc2c_room_invite_text(sc2c_t *c, const char *room, char *out, size_t cap)
{
    if (rec_load(c, room) != SC2_OK || (c->rec.flags & SC2C_RF_ADMIN) == 0) {
        return SC2_ERR_NOT_FOUND;
    }
    sc2_invite_t inv;
    memset(&inv, 0, sizeof(inv));
    memcpy(inv.room, c->rec.roster.room, sizeof(inv.room));
    memcpy(inv.secret, c->rec.secret, sizeof(inv.secret));
    memcpy(inv.admin_id, c->rec.admin_id, sizeof(inv.admin_id));
    memcpy(inv.admin_ik, c->rec.admin_ik, sizeof(inv.admin_ik));
    snprintf(inv.url, sizeof(inv.url), "%s", c->invite_url[0] != '\0' ? c->invite_url : "chat://localhost");
    inv.url_len = strlen(inv.url);
    size_t n = 0;
    int rc = sc2_invite_to_text(out, cap, &n, &inv);
    sc2_memzero(&inv, sizeof(inv));
    return rc;
}

int sc2c_room_join_invite(sc2c_t *c, const char *text, char room_out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    sc2_invite_t inv;
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    int rc = sc2_invite_from_text(&inv, text, strlen(text));
    if (rc != SC2_OK) {
        return rc;
    }
    hex_of(room, inv.room);
    if (rec_load(c, room) == SC2_OK && (c->rec.flags & SC2C_RF_ADMIN) != 0) {
        sc2_memzero(&inv, sizeof(inv));
        return sc2c_room_add(c, room, 0); /* our own room */
    }
    memset(&c->rec, 0, sizeof(c->rec));
    c->rec.flags = SC2C_RF_INVITE;
    memcpy(c->rec.secret, inv.secret, sizeof(inv.secret));
    memcpy(c->rec.admin_id, inv.admin_id, sizeof(inv.admin_id));
    memcpy(c->rec.admin_ik, inv.admin_ik, sizeof(inv.admin_ik));
    sc2_roster_init(&c->rec.roster, inv.room);
    sc2_memzero(&inv, sizeof(inv));
    rc = rec_save(c, room);
    if (rc != SC2_OK) {
        return rc;
    }
    if (room_out != NULL) {
        memcpy(room_out, room, sizeof(room));
    }
    return sc2c_room_add(c, room, 0);
}

static int ob_find_key(const sc2c_t *c, uint64_t key)
{
    if (key == 0) {
        return -1;
    }
    for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
        if (c->slots[i].used && c->slots[i].kind != OB_KIND_SHARE && c->slots[i].token_key == key) {
            return (int)i;
        }
    }
    return -1;
}

static int build_inner(sc2c_t *c, const char *body, size_t body_len, size_t *out_len)
{
    uint8_t id[SC2_MSG_ID_LEN];
    sc2e_t *e = lock(c);
    int rc = sc2_random(&e->pf.rng, id, sizeof(id));
    unlock(c);
    if (rc != SC2_OK) {
        return rc;
    }
    sc2_jw_t w;
    sc2_jw_init(&w, c->work, SC2C_WORK_MAX);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "t");
    sc2_jw_cstring(&w, "text");
    sc2_jw_key(&w, "id");
    sc2_jw_hex(&w, id, sizeof(id));
    if (sc2e_label_valid(c->nick, SC2C_NICK_MAX)) {
        sc2_jw_key(&w, "name");
        sc2_jw_cstring(&w, c->nick);
    }
    sc2_jw_key(&w, "body");
    sc2_jw_string(&w, body, body_len);
    uint64_t ts = c->host.wall_s != NULL ? c->host.wall_s(c->host.ctx) : 0;
    if (ts != 0) {
        sc2_jw_key(&w, "ts");
        sc2_jw_u64(&w, ts);
    }
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2c_submit(sc2c_t *c, sc2c_kind_t kind, const char *target, const char *body, size_t body_len, uint32_t token,
                uint64_t token_key)
{
    if (target == NULL || body == NULL) {
        return SC2_ERR_ARG;
    }
    int dup = ob_find_key(c, token_key);
    if (dup >= 0) {
        c->slots[dup].token = token; /* the caller's request id may have changed after a reboot */
        return SC2_OK;
    }
    if (c->state != SC2C_READY) {
        return SC2C_AGAIN;
    }
    int slot = ob_free_slot(c);
    if (slot < 0) {
        return SC2C_AGAIN;
    }
    if (!sc2_utf8_valid(body, body_len)) {
        return SC2_ERR_FORMAT;
    }
    char rid[16];
    snprintf(rid, sizeof(rid), "o%d", slot);
    size_t n = 0, il = 0;
    int rc;
    sc2e_t *e;
    if (kind == SC2C_DM) {
        if (!hex32_ok(target)) {
            return SC2_ERR_ARG;
        }
        if (strcmp(target, c->self_hex) == 0) {
            return SC2_ERR_ARG;
        }
        sc2e_peer_info_t pi;
        e = lock(c);
        rc = sc2e_peer_info(e, target, &pi);
        unlock(c);
        if (rc != SC2_OK) {
            return rc;
        }
        if (!pi.known && !pi.has_session) {
            for (unsigned i = 0; i < SC2C_CLAIMS_MAX; i++) {
                if (strcmp(c->claims[i].device, target) == 0 && c->claims[i].asked_ms == UINT64_MAX) {
                    return SC2_ERR_NOT_FOUND; /* relay has no keys for this device */
                }
            }
            rc = send_claim(c, target);
            return rc == SC2_OK ? SC2C_AGAIN : rc;
        }
        rc = build_inner(c, body, body_len, &il);
        if (rc == SC2_OK) {
            e = lock(c);
            rc = sc2e_dm_encrypt(e, target, rid, c->work, il, ob_frame(c), SC2C_TX_MAX - OB_HDR, &n);
            unlock(c);
        }
    } else if (kind == SC2C_PRIVATE) {
        int idx = room_index(c, target);
        if (idx < 0 || c->rooms[idx].kind != SC2C_PRIVATE) {
            return SC2_ERR_NOT_FOUND;
        }
        uint8_t st = c->rooms[idx].state;
        if (st != SC2C_ROOM_JOINED) {
            return st == SC2C_ROOM_FAILED || st == SC2C_ROOM_LEFT ? SC2_ERR_STATE : SC2C_AGAIN;
        }
        rc = build_inner(c, body, body_len, &il);
        if (rc == SC2_OK) {
            e = lock(c);
            rc = sc2e_room_encrypt(e, target, rid, c->work, il, ob_frame(c), SC2C_TX_MAX - OB_HDR, &n);
            unlock(c);
        }
    } else if (kind == SC2C_PUBLIC) {
        int idx = room_index(c, target);
        if (idx < 0 || c->rooms[idx].kind != SC2C_PUBLIC) {
            return SC2_ERR_NOT_FOUND;
        }
        uint8_t st = c->rooms[idx].state;
        if (st != SC2C_ROOM_JOINED) {
            return st == SC2C_ROOM_FAILED ? SC2_ERR_STATE : SC2C_AGAIN;
        }
        if (body_len > c->pub_max) {
            return SC2_ERR_BOUNDS;
        }
        uint64_t ts = c->host.wall_s != NULL ? c->host.wall_s(c->host.ctx) : 0;
        e = lock(c);
        rc = sc2e_post_sign(e, c->relay_id, target, c->nick, body, body_len, ts, NULL, false, rid, ob_frame(c),
                            SC2C_TX_MAX - OB_HDR, &n);
        unlock(c);
    } else {
        return SC2_ERR_ARG;
    }
    sc2_memzero(c->work, il);
    if (rc != SC2_OK) {
        return rc;
    }
    rc = ob_save(c, (unsigned)slot, (uint8_t)kind, token, token_key, target, n);
    if (rc != SC2_OK) {
        return rc;
    }
    if (send_frame(c, ob_frame(c), n) == SC2_OK) {
        c->slots[slot].inflight = true;
    }
    return SC2_OK;
}

size_t sc2c_outbox_count(const sc2c_t *c)
{
    size_t n = 0;
    for (unsigned i = 0; i < SC2C_OUTBOX_SLOTS; i++) {
        n += c->slots[i].used ? 1u : 0u;
    }
    return n;
}

uint32_t sc2c_backoff_ms(uint32_t *attempt, uint32_t random)
{
    uint32_t a = *attempt < 6 ? *attempt : 6; /* 1, 2, 4, 8, 16, 32, 60 s */
    uint32_t base = a >= 6 ? 60000u : (1000u << a);
    if (*attempt < 1000) {
        (*attempt)++;
    }
    uint32_t jitter = base / 4u;
    return base - jitter + (jitter != 0 ? random % (2u * jitter + 1u) : 0u);
}

sc2c_close_t sc2c_close_reason(const sc2c_t *c)
{
    return (sc2c_close_t)c->closed;
}

bool sc2c_close_soft(sc2c_close_t why)
{
    return why == SC2C_CLOSE_IDLE || why == SC2C_CLOSE_RECONNECT || why == SC2C_CLOSE_EVICTED;
}

const char *sc2c_close_name(sc2c_close_t why)
{
    switch (why) {
    case SC2C_CLOSE_IDLE:
        return "relay closed the idle session";
    case SC2C_CLOSE_RECONNECT:
        return "relay session lifetime reached";
    case SC2C_CLOSE_EVICTED:
        return "relay full, idle session closed";
    case SC2C_CLOSE_REPLACED:
        return "replaced by another login of this device";
    default:
        return "none";
    }
}

uint32_t sc2c_reconnect_ms(const sc2c_t *c, uint32_t *attempt, uint32_t random)
{
    sc2c_close_t why = sc2c_close_reason(c);
    if (sc2c_close_soft(why)) {
        return SC2C_SOFT_RECONNECT_MS + random % 2001u;
    }
    if (why == SC2C_CLOSE_REPLACED && *attempt < (uint32_t)c->replaced + 1u) {
        *attempt = (uint32_t)c->replaced + 1u; /* 4 s, 8 s, ... up to 60 s */
    }
    return sc2c_backoff_ms(attempt, random);
}

const char *sc2c_state_name(int state)
{
    switch (state) {
    case SC2C_OFFLINE:
        return "offline";
    case SC2C_HELLO:
        return "hello";
    case SC2C_AUTH:
        return "auth";
    case SC2C_SYNCING:
        return "syncing";
    case SC2C_READY:
        return "ready";
    default:
        return "?";
    }
}
