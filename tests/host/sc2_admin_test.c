/*
 * SC2 host test: private-room membership commits and rosters, relay-signed
 * moderation events and invites (protocol/sc2 sc2_commit.h, sc2_mod.h),
 * byte-exact against the Python reference vectors, plus a mutation loop.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_test_util.h"
#include "sc2_vectors.inc"

static char g_out[1024];

static void hexn(uint8_t *out, size_t n, const char *hex)
{
    sc2t_hexn(out, n, hex);
}

static uint8_t op_code(const char *op)
{
    static const char *names[] = {"", "create", "add", "remove", "promote", "demote", "rotate"};
    for (uint8_t i = 1; i < 7; i++) {
        if (strcmp(names[i], op) == 0) {
            return i;
        }
    }
    return 0;
}

static void member_id(uint8_t out[16], const char *who)
{
    if (strcmp(who, "a") == 0) {
        hexn(out, 16, SC2V_COMMIT_ID_A);
    } else if (strcmp(who, "b") == 0) {
        hexn(out, 16, SC2V_COMMIT_ID_B);
    } else if (strcmp(who, "c") == 0) {
        hexn(out, 16, SC2V_COMMIT_ID_C);
    } else if (strcmp(who, "d") == 0) {
        hexn(out, 16, SC2V_COMMIT_ID_D);
    } else {
        hexn(out, 16, SC2V_COMMIT_ID_E);
    }
}

/* Apply steps [0, k) to a fresh roster. */
static void roster_at(sc2_roster_t *r, unsigned k)
{
    uint8_t room[16], rec[SC2_COMMIT_LEN];
    hexn(room, 16, SC2V_COMMIT_ROOM);
    sc2_roster_init(r, room);
    for (unsigned i = 0; i < k; i++) {
        hexn(rec, SC2_COMMIT_LEN, SC2V_COMMIT_STEPS[i].rec);
        CHECK_RC(sc2_roster_apply(r, rec), SC2_OK);
    }
}

static void test_commit_chain(void)
{
    sc2_roster_t r;
    uint8_t room[16], rec[SC2_COMMIT_LEN], sig[64], ik[32], id[32], hash[32], made[SC2_COMMIT_LEN];
    hexn(room, 16, SC2V_COMMIT_ROOM);
    sc2_roster_init(&r, room);
    for (unsigned i = 0; i < SC2V_COUNT(SC2V_COMMIT_STEPS); i++) {
        const sc2v_commit_t *s = &SC2V_COMMIT_STEPS[i];
        sc2_commit_t c, m;
        uint8_t by[16], target[16] = {0};
        hexn(rec, sizeof(rec), s->rec);
        hexn(sig, 64, s->sig);
        hexn(ik, 32, s->ik);
        member_id(by, s->by);
        /* the admin side builds the identical record */
        CHECK_RC(sc2_commit_decode(&c, rec), SC2_OK);
        memcpy(target, c.target, 16);
        CHECK_RC(sc2_commit_make(&m, &r, by, op_code(s->op), target), SC2_OK);
        CHECK_RC(sc2_commit_encode(made, &m), SC2_OK);
        CHECK_MEM(made, rec, sizeof(rec));
        CHECK_RC(sc2_commit_verify(sig, rec, ik), SC2_OK);
        CHECK_RC(sc2_roster_check(&r, &c), SC2_OK);
        CHECK_RC(sc2_roster_apply(&r, rec), SC2_OK);
        CHECK(r.count == s->count);
        CHECK(r.epoch == i + 1u);
        hexn(id, 32, s->id);
        CHECK_MEM(r.head, id, 32);
        hexn(hash, 32, s->roster_hash);
        uint8_t got[32];
        CHECK_RC(sc2_roster_hash(&r, got), SC2_OK);
        CHECK_MEM(got, hash, 32);
        /* applying the same record again is a replay */
        sc2_roster_t before = r;
        CHECK_RC(sc2_roster_apply(&r, rec), SC2_ERR_STATE);
        CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    }
    CHECK(r.epoch == SC2V_COMMIT_FINAL_EPOCH);
    hexn(id, 32, SC2V_COMMIT_FINAL_HEAD);
    CHECK_MEM(r.head, id, 32);
    /* final roster: b (admin) and d */
    uint8_t b[16], d[16], a[16];
    member_id(a, "a");
    member_id(b, "b");
    member_id(d, "d");
    CHECK(r.count == 2u && sc2_roster_is_admin(&r, b) && !sc2_roster_is_admin(&r, d) && sc2_roster_find(&r, a) < 0);
    /* signing round trip with a fresh key */
    static const uint8_t seed[32] = {7};
    uint8_t pub[32], me[16];
    sc2_commit_t c;
    sc2_roster_t fresh;
    CHECK_RC(sc2_ed25519_public(pub, seed), SC2_OK);
    CHECK_RC(sc2_device_id(me, pub), SC2_OK);
    sc2_roster_init(&fresh, room);
    CHECK_RC(sc2_commit_make(&c, &fresh, me, SC2_COMMIT_CREATE, NULL), SC2_OK);
    CHECK_RC(sc2_commit_encode(rec, &c), SC2_OK);
    CHECK_RC(sc2_commit_sign(sig, rec, seed), SC2_OK);
    CHECK_RC(sc2_commit_verify(sig, rec, pub), SC2_OK);
    CHECK_RC(sc2_roster_apply(&fresh, rec), SC2_OK);
    CHECK(fresh.count == 1u && sc2_roster_is_admin(&fresh, me));
    /* admin-side refusals */
    CHECK_RC(sc2_commit_make(&c, &fresh, me, SC2_COMMIT_REMOVE, me), SC2_ERR_STATE);
    CHECK_RC(sc2_commit_make(&c, &fresh, me, SC2_COMMIT_DEMOTE, me), SC2_ERR_STATE);
    CHECK_RC(sc2_commit_make(&c, &fresh, b, SC2_COMMIT_ADD, d), SC2_ERR_STATE);
    CHECK_RC(sc2_commit_make(&c, &fresh, me, SC2_COMMIT_CREATE, NULL), SC2_ERR_STATE);
    CHECK_RC(sc2_commit_make(&c, &fresh, me, SC2_COMMIT_ADD, NULL), SC2_ERR_ARG);
    CHECK_RC(sc2_commit_make(&c, &fresh, me, 9, d), SC2_ERR_ARG);
}

static void test_commit_bad(void)
{
    for (unsigned i = 0; i < SC2V_COUNT(SC2V_COMMIT_BAD); i++) {
        const sc2v_commit_bad_t *b = &SC2V_COMMIT_BAD[i];
        sc2_roster_t r, before;
        sc2_commit_t c;
        uint8_t rec[SC2_COMMIT_LEN], sig[64], ik[32];
        hexn(rec, sizeof(rec), b->rec);
        hexn(sig, 64, b->sig);
        hexn(ik, 32, b->ik);
        roster_at(&r, b->at);
        before = r;
        int drc = sc2_commit_decode(&c, rec);
        if (strcmp(b->fail, "decode") == 0) {
            if (drc == SC2_OK) {
                fprintf(stderr, "decoded bad commit: %s\n", b->note);
            }
            CHECK(drc == SC2_ERR_FORMAT);
        } else {
            CHECK_RC(drc, SC2_OK);
            int vrc = sc2_commit_verify(sig, rec, ik);
            int crc = sc2_roster_check(&r, &c);
            if (strcmp(b->fail, "verify") == 0) {
                if (vrc == SC2_OK) {
                    fprintf(stderr, "verified bad commit: %s\n", b->note);
                }
                CHECK(vrc == SC2_ERR_AUTH);
            } else {
                CHECK_RC(vrc, SC2_OK);
                if (crc == SC2_OK) {
                    fprintf(stderr, "accepted bad commit: %s\n", b->note);
                }
                CHECK(crc == SC2_ERR_STATE);
            }
        }
        /* apply never accepts it either (signature aside) and leaves r untouched */
        if (strcmp(b->fail, "verify") != 0) {
            CHECK(sc2_roster_apply(&r, rec) != SC2_OK);
        }
        CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    }
}

static void test_commit_cap(void)
{
    /* a roster at the protocol maximum refuses a 33rd member */
    sc2_roster_t r;
    sc2_commit_t c;
    uint8_t room[16] = {1}, admin[16] = {0x80}, rec[SC2_COMMIT_LEN], t[16];
    sc2_roster_init(&r, room);
    CHECK_RC(sc2_commit_make(&c, &r, admin, SC2_COMMIT_CREATE, NULL), SC2_OK);
    CHECK_RC(sc2_commit_encode(rec, &c), SC2_OK);
    CHECK_RC(sc2_roster_apply(&r, rec), SC2_OK);
    for (unsigned i = 1; i < SC2_ROOM_MEMBERS_MAX; i++) {
        memset(t, 0, sizeof(t));
        t[0] = (uint8_t)(i * 7u);
        t[15] = (uint8_t)i;
        CHECK_RC(sc2_commit_make(&c, &r, admin, SC2_COMMIT_ADD, t), SC2_OK);
        CHECK_RC(sc2_commit_encode(rec, &c), SC2_OK);
        CHECK_RC(sc2_roster_apply(&r, rec), SC2_OK);
    }
    CHECK(r.count == SC2_ROOM_MEMBERS_MAX);
    for (unsigned i = 1; i < r.count; i++) {
        CHECK(memcmp(r.m[i - 1u].id, r.m[i].id, 16) < 0); /* kept sorted */
    }
    t[0] = 0xfe;
    CHECK_RC(sc2_commit_make(&c, &r, admin, SC2_COMMIT_ADD, t), SC2_ERR_STATE);
    printf("roster: %zu bytes for %u members (cap %u)\n", sizeof(sc2_roster_t), (unsigned)SC2_ROOM_MEMBERS_MAX,
           (unsigned)SC2_ROSTER_CAP);
}

static void test_commit_frames(void)
{
    uint8_t room[16], rec[SC2_COMMIT_LEN], sig[64], room2[16], rec2[SC2_COMMIT_LEN], sig2[64];
    size_t n = 0;
    uint64_t seq = 0;
    hexn(room, 16, SC2V_COMMIT_ROOM);
    hexn(rec, sizeof(rec), SC2V_COMMIT_STEPS[0].rec);
    hexn(sig, 64, SC2V_COMMIT_STEPS[0].sig);
    CHECK_RC(sc2_frame_room_create_private(g_out, sizeof(g_out), &n, "r1", room, rec, sig), SC2_OK);
    CHECK(strcmp(g_out, SC2V_FRAME_ROOM_CREATE_PRIVATE) == 0);
    CHECK_RC(sc2_frame_room_commit(g_out, sizeof(g_out), &n, "r1", room, rec, sig), SC2_ERR_ARG); /* genesis */
    hexn(rec, sizeof(rec), SC2V_COMMIT_STEPS[1].rec);
    hexn(sig, 64, SC2V_COMMIT_STEPS[1].sig);
    CHECK_RC(sc2_frame_room_create_private(g_out, sizeof(g_out), &n, "r1", room, rec, sig), SC2_ERR_ARG);
    CHECK_RC(sc2_frame_room_commit(g_out, sizeof(g_out), &n, "c1", room, rec, sig), SC2_OK);
    CHECK(strcmp(g_out, SC2V_FRAME_ROOM_COMMIT) == 0);
    CHECK_RC(sc2_frame_room_commit(g_out, 100, &n, "c1", room, rec, sig), SC2_ERR_BOUNDS);
    const char *msg = SC2V_FRAME_ROOM_COMMIT_MSG;
    CHECK_RC(sc2_frame_room_commit_decode(msg, strlen(msg), room2, &seq, rec2, sig2), SC2_OK);
    CHECK(seq == 4524u);
    CHECK_MEM(room2, room, 16);
    CHECK_MEM(rec2, rec, sizeof(rec));
    CHECK_MEM(sig2, sig, 64);
    /* the frame's room must be the record's room */
    char other[512];
    snprintf(other, sizeof(other), "{\"type\":\"room.commit\",\"room\":\"%032x\",\"seq\":\"1\",\"commit\":%s",
             0, strstr(msg, "\"commit\":") + 9);
    CHECK_RC(sc2_frame_room_commit_decode(other, strlen(other), room2, &seq, rec2, sig2), SC2_ERR_FORMAT);
    /* invites and joins */
    uint8_t ikey[32], proof[32];
    hexn(room, 16, SC2V_INVITE_ROOM);
    hexn(ikey, 32, SC2V_INVITE_KEY);
    hexn(proof, 32, SC2V_INVITE_PROOF);
    CHECK_RC(sc2_frame_room_invite(g_out, sizeof(g_out), &n, "i1", room, ikey, false), SC2_OK);
    CHECK(strcmp(g_out, SC2V_FRAME_ROOM_INVITE) == 0);
    CHECK_RC(sc2_frame_room_invite(g_out, sizeof(g_out), &n, "i2", room, ikey, true), SC2_OK);
    CHECK(strcmp(g_out, SC2V_FRAME_ROOM_INVITE_REVOKE) == 0);
    CHECK_RC(sc2_frame_room_join_private(g_out, sizeof(g_out), &n, "j1", room, proof), SC2_OK);
    CHECK(strcmp(g_out, SC2V_FRAME_ROOM_JOIN_PRIVATE) == 0);
}

static void test_mod(void)
{
    uint8_t pub[32], seed[32], rec[SC2_MOD_LEN], want[SC2_MOD_LEN], sig[64], vsig[64];
    size_t n = 0;
    hexn(pub, 32, SC2V_MOD_RELAY_PUB);
    hexn(seed, 32, SC2V_MOD_RELAY_SEED);
    for (unsigned i = 0; i < SC2V_COUNT(SC2V_MOD); i++) {
        const sc2v_mod_t *v = &SC2V_MOD[i];
        sc2_mod_t m, d;
        CHECK_RC(sc2_frame_room_mod_decode(v->frame, strlen(v->frame), &m, sig), SC2_OK);
        CHECK(strcmp(sc2_mod_action_name(m.action), v->action) == 0);
        CHECK(m.room_len == strlen(v->room) && memcmp(m.room, v->room, m.room_len) == 0);
        CHECK(m.seq == v->seq && m.ts == v->ts && m.until == v->until);
        CHECK_RC(sc2_mod_encode(rec, &m), SC2_OK);
        hexn(want, sizeof(want), v->rec);
        CHECK_MEM(rec, want, sizeof(rec));
        hexn(vsig, 64, v->sig);
        CHECK_MEM(sig, vsig, 64);
        CHECK_RC(sc2_mod_verify(sig, &m, pub), SC2_OK);
        CHECK_RC(sc2_mod_decode(&d, rec), SC2_OK);
        CHECK_RC(sc2_mod_encode(want, &d), SC2_OK);
        CHECK_MEM(rec, want, sizeof(rec));
        /* the relay side produces the identical frame and signature */
        uint8_t s2[64];
        CHECK_RC(sc2_mod_sign(s2, &m, seed), SC2_OK);
        CHECK_MEM(s2, sig, 64);
        CHECK_RC(sc2_frame_room_mod(g_out, sizeof(g_out), &n, &m, s2), SC2_OK);
        CHECK(strcmp(g_out, v->frame) == 0);
        /* any change to the event breaks the signature */
        m.seq++;
        CHECK_RC(sc2_mod_verify(sig, &m, pub), SC2_ERR_AUTH);
    }
    for (unsigned i = 0; i < SC2V_COUNT(SC2V_MOD_BAD); i++) {
        sc2_mod_t m;
        int rc = sc2_frame_room_mod_decode(SC2V_MOD_BAD[i].frame, strlen(SC2V_MOD_BAD[i].frame), &m, sig);
        if (rc == SC2_OK) {
            fprintf(stderr, "accepted bad room.mod: %s\n", SC2V_MOD_BAD[i].note);
        }
        CHECK(rc != SC2_OK);
    }
    sc2_mod_t m;
    CHECK_RC(sc2_frame_room_mod_decode(SC2V_MOD_TAMPERED, strlen(SC2V_MOD_TAMPERED), &m, sig), SC2_OK);
    CHECK_RC(sc2_mod_verify(sig, &m, pub), SC2_ERR_AUTH);
    CHECK(sc2_mod_action_parse("ban", 3) == SC2_MOD_BAN && sc2_mod_action_parse("bans", 4) == 0);
    /* record decode rejects non-zero padding after the room name */
    hexn(rec, sizeof(rec), SC2V_MOD[0].rec);
    rec[10 + 63] = 'x';
    CHECK_RC(sc2_mod_decode(&m, rec), SC2_ERR_FORMAT);
    /* field rules on both encode and record decode: until only on ban, ref only on remove */
    for (unsigned i = 0; i < SC2V_COUNT(SC2V_MOD); i++) {
        sc2_mod_t e;
        CHECK_RC(sc2_frame_room_mod_decode(SC2V_MOD[i].frame, strlen(SC2V_MOD[i].frame), &e, sig), SC2_OK);
        if (e.action == SC2_MOD_BAN) {
            continue;
        }
        sc2_mod_t bad = e;
        bad.until = 1;
        CHECK(sc2_mod_encode(rec, &bad) != SC2_OK);
        CHECK_RC(sc2_mod_encode(rec, &e), SC2_OK);
        rec[114 + 7] = 1; /* until = 1 inside the signed record */
        CHECK(sc2_mod_decode(&m, rec) != SC2_OK);
        if (e.action == SC2_MOD_UNBAN) {
            bad = e;
            bad.ref[0] = 1;
            CHECK(sc2_mod_encode(rec, &bad) != SC2_OK);
        }
    }
}

static void test_invite(void)
{
    uint8_t secret[16], room[16], joiner[16], key[32], proof[32], want[32], code[SC2_INVITE_CODE_MAX];
    uint8_t admin_ik[32];
    size_t n = 0, cl = 0;
    hexn(secret, 16, SC2V_INVITE_SECRET);
    hexn(room, 16, SC2V_INVITE_ROOM);
    hexn(joiner, 16, SC2V_INVITE_JOINER);
    hexn(admin_ik, 32, SC2V_INVITE_ADMIN_IK);
    CHECK_RC(sc2_invite_key(key, secret, room), SC2_OK);
    hexn(want, 32, SC2V_INVITE_KEY);
    CHECK_MEM(key, want, 32);
    CHECK_RC(sc2_invite_proof(proof, key, joiner), SC2_OK);
    hexn(want, 32, SC2V_INVITE_PROOF);
    CHECK_MEM(proof, want, 32);
    sc2_invite_t inv, back;
    memset(&inv, 0, sizeof(inv));
    memcpy(inv.room, room, 16);
    memcpy(inv.secret, secret, 16);
    memcpy(inv.admin_ik, admin_ik, 32);
    hexn(inv.admin_id, 16, SC2V_INVITE_ADMIN_ID);
    inv.url_len = strlen(SC2V_INVITE_URL);
    memcpy(inv.url, SC2V_INVITE_URL, inv.url_len);
    CHECK_RC(sc2_invite_encode(code, sizeof(code), &cl, &inv), SC2_OK);
    size_t wl = 0;
    uint8_t *wcode = sc2t_hex(SC2V_INVITE_CODE, &wl);
    CHECK(cl == wl);
    CHECK_MEM(code, wcode, wl);
    CHECK_RC(sc2_invite_decode(&back, wcode, wl), SC2_OK);
    CHECK(memcmp(&back, &inv, sizeof(inv)) == 0);
    free(wcode);
    char text[SC2_INVITE_TEXT_MAX];
    CHECK_RC(sc2_invite_to_text(text, sizeof(text), &n, &inv), SC2_OK);
    CHECK(strcmp(text, SC2V_INVITE_TEXT) == 0 && n == strlen(text));
    CHECK_RC(sc2_invite_from_text(&back, text, n), SC2_OK);
    CHECK(memcmp(&back, &inv, sizeof(inv)) == 0);
    CHECK_RC(sc2_invite_from_text(&back, text + 1, n - 1), SC2_ERR_FORMAT);
    CHECK_RC(sc2_invite_to_text(text, 20, &n, &inv), SC2_ERR_BOUNDS);
    for (unsigned i = 0; i < SC2V_COUNT(SC2V_INVITE_BAD); i++) {
        size_t bl = 0;
        uint8_t *b = sc2t_hex(SC2V_INVITE_BAD[i].code, &bl);
        int rc = sc2_invite_decode(&back, b, bl);
        if (rc == SC2_OK) {
            fprintf(stderr, "accepted bad invite: %s\n", SC2V_INVITE_BAD[i].note);
        }
        CHECK(rc == SC2_ERR_FORMAT);
        free(b);
    }
    inv.admin_id[0] ^= 1;
    CHECK_RC(sc2_invite_encode(code, sizeof(code), &cl, &inv), SC2_ERR_ARG);
}

static void test_mutations(void)
{
    /* flip bytes of valid records and frames: nothing crashes, and a
     * flipped commit never applies under its original signature. */
    uint32_t x = 0x5c2a11u;
    unsigned applied = 0;
    for (unsigned it = 0; it < 20000; it++) {
        x = x * 1103515245u + 12345u;
        unsigned k = 1u + (x >> 8) % (SC2V_COUNT(SC2V_COMMIT_STEPS) - 1u);
        sc2_roster_t r;
        roster_at(&r, k);
        uint8_t rec[SC2_COMMIT_LEN], sig[64], ik[32];
        sc2_commit_t c;
        hexn(rec, sizeof(rec), SC2V_COMMIT_STEPS[k].rec);
        hexn(sig, 64, SC2V_COMMIT_STEPS[k].sig);
        hexn(ik, 32, SC2V_COMMIT_STEPS[k].ik);
        rec[(x >> 3) % SC2_COMMIT_LEN] ^= (uint8_t)(1u << ((x >> 17) & 7u));
        if (sc2_commit_decode(&c, rec) == SC2_OK && sc2_commit_verify(sig, rec, ik) == SC2_OK &&
            sc2_roster_apply(&r, rec) == SC2_OK) {
            applied++;
        }
        char line[1024];
        const sc2v_mod_t *v = &SC2V_MOD[it % SC2V_COUNT(SC2V_MOD)];
        size_t len = strlen(v->frame);
        memcpy(line, v->frame, len + 1u);
        line[(x >> 5) % len] = (char)(x >> 24);
        sc2_mod_t m;
        uint8_t msig[64];
        (void)sc2_frame_room_mod_decode(line, len, &m, msig);
        uint8_t room[16];
        uint64_t seq;
        const char *cm = SC2V_FRAME_ROOM_COMMIT_MSG;
        len = strlen(cm);
        memcpy(line, cm, len + 1u);
        line[(x >> 7) % len] = (char)(x >> 16);
        (void)sc2_frame_room_commit_decode(line, len, room, &seq, rec, sig);
    }
    CHECK(applied == 0);
    printf("mutations: 20000 commit records, 20000 room.mod and 20000 room.commit frames\n");
}

int main(void)
{
    test_commit_chain();
    test_commit_bad();
    test_commit_cap();
    test_commit_frames();
    test_mod();
    test_invite();
    test_mutations();
    return sc2t_report("sc2_admin_test");
}
