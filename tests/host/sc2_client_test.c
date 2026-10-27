/*
 * Host tests for the device-side relay client (src/services/solar_os_sc2_client.c)
 * against the portable mini-relay (protocol/sc2 sc2_minirelay), both in
 * process: how the client treats a session the relay closes on purpose.
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2_minirelay.h"
#include "sc2_test_util.h"
#include "solar_os_sc2_client.h"
#include "solar_os_sc2_engine.h"
#include "solar_os_sc2_selftest.h"

/* ---------------- clock, memory ---------------- */

static uint64_t g_ms = 1000u;
static uint64_t g_s = 1700000000u;

static void advance_s(uint64_t s)
{
    g_ms += s * 1000u;
    g_s += s;
}

static void *t_alloc(void *ctx, size_t len)
{
    (void)ctx;
    return calloc(1, len);
}

static void t_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    if (ptr != NULL) {
        sc2_memzero(ptr, len);
        free(ptr);
    }
}

static uint64_t t_clock_us(void *ctx)
{
    (void)ctx;
    return g_ms * 1000u;
}

/* ---------------- line queues (no re-entry between the two sides) ---------------- */

typedef struct line {
    struct line *next;
    size_t len;
    char s[];
} line_t;

typedef struct {
    line_t *head, *tail;
} queue_t;

static void q_push(queue_t *q, const char *s, size_t len)
{
    line_t *l = malloc(sizeof(*l) + len + 1u);
    if (l == NULL) {
        exit(2);
    }
    l->next = NULL;
    l->len = len;
    memcpy(l->s, s, len);
    l->s[len] = '\0';
    if (q->tail != NULL) {
        q->tail->next = l;
    } else {
        q->head = l;
    }
    q->tail = l;
}

static line_t *q_pop(queue_t *q)
{
    line_t *l = q->head;
    if (l != NULL) {
        q->head = l->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
    }
    return l;
}

static void q_clear(queue_t *q)
{
    line_t *l;
    while ((l = q_pop(q)) != NULL) {
        free(l);
    }
}

/* ---------------- mini-relay ---------------- */

#define NSESS 4u
static sc2_mr_t *mr;
static queue_t to_client[NSESS];
static bool closing[NSESS]; /* kicked by the relay: close once its last frame is delivered */

static int mr_send(void *ctx, unsigned s, const char *line, size_t len)
{
    (void)ctx;
    q_push(&to_client[s], line, len);
    return 0;
}

static uint64_t mr_now(void *ctx)
{
    (void)ctx;
    return g_s;
}

static void mr_kick(void *ctx, unsigned s)
{
    (void)ctx;
    closing[s] = true;
}

/* ---------------- devices ---------------- */

typedef struct {
    const char *name;
    sc2e_memstore_t *store;
    sc2e_t e;
    sc2e_t *engine; /* own engine, or another device's (same identity) */
    sc2c_t c;
    int sess;       /* mini-relay session, -1 offline */
    queue_t to_relay;
    int notices, last_notice;
    char sender_name[SC2C_NICK_MAX + 1];
    char sender_id[SC2_DEVICE_ID_HEX_LEN + 1];
} tdev_t;

static int d_send(void *ctx, const char *line, size_t len)
{
    tdev_t *d = ctx;
    if (d->sess < 0) {
        return -1;
    }
    q_push(&d->to_relay, line, len);
    return 0;
}

static void d_event(void *ctx, const sc2c_event_t *ev)
{
    tdev_t *d = ctx;
    if (ev->type == SC2C_EV_NOTICE) {
        d->notices++;
        d->last_notice = ev->code;
    }
    if (ev->type == SC2C_EV_MESSAGE) {
        snprintf(d->sender_name, sizeof(d->sender_name), "%s", ev->name);
        snprintf(d->sender_id, sizeof(d->sender_id), "%s", ev->from);
    }
}

static sc2e_t *d_lock(void *ctx)
{
    return ((tdev_t *)ctx)->engine;
}

static void d_unlock(void *ctx)
{
    (void)ctx;
}

static uint64_t d_mono(void *ctx)
{
    (void)ctx;
    return g_ms;
}

static uint64_t d_wall(void *ctx)
{
    (void)ctx;
    return g_s;
}

static const sc2e_test_host_t TH = {t_alloc, t_release, t_clock_us, {sc2t_xs_fn, NULL}, NULL};

static void d_init(tdev_t *d, const char *name, const char *tag, tdev_t *same_identity)
{
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->sess = -1;
    if (same_identity != NULL) {
        d->engine = same_identity->engine;
    } else {
        sc2e_platform_t pf;
        CHECK_RC(sc2e_memstore_create(&d->store, &TH), SC2_OK);
        sc2e_memstore_platform(d->store, &pf);
        CHECK_RC(sc2e_open(&d->e, &pf), SC2_OK);
        d->engine = &d->e;
    }
    sc2c_host_t h = {d_send, d_event, d_lock, d_unlock, t_alloc, t_release, d_mono, d_wall, d};
    sc2c_config_t cfg = {.tag = tag, .nick = name, .expect_relay = "mr.test"};
    CHECK_RC(sc2c_init(&d->c, &h, &cfg), SC2_OK);
}

static void d_free(tdev_t *d)
{
    sc2c_free(&d->c);
    q_clear(&d->to_relay);
    if (d->store != NULL) {
        sc2e_close(&d->e);
        sc2e_memstore_destroy(d->store);
    }
}

static tdev_t *devs[4];
static unsigned ndevs;

static tdev_t *dev_on(unsigned s)
{
    for (unsigned i = 0; i < ndevs; i++) {
        if (devs[i]->sess == (int)s) {
            return devs[i];
        }
    }
    return NULL;
}

/* The host side of a closed connection, as sc2-sync and chatd do it. */
static void disconnect(tdev_t *d)
{
    if (d->sess < 0) {
        return;
    }
    unsigned s = (unsigned)d->sess;
    sc2_mr_close(mr, s);
    closing[s] = false;
    q_clear(&to_client[s]);
    q_clear(&d->to_relay);
    d->sess = -1;
    sc2c_on_disconnected(&d->c);
}

/* Deliver queued lines both ways until nothing moves. */
static void pump(void)
{
    for (int round = 0; round < 10000; round++) {
        bool moved = false;
        for (unsigned i = 0; i < ndevs; i++) {
            tdev_t *d = devs[i];
            line_t *l;
            while (d->sess >= 0 && (l = q_pop(&d->to_relay)) != NULL) {
                int rc = sc2_mr_line(mr, (unsigned)d->sess, l->s, l->len);
                free(l);
                moved = true;
                if (rc == SC2_MR_CLOSE) {
                    disconnect(d);
                }
            }
        }
        for (unsigned s = 0; s < NSESS; s++) {
            line_t *l;
            while ((l = q_pop(&to_client[s])) != NULL) {
                tdev_t *d = dev_on(s);
                moved = true;
                if (d != NULL) {
                    char *nl = malloc(l->len + 1u);
                    memcpy(nl, l->s, l->len);
                    nl[l->len] = '\n';
                    if (sc2c_feed(&d->c, (const uint8_t *)nl, l->len + 1u) != SC2_OK) {
                        disconnect(d);
                    }
                    free(nl);
                }
                free(l);
            }
            tdev_t *d = dev_on(s);
            if (closing[s] && d != NULL) {
                disconnect(d);
                moved = true;
            }
        }
        if (!moved) {
            return;
        }
    }
    fprintf(stderr, "pump: no quiet point\n");
    exit(2);
}

static void connect_to(tdev_t *d, unsigned s)
{
    sc2_mr_open(mr, s);
    d->sess = (int)s;
    CHECK_RC(sc2c_on_connected(&d->c), SC2_OK);
    pump();
}

static void feed_line(tdev_t *d, const char *line)
{
    char buf[SC2E_FRAME_MAX + 1u];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    CHECK_RC(sc2c_feed(&d->c, (const uint8_t *)buf, (size_t)n), SC2_OK);
}

static void bundle_to(tdev_t *owner, tdev_t *to)
{
    char publish[SC2E_FRAME_MAX], bundle[SC2E_FRAME_MAX], device[SC2_DEVICE_ID_HEX_LEN + 1];
    size_t n, m;
    sc2e_identity_info_t identity;
    CHECK_RC(sc2e_identity(owner->engine, &identity), SC2_OK);
    CHECK_RC(sc2e_keys_publish(owner->engine, NULL, 0, publish, sizeof(publish), &n), SC2_OK);
    CHECK_RC(sc2e_relay_bundle(publish, n, &identity, bundle, sizeof(bundle), &m), SC2_OK);
    CHECK_RC(sc2e_add_bundle(to->engine, bundle, m, device), SC2_OK);
}

static line_t *take_type(tdev_t *d, const char *want)
{
    line_t *l;
    while ((l = q_pop(&d->to_relay)) != NULL) {
        sc2_json_val_t root;
        char type[SC2_TYPE_MAX + 1], rid[SC2_RID_MAX + 1];
        if (sc2_frame_peek(l->s, l->len, &root, type, rid) == SC2_OK && !strcmp(type, want)) return l;
        free(l);
    }
    return NULL;
}

static uint64_t inbox_seq;
static void deliver_dm(tdev_t *to, line_t *dm)
{
    CHECK(dm != NULL);
    if (!dm) return;
    char frame[SC2E_FRAME_MAX];
    size_t n;
    CHECK_RC(sc2e_relay_dm(dm->s, dm->len, ++inbox_seq, frame, sizeof(frame), &n), SC2_OK);
    feed_line(to, frame);
    free(dm);
}

static sc2_roster_t room_roster(tdev_t *d, const char *room)
{
    char name[SC2E_NAME_MAX];
    uint8_t record[sizeof(sc2c_roomrec_t) + SC2E_RECORD_OVERHEAD];
    const uint8_t *payload;
    size_t len, payload_len;
    sc2c_roomrec_t rec;
    snprintf(name, sizeof(name), "sc2c/%s_r%s", d->c.tag, room);
    CHECK_RC(d->engine->pf.load(d->engine->pf.ctx, name, record, sizeof(record), &len), SC2_OK);
    CHECK_RC(sc2e_record_unwrap(record, len, 0x31, record[5], &payload, &payload_len), SC2_OK);
    CHECK(payload_len == sizeof(rec));
    memcpy(&rec, payload, sizeof(rec));
    return rec.roster;
}

static void feed_commit(tdev_t *to, const sc2_commit_t *commit, tdev_t *signer,
                         const uint8_t outer_room[SC2_ROOM_ID_LEN], bool bad_signature)
{
    uint8_t rec[SC2_COMMIT_LEN], sig[SC2_SIG_LEN];
    char frame[1024], b64[256], sig64[128], room[SC2_DEVICE_ID_HEX_LEN + 1];
    size_t n;
    CHECK_RC(sc2_commit_encode(rec, commit), SC2_OK);
    CHECK_RC(sc2_commit_sign(sig, rec, signer->engine->id.ik_seed), SC2_OK);
    if (bad_signature) sig[0] ^= 1;
    CHECK_RC(sc2_b64_encode(b64, sizeof(b64), &n, rec, sizeof(rec)), SC2_OK);
    CHECK_RC(sc2_b64_encode(sig64, sizeof(sig64), &n, sig, sizeof(sig)), SC2_OK);
    CHECK_RC(sc2_hex_encode(room, sizeof(room), outer_room, SC2_ROOM_ID_LEN), SC2_OK);
    snprintf(frame, sizeof(frame), "{\"type\":\"room.commit\",\"room\":\"%s\",\"seq\":\"999\","
        "\"commit\":\"%s\",\"sig\":\"%s\"}", room, b64, sig64);
    feed_line(to, frame);
}

/* A hostile relay can forge notifications, reorder signed records and route
 * authenticated DMs from arbitrary devices. None grants room authority. */
static void private_room_trust(void)
{
    static tdev_t admin, member, attacker;
    d_init(&admin, "admin", "adm", NULL);
    d_init(&member, "member", "mem", NULL);
    d_init(&attacker, "attacker", "atk", NULL);
    tdev_t *all[] = {&admin, &member, &attacker};
    for (unsigned i = 0; i < 3; i++) {
        all[i]->sess = 0; /* capture outgoing frames without a real transport */
        all[i]->c.state = SC2C_READY;
        for (unsigned j = 0; j < 3; j++) if (i != j) bundle_to(all[i], all[j]);
    }
    admin.c.auto_admit = true;
    char room[SC2_DEVICE_ID_HEX_LEN + 1], invite[SC2_INVITE_TEXT_MAX];
    CHECK_RC(sc2c_room_create_named(&admin.c, "Team room", room), SC2_OK);
    q_clear(&admin.to_relay);
    feed_line(&admin, "{\"type\":\"ack\",\"rid\":\"c0\"}");
    CHECK_RC(sc2c_room_invite_text(&admin.c, room, invite, sizeof(invite)), SC2_OK);
    q_clear(&admin.to_relay);

    char frame[1024];
    snprintf(frame, sizeof(frame), "{\"type\":\"room.event\",\"room\":\"%s\","
        "\"event\":\"join_request\",\"device\":\"%s\",\"seq\":\"50\"}", room, attacker.c.self_hex);
    feed_line(&admin, frame);
    CHECK(take_type(&admin, "room.commit") == NULL);
    CHECK(room_roster(&admin, room).count == 1);

    /* An authenticated device still needs the invite-secret proof. */
    char inner[256];
    snprintf(inner, sizeof(inner), "{\"t\":\"room_join\",\"room\":\"%s\","
        "\"proof\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}", room);
    size_t n;
    char sealed[SC2E_FRAME_MAX];
    CHECK_RC(sc2e_dm_encrypt(attacker.engine, admin.c.self_hex, NULL, inner, strlen(inner),
                            sealed, sizeof(sealed), &n), SC2_OK);
    queue_t dmq = {0};
    q_push(&dmq, sealed, n);
    deliver_dm(&admin, q_pop(&dmq));
    CHECK(take_type(&admin, "room.commit") == NULL);
    CHECK(room_roster(&admin, room).count == 1);

    CHECK_RC(sc2c_room_join_invite(&member.c, invite, NULL), SC2_OK);
    q_clear(&member.to_relay);
    /* A foreign epoch key arriving first must create no engine room. */
    CHECK_RC(sc2e_room_create(attacker.engine, room, NULL, NULL), SC2_OK);
    CHECK_RC(sc2e_room_share(attacker.engine, room, member.c.self_hex, NULL, sealed, sizeof(sealed), &n), SC2_OK);
    q_push(&dmq, sealed, n);
    deliver_dm(&member, q_pop(&dmq));
    sc2e_room_info_t info;
    CHECK_RC(sc2e_room_info(member.engine, room, &info), SC2_OK);
    CHECK(!info.known);

    /* A pinned admin's bare epoch key is insufficient to bootstrap membership. */
    CHECK_RC(sc2e_room_share(admin.engine, room, member.c.self_hex, NULL, sealed, sizeof(sealed), &n), SC2_OK);
    q_push(&dmq, sealed, n);
    deliver_dm(&member, q_pop(&dmq));
    CHECK_RC(sc2e_room_info(member.engine, room, &info), SC2_OK);
    CHECK(!info.known);

    /* The member proves invite possession over its authenticated DM. */
    q_clear(&member.to_relay);
    feed_line(&member, "{\"type\":\"ack\",\"rid\":\"j0\",\"status\":\"requested\"}");
    line_t *request = take_type(&member, "dm.send");
    CHECK(request != NULL);
    deliver_dm(&admin, request);
    line_t *commit_frame = take_type(&admin, "room.commit");
    CHECK(commit_frame != NULL);
    free(commit_frame);
    feed_line(&admin, "{\"type\":\"ack\",\"rid\":\"c0\"}");
    sc2_roster_t roster = room_roster(&admin, room);
    CHECK(roster.epoch == 2 && roster.count == 2);
    deliver_dm(&member, take_type(&admin, "dm.send"));
    CHECK_RC(sc2e_room_info(member.engine, room, &info), SC2_OK);
    CHECK(info.known && info.epoch == 2 && !strcmp(info.admin, admin.c.self_hex));
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_JOINED);
    char title[SC2E_ROOM_TITLE_MAX + 1];
    CHECK_RC(sc2c_room_get_title(&member.c, room, title, sizeof(title)), SC2_OK);
    CHECK(!strcmp(title, "Team room"));
    snprintf(member.c.nick, sizeof(member.c.nick), "%s", "nils@cl32");
    q_clear(&member.to_relay);
    CHECK_RC(sc2c_submit(&member.c, SC2C_PRIVATE, room, "hello", 5, 92, 92), SC2_OK);
    line_t *text = take_type(&member, "room.send");
    CHECK(text != NULL);
    if (text) {
        CHECK_RC(sc2e_relay_room(text->s, text->len, 100, sealed, sizeof(sealed), &n), SC2_OK);
        feed_line(&admin, sealed);
        free(text);
        CHECK(!strcmp(admin.sender_name, "nils@cl32") && !strcmp(admin.sender_id, member.c.self_hex));
    }
    const char control_name[] = "{\"t\":\"text\",\"name\":\"bad\\u001b[31m\",\"body\":\"hello\"}";
    CHECK_RC(sc2e_room_encrypt(member.engine, room, NULL, control_name, strlen(control_name),
                               frame, sizeof(frame), &n), SC2_OK);
    size_t delivered_len;
    CHECK_RC(sc2e_relay_room(frame, n, 101, sealed, sizeof(sealed), &delivered_len), SC2_OK);
    feed_line(&admin, sealed);
    CHECK(!admin.sender_name[0] && !strcmp(admin.sender_id, member.c.self_hex));
    sc2_roster_t trusted = room_roster(&member, room);
    CHECK_MEM(&trusted, &roster, sizeof(roster));

    /* Forged removal events must not change authenticated membership. */
    snprintf(frame, sizeof(frame), "{\"type\":\"room.event\",\"room\":\"%s\","
        "\"event\":\"removed\",\"device\":\"%s\",\"seq\":\"51\"}", room, member.c.self_hex);
    feed_line(&member, frame);
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_JOINED);

    sc2_commit_t valid, forged;
    CHECK_RC(sc2_commit_make(&valid, &roster, admin.c.self_id, SC2_COMMIT_ROTATE, NULL), SC2_OK);
    feed_commit(&member, &valid, &admin, roster.room, true);
    forged = valid; memcpy(forged.by, attacker.c.self_id, sizeof(forged.by));
    feed_commit(&member, &forged, &attacker, roster.room, false);
    forged = valid; forged.prev[0] ^= 1;
    feed_commit(&member, &forged, &admin, roster.room, false);
    forged = valid; forged.epoch++;
    feed_commit(&member, &forged, &admin, roster.room, false);
    forged = valid; forged.roster_hash[0] ^= 1;
    feed_commit(&member, &forged, &admin, roster.room, false);
    forged = valid; forged.room[0] ^= 1;
    feed_commit(&member, &forged, &admin, roster.room, false);
    trusted = room_roster(&member, room);
    CHECK_MEM(&trusted, &roster, sizeof(roster));
    feed_commit(&member, &valid, &admin, roster.room, false);
    trusted = room_roster(&member, room);
    CHECK(trusted.epoch == 3);
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_WAITKEY);
    CHECK_RC(sc2c_submit(&member.c, SC2C_PRIVATE, room, "stale key", 9, 1, 1), SC2C_AGAIN);
    feed_commit(&member, &valid, &admin, roster.room, false); /* replay */
    CHECK(room_roster(&member, room).epoch == 3);

    /* Stale keys cannot roll back a signed epoch. A matching checkpoint can
     * finish the transition after the admin rotates its engine key. */
    CHECK_RC(sc2e_room_share_roster(admin.engine, room, member.c.self_hex, NULL, &roster,
                                   sealed, sizeof(sealed), &n), SC2_OK);
    q_push(&dmq, sealed, n); deliver_dm(&member, q_pop(&dmq));
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_WAITKEY);
    CHECK_RC(sc2e_room_rotate(admin.engine, room, NULL), SC2_OK);
    CHECK_RC(sc2e_room_share_roster(admin.engine, room, member.c.self_hex, NULL, &trusted,
                                   sealed, sizeof(sealed), &n), SC2_OK);
    q_push(&dmq, sealed, n); deliver_dm(&member, q_pop(&dmq));
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_JOINED);
    CHECK_RC(sc2e_room_info(member.engine, room, &info), SC2_OK);
    CHECK(info.epoch == 3);

    /* Administrative authority comes from the authenticated roster, and
     * delegated admin signatures must resolve to that device's key. */
    CHECK_RC(sc2_commit_make(&valid, &trusted, admin.c.self_id, SC2_COMMIT_ADD, attacker.c.self_id), SC2_OK);
    feed_commit(&member, &valid, &admin, roster.room, false);
    trusted = room_roster(&member, room);
    CHECK_RC(sc2_commit_make(&valid, &trusted, admin.c.self_id, SC2_COMMIT_PROMOTE, attacker.c.self_id), SC2_OK);
    feed_commit(&member, &valid, &admin, roster.room, false);
    trusted = room_roster(&member, room);
    CHECK_RC(sc2_commit_make(&valid, &trusted, attacker.c.self_id, SC2_COMMIT_ROTATE, NULL), SC2_OK);
    CHECK_RC(sc2e_forget_peer(member.engine, attacker.c.self_hex), SC2_OK);
    feed_commit(&member, &valid, &attacker, roster.room, false);
    CHECK(room_roster(&member, room).epoch == 5);
    CHECK(sc2c_room_get(&member.c, room)->commit_hold == 999);
    snprintf(frame, sizeof(frame), "{\"type\":\"sync.done\",\"inbox\":\"0\",\"rooms\":{\"%s\":\"1001\"}}", room);
    feed_line(&member, frame);
    CHECK(sc2c_room_get(&member.c, room)->cursor == 998);
    sc2e_identity_info_t identity;
    char bundle[SC2E_FRAME_MAX];
    size_t bundle_len;
    CHECK_RC(sc2e_identity(attacker.engine, &identity), SC2_OK);
    CHECK_RC(sc2e_keys_publish(attacker.engine, NULL, 0, sealed, sizeof(sealed), &n), SC2_OK);
    CHECK_RC(sc2e_relay_bundle(sealed, n, &identity, bundle, sizeof(bundle), &bundle_len), SC2_OK);
    feed_line(&member, bundle);
    CHECK(!sc2c_room_get(&member.c, room)->commit_hold);
    feed_commit(&member, &valid, &attacker, roster.room, false);
    trusted = room_roster(&member, room);
    CHECK(trusted.epoch == 6);

    /* A correctly chained signed removal erases keys and ends membership. */
    CHECK_RC(sc2_commit_make(&valid, &trusted, admin.c.self_id, SC2_COMMIT_REMOVE, member.c.self_id), SC2_OK);
    feed_commit(&member, &valid, &admin, roster.room, false);
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_LEFT);
    CHECK_RC(sc2c_room_add(&member.c, room, 0), SC2_OK);
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_LEFT &&
          !sc2c_room_get(&member.c, room)->want);
    sc2c_free(&member.c);
    sc2c_host_t host = {d_send, d_event, d_lock, d_unlock, t_alloc, t_release, d_mono, d_wall, &member};
    sc2c_config_t config = {.tag = "mem", .nick = "member"};
    CHECK_RC(sc2c_init(&member.c, &host, &config), SC2_OK);
    CHECK_RC(sc2c_room_add(&member.c, room, 0), SC2_OK);
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_LEFT &&
          !sc2c_room_get(&member.c, room)->want);
    CHECK_RC(sc2e_room_info(member.engine, room, &info), SC2_OK);
    CHECK(!info.known);
    /* Old room records retain their trust state when upgraded without a title. */
    char record_name[SC2E_NAME_MAX];
    snprintf(record_name, sizeof(record_name), "sc2c/mem_r%s", room);
    uint8_t record[sizeof(sc2c_roomrec_t) + SC2E_RECORD_OVERHEAD];
    size_t record_len, legacy_len;
    const uint8_t *payload;
    size_t payload_len;
    CHECK_RC(member.engine->pf.load(member.engine->pf.ctx, record_name, record, sizeof(record), &record_len), SC2_OK);
    CHECK_RC(sc2e_record_unwrap(record, record_len, 0x31, 2, &payload, &payload_len), SC2_OK);
    CHECK_RC(sc2e_record_wrap(record, sizeof(record), &legacy_len, 0x31, 1, payload,
                              offsetof(sc2c_roomrec_t, title)), SC2_OK);
    CHECK_RC(member.engine->pf.save(member.engine->pf.ctx, record_name, record, legacy_len), SC2_OK);
    CHECK_RC(sc2c_room_get_title(&member.c, room, title, sizeof(title)), SC2_OK);
    CHECK(!title[0] && member.c.rec.roster.epoch == 7);
    CHECK_RC(sc2c_room_add(&member.c, room, 0), SC2_OK);
    CHECK(sc2c_room_get(&member.c, room)->state == SC2C_ROOM_LEFT);
    for (unsigned i = 0; i < 3; i++) d_free(all[i]);
}

/* ---------------- tests ---------------- */

/* Close reasons the relay announces with an error frame (no rid). */
static void announced_closes(tdev_t *a)
{
    static const struct {
        const char *frame;
        sc2c_close_t want;
    } cases[] = {
        {"{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"closed after a period without activity\"}",
         SC2C_CLOSE_IDLE},
        {"{\"type\":\"error\",\"code\":503,\"err\":\"reconnect\",\"text\":\"session lifetime reached\"}",
         SC2C_CLOSE_RECONNECT},
        {"{\"type\":\"error\",\"code\":503,\"err\":\"evicted\",\"text\":\"relay full\"}", SC2C_CLOSE_EVICTED},
        /* a full relay is a failure: backoff and failover as before */
        {"{\"type\":\"error\",\"code\":503,\"err\":\"server_full\",\"text\":\"server full\"}", SC2C_CLOSE_NONE},
        /* the answer to one request never ends the session */
        {"{\"type\":\"error\",\"rid\":\"o1\",\"code\":503,\"err\":\"evicted\",\"text\":\"x\"}", SC2C_CLOSE_NONE},
        {"{\"type\":\"error\",\"code\":409,\"err\":\"epoch_conflict\",\"text\":\"x\"}", SC2C_CLOSE_NONE},
        {"{\"type\":\"error\",\"code\":409,\"err\":\"replaced\",\"text\":\"session replaced\"}",
         SC2C_CLOSE_REPLACED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        connect_to(a, 0);
        CHECK(sc2c_ready(&a->c));
        CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE);
        int notices = a->notices;
        feed_line(a, cases[i].frame);
        CHECK(a->notices == notices + 1);
        CHECK(sc2c_close_reason(&a->c) == cases[i].want);
        disconnect(a);
        /* still readable after the disconnect, for the reconnect decision */
        CHECK(sc2c_close_reason(&a->c) == cases[i].want);
        CHECK(sc2c_close_soft(cases[i].want) ==
              (cases[i].want != SC2C_CLOSE_NONE && cases[i].want != SC2C_CLOSE_REPLACED));
    }
    /* during login a refusal is never a soft close */
    sc2_mr_open(mr, 0);
    a->sess = 0;
    CHECK_RC(sc2c_on_connected(&a->c), SC2_OK);
    CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE); /* cleared on connect */
    feed_line(a, "{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"x\"}");
    CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE);
    disconnect(a);
}

static void reconnect_delays(tdev_t *a)
{
    /* soft: a short pause, the attempt counter is not touched */
    connect_to(a, 0);
    feed_line(a, "{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"x\"}");
    disconnect(a);
    uint32_t attempt = 3;
    for (uint32_t r = 0; r < 5000; r += 97) {
        uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, r);
        CHECK(ms >= SC2C_SOFT_RECONNECT_MS && ms <= SC2C_SOFT_RECONNECT_MS + 2000u);
    }
    CHECK(attempt == 3);
    /* a plain drop: the exponential backoff, counted */
    connect_to(a, 0);
    disconnect(a);
    attempt = 3;
    uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, 0);
    CHECK(attempt == 4 && ms >= 6000u && ms <= 10000u);
}

/* Replaced sessions back off, and keep backing off further while it goes
 * on, although each login in between resets the host's attempt counter. */
static void replaced_backoff(tdev_t *a)
{
    static const char replaced[] = "{\"type\":\"error\",\"code\":409,\"err\":\"replaced\",\"text\":\"x\"}";
    static const char idle[] = "{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"x\"}";
    /* start from a clean streak */
    connect_to(a, 0);
    disconnect(a);
    uint32_t prev = 0;
    for (unsigned round = 0; round < 8; round++) {
        uint32_t attempt = 0; /* reset by the login, as sc2-sync does on READY */
        connect_to(a, 0);
        CHECK(sc2c_ready(&a->c));
        feed_line(a, replaced);
        disconnect(a);
        CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_REPLACED);
        uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, 0); /* no jitter: lowest delay */
        CHECK(ms >= 3000u);
        CHECK(ms >= prev);
        CHECK(round < 4 ? ms > prev : ms == 45000u); /* 60 s minus the jitter */
        CHECK(attempt == round + 3u || (round >= 4 && attempt >= 7u));
        prev = ms;
    }
    /* any other close ends the streak */
    connect_to(a, 0);
    feed_line(a, idle);
    disconnect(a);
    connect_to(a, 0);
    feed_line(a, replaced);
    disconnect(a);
    uint32_t attempt = 0;
    uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, 0);
    CHECK(ms == 3000u && attempt == 3u);
    /* a plain drop ends it too */
    connect_to(a, 0);
    disconnect(a);
    connect_to(a, 0);
    feed_line(a, replaced);
    disconnect(a);
    attempt = 0;
    CHECK(sc2c_reconnect_ms(&a->c, &attempt, 0) == 3000u);
}

/* The same device key logs in on a second connection: the mini-relay
 * replaces the older session, whose client backs off. */
static void replaced_by_relay(tdev_t *a, tdev_t *a2)
{
    connect_to(a, 0);
    CHECK(sc2c_ready(&a->c));
    connect_to(a2, 1);
    CHECK(sc2c_ready(&a2->c));
    CHECK(a->sess < 0 && a->c.state == SC2C_OFFLINE);
    CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_REPLACED);
    CHECK(sc2c_close_reason(&a2->c) == SC2C_CLOSE_NONE);
    uint32_t attempt = 0;
    CHECK(sc2c_reconnect_ms(&a->c, &attempt, 0) >= 3000u && attempt > 0);
    disconnect(a2);
}

/* A client that only sends keepalive pings is closed as idle and treats it
 * as a normal reconnect. */
static void idle_by_relay(tdev_t *a)
{
    connect_to(a, 0);
    CHECK(sc2c_ready(&a->c));
    unsigned closed = 0;
    for (unsigned t = 0; t <= SC2_MR_IDLE_S + 30u && a->sess >= 0; t += 15u) {
        advance_s(15u);
        CHECK_RC(sc2c_tick(&a->c), SC2_OK); /* pings every SC2C_PING_MS */
        pump();
        closed += sc2_mr_tick(mr);
        pump();
    }
    CHECK(closed == 1 && a->sess < 0);
    CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_IDLE);
    uint32_t attempt = 2;
    uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, 7);
    CHECK(attempt == 2 && ms <= SC2C_SOFT_RECONNECT_MS + 2000u);
    /* and the next session starts clean */
    connect_to(a, 0);
    CHECK(sc2c_ready(&a->c) && sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE);
    disconnect(a);
}

int main(void)
{
    uint8_t seed[32] = {9};
    sc2_mr_config_t cfg = {.relay_id = "mr.test", .seed = seed, .rng = {sc2t_xs_fn, NULL}, .now_s = mr_now,
                           .send = mr_send, .alloc = t_alloc, .release = t_release, .sessions = NSESS,
                           .kick = mr_kick};
    CHECK_RC(sc2_mr_init(&mr, &cfg), SC2_OK);

    static tdev_t a, a2;
    d_init(&a, "alice", "a", NULL);
    d_init(&a2, "alice2", "b", &a); /* same device key, second connection */
    devs[ndevs++] = &a;
    devs[ndevs++] = &a2;

    announced_closes(&a);
    reconnect_delays(&a);
    replaced_backoff(&a);
    replaced_by_relay(&a, &a2);
    idle_by_relay(&a);
    private_room_trust();
    CHECK(!sc2e_label_valid("", SC2E_ROOM_TITLE_MAX));
    CHECK(!sc2e_label_valid("  ", SC2E_ROOM_TITLE_MAX));
    CHECK(!sc2e_label_valid("bad\nname", SC2E_ROOM_TITLE_MAX));
    CHECK(!sc2e_label_valid("bad\x1b[31mname", SC2E_ROOM_TITLE_MAX));
    CHECK(sc2e_label_valid("Team \"room\"", SC2E_ROOM_TITLE_MAX));

    d_free(&a2);
    d_free(&a);
    for (unsigned s = 0; s < NSESS; s++) {
        q_clear(&to_client[s]);
    }
    sc2_mr_free(mr);
    return sc2t_report("sc2_client_test: reconnects, admission, key authorization and signed membership");
}
