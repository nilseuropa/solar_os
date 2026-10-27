/*
 * SC2 host test: identity keys, X3DH, double ratchet (byte-exact transcript
 * replay against the Python/PyNaCl reference), state blobs, sender-key rooms
 * and safety numbers.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_test_util.h"
#include "sc2_vectors.inc"

typedef struct {
    sc2t_script_t script;
    sc2_rng_t rng;
} scripted_t;

static void scripted_init(scripted_t *s, const char *hex)
{
    size_t n;
    s->script.data = sc2t_hex(hex, &n);
    s->script.len = n;
    s->script.pos = 0;
    s->rng.fn = sc2t_script_fn;
    s->rng.ctx = &s->script;
}

static void scripted_free(scripted_t *s)
{
    free((void *)s->script.data);
}

static void test_identity(void)
{
    scripted_t r;
    scripted_init(&r, SC2V_ID_RNG);
    sc2_identity_t id;
    uint8_t want[64];
    CHECK_RC(sc2_identity_generate(&id, &r.rng), SC2_OK);
    CHECK(r.script.pos == 64);
    sc2t_hexn(want, 32, SC2V_ID_IK_PUB);
    CHECK_MEM(id.ik_pub, want, 32);
    sc2t_hexn(want, 32, SC2V_ID_DK_PUB);
    CHECK_MEM(id.dk_pub, want, 32);
    sc2t_hexn(want, 64, SC2V_ID_DK_SIG);
    CHECK_MEM(id.dk_sig, want, 64);
    uint8_t dev[16];
    char devhex[33];
    CHECK_RC(sc2_device_id(dev, id.ik_pub), SC2_OK);
    sc2t_hexn(want, 16, SC2V_ID_DEVICE_ID);
    CHECK_MEM(dev, want, 16);
    CHECK_RC(sc2_device_id_hex(devhex, id.ik_pub), SC2_OK);
    CHECK(strcmp(devhex, SC2V_ID_DEVICE_ID) == 0);
    CHECK_RC(sc2_dk_verify(id.dk_sig, id.ik_pub, id.dk_pub), SC2_OK);
    uint8_t other[32];
    memcpy(other, id.dk_pub, 32);
    other[0] ^= 1;
    CHECK_RC(sc2_dk_verify(id.dk_sig, id.ik_pub, other), SC2_ERR_AUTH);

    sc2_identity_t again;
    CHECK_RC(sc2_identity_from_secrets(&again, id.ik_seed, id.dk_priv), SC2_OK);
    CHECK_MEM(&again, &id, sizeof(id));

    sc2_spk_t spk;
    CHECK_RC(sc2_spk_generate(&spk, SC2V_ID_SPK_ID, &id, &r.rng), SC2_OK);
    CHECK(r.script.pos == 96);
    sc2t_hexn(want, 32, SC2V_ID_SPK_PUB);
    CHECK_MEM(spk.pub, want, 32);
    sc2t_hexn(want, 64, SC2V_ID_SPK_SIG);
    CHECK_MEM(spk.sig, want, 64);
    CHECK_RC(sc2_spk_verify(spk.sig, id.ik_pub, SC2V_ID_SPK_ID, spk.pub), SC2_OK);
    CHECK_RC(sc2_spk_verify(spk.sig, id.ik_pub, SC2V_ID_SPK_ID + 1u, spk.pub), SC2_ERR_AUTH);

    scripted_t ro;
    scripted_init(&ro, SC2V_ID_OPK_RNG);
    sc2_opk_t opk;
    CHECK_RC(sc2_opk_generate(&opk, SC2V_ID_OPK_ID, &ro.rng), SC2_OK);
    sc2t_hexn(want, 32, SC2V_ID_OPK_PUB);
    CHECK_MEM(opk.pub, want, 32);
    CHECK(opk.id == SC2V_ID_OPK_ID);
    CHECK_RC(sc2_opk_generate(&opk, SC2V_ID_OPK_ID, &ro.rng), SC2_ERR_RNG);
    scripted_free(&ro);

    uint8_t ch[32], sig[64];
    sc2t_hexn(ch, 32, SC2V_ID_CHALLENGE);
    const char *relay = SC2V_ID_RELAY_ID;
    CHECK_RC(sc2_auth_sign(sig, &id, relay, strlen(relay), ch), SC2_OK);
    sc2t_hexn(want, 64, SC2V_ID_AUTH_SIG);
    CHECK_MEM(sig, want, 64);
    CHECK_RC(sc2_auth_verify(sig, id.ik_pub, relay, strlen(relay), ch), SC2_OK);
    CHECK_RC(sc2_auth_verify(sig, id.ik_pub, "relay.exampl", 12, ch), SC2_ERR_AUTH);
    ch[31] ^= 1;
    CHECK_RC(sc2_auth_verify(sig, id.ik_pub, relay, strlen(relay), ch), SC2_ERR_AUTH);
    static char longid[257];
    memset(longid, 'r', 256);
    CHECK_RC(sc2_auth_sign(sig, &id, longid, 256, ch), SC2_ERR_ARG);

    /* DK rotation keeps the IK and re-signs */
    sc2_rng_t xs = {sc2t_xs_fn, NULL};
    CHECK_RC(sc2_identity_rotate_dk(&id, &xs), SC2_OK);
    CHECK_MEM(id.ik_pub, again.ik_pub, 32);
    CHECK(memcmp(id.dk_pub, again.dk_pub, 32) != 0);
    CHECK_RC(sc2_dk_verify(id.dk_sig, id.ik_pub, id.dk_pub), SC2_OK);
    sc2_identity_wipe(&id);
    CHECK(sc2_ct_is_zero(&id, sizeof(id)));
    sc2_rng_t none = {sc2t_fail_fn, NULL};
    CHECK_RC(sc2_identity_generate(&id, &none), SC2_ERR_RNG);
    scripted_free(&r);
}

static void test_x3dh(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_X3DH); i++) {
        const sc2v_x3dh_t *v = &SC2V_X3DH[i];
        scripted_t ra, rb, re;
        scripted_init(&ra, v->alice_rng);
        scripted_init(&rb, v->bob_rng);
        scripted_init(&re, v->ek_rng);
        sc2_identity_t alice, bob;
        sc2_spk_t spk;
        sc2_opk_t opk;
        int with_opk = v->opk_rng[0] != '\0';
        CHECK_RC(sc2_identity_generate(&alice, &ra.rng), SC2_OK);
        CHECK_RC(sc2_identity_generate(&bob, &rb.rng), SC2_OK);
        CHECK_RC(sc2_spk_generate(&spk, v->spk_id, &bob, &rb.rng), SC2_OK);
        if (with_opk) {
            scripted_t ro;
            scripted_init(&ro, v->opk_rng);
            CHECK_RC(sc2_opk_generate(&opk, v->opk_id, &ro.rng), SC2_OK);
            scripted_free(&ro);
        }
        sc2_bundle_t b;
        CHECK_RC(sc2_bundle_from_keys(&b, &bob, &spk, with_opk ? &opk : NULL), SC2_OK);
        CHECK(b.opk_id == v->opk_id);
        CHECK_RC(sc2_bundle_verify(&b), SC2_OK);
        uint8_t sk[32], ad[64], sk2[32], ad2[64], want[64];
        sc2_x3dh_hdr_t hdr;
        CHECK_RC(sc2_x3dh_initiate(sk, ad, &hdr, &alice, &b, &re.rng), SC2_OK);
        sc2t_hexn(want, 32, v->ek_pub);
        CHECK_MEM(hdr.ek, want, 32);
        CHECK(hdr.spk_id == v->spk_id && hdr.opk_id == v->opk_id);
        sc2t_hexn(want, 32, v->sk);
        CHECK_MEM(sk, want, 32);
        sc2t_hexn(want, 64, v->ad);
        CHECK_MEM(ad, want, 64);
        CHECK_RC(sc2_x3dh_respond(sk2, ad2, &bob, &spk, with_opk ? &opk : NULL, alice.ik_pub, alice.dk_pub,
                                  alice.dk_sig, &hdr),
                 SC2_OK);
        CHECK_MEM(sk2, sk, 32);
        CHECK_MEM(ad2, ad, 64);

        /* negative cases */
        sc2_bundle_t bad = b;
        bad.dk_sig[3] ^= 1;
        CHECK_RC(sc2_bundle_verify(&bad), SC2_ERR_AUTH);
        CHECK_RC(sc2_x3dh_initiate(sk2, ad2, &hdr, &alice, &bad, &re.rng), SC2_ERR_AUTH);
        bad = b;
        bad.spk_id++;
        CHECK_RC(sc2_bundle_verify(&bad), SC2_ERR_AUTH);
        bad = b;
        memset(bad.spk, 0, 32); /* small-order point fails before signature use */
        CHECK(sc2_bundle_verify(&bad) != SC2_OK);
        sc2_x3dh_hdr_t h2 = hdr;
        h2.spk_id++;
        CHECK_RC(sc2_x3dh_respond(sk2, ad2, &bob, &spk, with_opk ? &opk : NULL, alice.ik_pub, alice.dk_pub,
                                  alice.dk_sig, &h2),
                 SC2_ERR_STATE);
        h2 = hdr;
        memset(h2.ek, 0, 32);
        CHECK_RC(sc2_x3dh_respond(sk2, ad2, &bob, &spk, with_opk ? &opk : NULL, alice.ik_pub, alice.dk_pub,
                                  alice.dk_sig, &h2),
                 SC2_ERR_KEY);
        uint8_t badsig[64];
        memcpy(badsig, alice.dk_sig, 64);
        badsig[0] ^= 1;
        CHECK_RC(sc2_x3dh_respond(sk2, ad2, &bob, &spk, with_opk ? &opk : NULL, alice.ik_pub, alice.dk_pub, badsig,
                                  &hdr),
                 SC2_ERR_AUTH);
        if (with_opk) {
            /* OPK announced but already consumed locally */
            CHECK_RC(sc2_x3dh_respond(sk2, ad2, &bob, &spk, NULL, alice.ik_pub, alice.dk_pub, alice.dk_sig, &hdr),
                     SC2_ERR_STATE);
        }
        scripted_free(&ra);
        scripted_free(&rb);
        scripted_free(&re);
    }
}

/* ---- ratchet transcript replay ---- */

#define MAX_MSGS 80
typedef struct {
    sc2_dm_header_t hdr;
    uint8_t nonce[24];
    uint8_t ct[SC2_DM_MAX_CIPHERTEXT];
    size_t ct_len;
} msg_t;

static msg_t g_msgs[MAX_MSGS];
static sc2_ratchet_t g_work;

static void test_ratchet_transcript(void)
{
    scripted_t ra, rb;
    scripted_init(&ra, SC2V_RT_ALICE_RNG);
    scripted_init(&rb, SC2V_RT_BOB_RNG);
    sc2_identity_t alice, bob;
    sc2_spk_t spk;
    sc2_opk_t opk;
    CHECK_RC(sc2_identity_generate(&alice, &ra.rng), SC2_OK);
    CHECK_RC(sc2_identity_generate(&bob, &rb.rng), SC2_OK);
    CHECK_RC(sc2_spk_generate(&spk, SC2V_RT_SPK_ID, &bob, &rb.rng), SC2_OK);
    CHECK_RC(sc2_opk_generate(&opk, SC2V_RT_OPK_ID, &rb.rng), SC2_OK);
    CHECK(ra.script.pos == SC2V_RT_ALICE_SETUP && rb.script.pos == SC2V_RT_BOB_SETUP);
    uint8_t aid[16], bid[16];
    sc2_device_id(aid, alice.ik_pub);
    sc2_device_id(bid, bob.ik_pub);

    sc2_bundle_t bundle;
    CHECK_RC(sc2_bundle_from_keys(&bundle, &bob, &spk, &opk), SC2_OK);
    uint8_t sk[32], ad[64], want[64];
    sc2_x3dh_hdr_t x3;
    static sc2_ratchet_t A, B;
    int have_b = 0;
    CHECK_RC(sc2_x3dh_initiate(sk, ad, &x3, &alice, &bundle, &ra.rng), SC2_OK);
    sc2t_hexn(want, 32, SC2V_RT_SK);
    CHECK_MEM(sk, want, 32);
    sc2t_hexn(want, 64, SC2V_RT_AD);
    CHECK_MEM(ad, want, 64);
    CHECK_RC(sc2_ratchet_init_initiator(&A, sk, ad, aid, bid, spk.pub, &x3, &ra.rng), SC2_OK);
    CHECK(ra.script.pos == SC2V_RT_ALICE_INIT);
    /* Bob cannot send before receiving */
    {
        static sc2_ratchet_t tmp;
        sc2_dm_header_t h;
        uint8_t n[24], c[300];
        size_t cl;
        CHECK_RC(sc2_ratchet_init_responder(&tmp, sk, ad, bid, aid, &spk), SC2_OK);
        CHECK_RC(sc2_ratchet_encrypt(&tmp, &h, n, c, sizeof(c), &cl, (const uint8_t *)"x", 1, &rb.rng),
                 SC2_ERR_STATE);
    }

    size_t nmsgs = 0;
    static uint8_t pt[SC2_DM_MAX_CIPHERTEXT];
    static uint8_t before[8192], after[8192];
    for (size_t i = 0; i < SC2V_COUNT(SC2V_RT_STEPS); i++) {
        const sc2v_step_t *s = &SC2V_RT_STEPS[i];
        scripted_t *r = s->actor == 'A' ? &ra : &rb;
        sc2_ratchet_t *st = s->actor == 'A' ? &A : &B;
        size_t ptl;
        uint8_t *wpt = sc2t_hex(s->pt, &ptl);
        CHECK(r->script.pos == s->rng_from);
        if (s->op == 'E') {
            CHECK(nmsgs == s->msg && nmsgs < MAX_MSGS);
            msg_t *m = &g_msgs[nmsgs++];
            CHECK_RC(sc2_ratchet_encrypt(st, &m->hdr, m->nonce, m->ct, sizeof(m->ct), &m->ct_len, wpt, ptl, &r->rng),
                     SC2_OK);
            sc2t_hexn(want, 32, s->dh);
            CHECK_MEM(m->hdr.dh, want, 32);
            CHECK(m->hdr.pn == s->pn && m->hdr.n == s->n && m->hdr.has_x3dh == (s->x3dh != 0));
            sc2t_hexn(want, 24, s->nonce);
            CHECK_MEM(m->nonce, want, 24);
            size_t cl;
            uint8_t *wct = sc2t_hex(s->ct, &cl);
            CHECK(cl == m->ct_len);
            if (cl == m->ct_len) {
                CHECK_MEM(m->ct, wct, cl);
            }
            free(wct);
        } else {
            msg_t *m = &g_msgs[s->msg];
            if (!have_b && s->actor == 'B') {
                uint8_t sk2[32], ad2[64];
                CHECK(m->hdr.has_x3dh);
                CHECK_RC(sc2_x3dh_respond(sk2, ad2, &bob, &spk, m->hdr.x3dh.opk_id == opk.id ? &opk : NULL,
                                          alice.ik_pub, alice.dk_pub, alice.dk_sig, &m->hdr.x3dh),
                         SC2_OK);
                CHECK_RC(sc2_ratchet_init_responder(&B, sk2, ad2, bid, aid, &spk), SC2_OK);
                have_b = 1;
            }
            static uint8_t ct[SC2_DM_MAX_CIPHERTEXT];
            memcpy(ct, m->ct, m->ct_len);
            if (s->tamper) {
                ct[5] ^= 0x40;
            }
            size_t bl0 = 0, bl1 = 0;
            CHECK_RC(sc2_ratchet_serialize(st, before, sizeof(before), &bl0), SC2_OK);
            size_t got = 0;
            int rc = sc2_ratchet_decrypt(st, &g_work, &m->hdr, m->nonce, ct, m->ct_len, pt, sizeof(pt), &got,
                                         s->now, &r->rng);
            if (rc != s->rc) {
                fprintf(stderr, "step %zu: decrypt rc %d, reference %d\n", i, rc, s->rc);
            }
            CHECK(rc == s->rc);
            if (rc == SC2_OK) {
                CHECK(got == ptl && memcmp(pt, wpt, ptl) == 0);
            } else {
                /* failed decrypts leave the committed state untouched */
                CHECK_RC(sc2_ratchet_serialize(st, after, sizeof(after), &bl1), SC2_OK);
                CHECK(bl0 == bl1 && memcmp(before, after, bl0) == 0);
                r->script.pos = s->rng_from;
            }
        }
        if (r->script.pos != s->rng_to) {
            fprintf(stderr, "step %zu: rng at %zu, reference %u\n", i, r->script.pos, s->rng_to);
        }
        CHECK(r->script.pos == s->rng_to);
        free(wpt);
    }
    CHECK(sc2_ratchet_skipped_count(&A) == SC2V_RT_ALICE_SKIPPED);
    CHECK(sc2_ratchet_skipped_count(&B) == SC2V_RT_BOB_SKIPPED);

    /* final state blobs are byte-identical to the reference */
    static uint8_t blob[8192];
    const struct {
        sc2_ratchet_t *st;
        const char *hex;
    } sides[2] = {{&A, SC2V_RT_ALICE_BLOB}, {&B, SC2V_RT_BOB_BLOB}};
    for (int k = 0; k < 2; k++) {
        size_t wl, bl = 0;
        uint8_t *wb = sc2t_hex(sides[k].hex, &wl);
        CHECK_RC(sc2_ratchet_serialize(sides[k].st, blob, sizeof(blob), &bl), SC2_OK);
        CHECK(bl == wl && bl <= sc2_ratchet_blob_max());
        if (bl == wl) {
            CHECK_MEM(blob, wb, bl);
        }
        if (bl != 351u + 76u * sc2_ratchet_skipped_count(sides[k].st)) {
            fprintf(stderr, "blob %zu bytes with %zu skipped keys\n", bl, sc2_ratchet_skipped_count(sides[k].st));
        }
        CHECK(bl == 351u + 76u * sc2_ratchet_skipped_count(sides[k].st));
        static sc2_ratchet_t copy;
        CHECK_RC(sc2_ratchet_deserialize(&copy, wb, wl), SC2_OK);
        size_t cl2 = 0;
        CHECK_RC(sc2_ratchet_serialize(&copy, blob, sizeof(blob), &cl2), SC2_OK);
        CHECK(cl2 == wl && memcmp(blob, wb, wl) == 0);
        CHECK_RC(sc2_ratchet_serialize(sides[k].st, blob, wl - 1u, &cl2), SC2_ERR_BOUNDS);
        /* corruption, truncation, bad version, inconsistent count */
        for (size_t pos = 0; pos < wl; pos += 7) {
            wb[pos] ^= 0x10;
            CHECK_RC(sc2_ratchet_deserialize(&copy, wb, wl), SC2_ERR_FORMAT);
            wb[pos] ^= 0x10;
        }
        CHECK_RC(sc2_ratchet_deserialize(&copy, wb, wl - 1u), SC2_ERR_FORMAT);
        CHECK_RC(sc2_ratchet_deserialize(&copy, wb, 10), SC2_ERR_FORMAT);
        uint8_t *mod = (uint8_t *)malloc(wl);
        memcpy(mod, wb, wl);
        mod[4] = 2; /* version */
        uint32_t crc = sc2_crc32(0, mod, wl - 4u);
        sc2_store_be32(mod + wl - 4u, crc);
        CHECK_RC(sc2_ratchet_deserialize(&copy, mod, wl), SC2_ERR_FORMAT);
        free(mod);
        /* a blob with more skipped keys than this build stores (written by
         * a build with a larger SC2_RATCHET_MAX_STORED) loads with the
         * oldest entries dropped */
        {
            const size_t base = 347u, entry = 76u, extra = 3u;
            const size_t count = SC2_RATCHET_MAX_STORED + extra;
            size_t bigl = base + count * entry + 4u;
            uint8_t *big = (uint8_t *)calloc(1, bigl);
            memcpy(big, wb, base);
            big[base - 1u] = (uint8_t)count;
            for (size_t i = 0; i < count; i++) {
                uint8_t *en = big + base + i * entry;
                memset(en, 0x40 + (int)(i & 0x3f), 32);
                sc2_store_be32(en + 32, (uint32_t)(1000u + i));
                memset(en + 36, 0x11, 32);
                sc2_store_be64(en + 68, 1700000000u);
            }
            sc2_store_be32(big + bigl - 4u, sc2_crc32(0, big, bigl - 4u));
            CHECK_RC(sc2_ratchet_deserialize(&copy, big, bigl), SC2_OK);
            CHECK(sc2_ratchet_skipped_count(&copy) == SC2_RATCHET_MAX_STORED);
            CHECK(copy.skipped[0].n == 1000u + extra);
            CHECK(copy.skipped[SC2_RATCHET_MAX_STORED - 1u].n == 1000u + count - 1u);
            free(big);
        }
        free(wb);
    }

    /* the session keeps working after restore: continue the conversation */
    static sc2_ratchet_t A2, B2;
    sc2_rng_t xs = {sc2t_xs_fn, NULL};
    CHECK_RC(sc2_ratchet_serialize(&A, blob, sizeof(blob), &(size_t){0}), SC2_OK);
    {
        size_t l;
        sc2_ratchet_serialize(&A, blob, sizeof(blob), &l);
        CHECK_RC(sc2_ratchet_deserialize(&A2, blob, l), SC2_OK);
        sc2_ratchet_serialize(&B, blob, sizeof(blob), &l);
        CHECK_RC(sc2_ratchet_deserialize(&B2, blob, l), SC2_OK);
    }
    for (int k = 0; k < 6; k++) {
        msg_t *m = &g_msgs[0];
        char text[64];
        int tl = snprintf(text, sizeof(text), "{\"t\":\"text\",\"body\":\"after restore %d\"}", k);
        sc2_ratchet_t *tx = (k & 1) ? &B2 : &A2, *rx = (k & 1) ? &A2 : &B2;
        CHECK_RC(sc2_ratchet_encrypt(tx, &m->hdr, m->nonce, m->ct, sizeof(m->ct), &m->ct_len, (const uint8_t *)text,
                                     (size_t)tl, &xs),
                 SC2_OK);
        size_t got;
        CHECK_RC(sc2_ratchet_decrypt(rx, &g_work, &m->hdr, m->nonce, m->ct, m->ct_len, pt, sizeof(pt), &got, 0, &xs),
                 SC2_OK);
        CHECK(got == (size_t)tl && memcmp(pt, text, got) == 0);
    }
    /* header encoding length: with and without X3DH */
    {
        uint8_t hb[SC2_DM_HDR_MAX_LEN];
        size_t hl;
        sc2_dm_header_t h;
        memset(&h, 0, sizeof(h));
        CHECK_RC(sc2_dm_header_encode(hb, &hl, &h, aid, bid), SC2_OK);
        CHECK(hl == SC2_DM_HDR_BASE_LEN && memcmp(hb, "SC2D", 4) == 0);
        h.has_x3dh = 1;
        CHECK_RC(sc2_dm_header_encode(hb, &hl, &h, aid, bid), SC2_OK);
        CHECK(hl == SC2_DM_HDR_MAX_LEN && hb[77] == SC2_DM_HDR_FLAG_X3DH);
    }
    sc2_ratchet_wipe(&A);
    CHECK(sc2_ct_is_zero(&A, sizeof(A)));
    scripted_free(&ra);
    scripted_free(&rb);
}

/* Skipped-key store: per-gap limit, FIFO eviction at capacity, TTL. */
static void test_ratchet_store(void)
{
    sc2_rng_t xs = {sc2t_xs_fn, NULL};
    sc2_identity_t a, b;
    sc2_spk_t spk;
    CHECK_RC(sc2_identity_generate(&a, &xs), SC2_OK);
    CHECK_RC(sc2_identity_generate(&b, &xs), SC2_OK);
    CHECK_RC(sc2_spk_generate(&spk, 1, &b, &xs), SC2_OK);
    sc2_bundle_t bundle;
    CHECK_RC(sc2_bundle_from_keys(&bundle, &b, &spk, NULL), SC2_OK);
    uint8_t sk[32], ad[64], sk2[32], ad2[64], aid[16], bid[16];
    sc2_device_id(aid, a.ik_pub);
    sc2_device_id(bid, b.ik_pub);
    sc2_x3dh_hdr_t x3;
    static sc2_ratchet_t A, B;
    CHECK_RC(sc2_x3dh_initiate(sk, ad, &x3, &a, &bundle, &xs), SC2_OK);
    CHECK(x3.opk_id == SC2_NO_OPK);
    CHECK_RC(sc2_ratchet_init_initiator(&A, sk, ad, aid, bid, spk.pub, &x3, &xs), SC2_OK);
    CHECK_RC(sc2_x3dh_respond(sk2, ad2, &b, &spk, NULL, a.ik_pub, a.dk_pub, a.dk_sig, &x3), SC2_OK);
    CHECK_RC(sc2_ratchet_init_responder(&B, sk2, ad2, bid, aid, &spk), SC2_OK);

    static uint8_t pt[SC2_DM_MAX_CIPHERTEXT];
    for (size_t i = 0; i < 41; i++) {
        char t[32];
        int tl = snprintf(t, sizeof(t), "m%zu", i);
        CHECK_RC(sc2_ratchet_encrypt(&A, &g_msgs[i].hdr, g_msgs[i].nonce, g_msgs[i].ct, sizeof(g_msgs[i].ct),
                                     &g_msgs[i].ct_len, (const uint8_t *)t, (size_t)tl, &xs),
                 SC2_OK);
        CHECK(g_msgs[i].ct_len == 256u + 16u);
    }
    size_t got;
#define DEC(i, now) \
    sc2_ratchet_decrypt(&B, &g_work, &g_msgs[i].hdr, g_msgs[i].nonce, g_msgs[i].ct, g_msgs[i].ct_len, pt, sizeof(pt), \
                        &got, (now), &xs)
    CHECK_RC(DEC(33, 100), SC2_ERR_SKIP);
    CHECK(sc2_ratchet_skipped_count(&B) == 0);
    CHECK_RC(DEC(32, 100), SC2_OK);
    CHECK(sc2_ratchet_skipped_count(&B) == 32);
    CHECK_RC(DEC(40, 200), SC2_OK);          /* 7 more skipped keys: oldest 7 evicted */
    CHECK(sc2_ratchet_skipped_count(&B) == 32);
    for (size_t i = 0; i < 7; i++) {
        CHECK_RC(DEC(i, 300), SC2_ERR_REPLAY);
    }
    CHECK_RC(DEC(7, 300), SC2_OK);
    CHECK(got == 2 && memcmp(pt, "m7", 2) == 0);
    CHECK_RC(DEC(7, 300), SC2_ERR_REPLAY);
    CHECK(sc2_ratchet_skipped_count(&B) == 31);
    /* stored at 100 (25 keys: 8..31) and 200 (33..39); expire the first batch */
    CHECK(sc2_ratchet_expire(&B, 100u + SC2_RATCHET_SKIP_TTL - 1u) == 0);
    CHECK(sc2_ratchet_expire(&B, 100u + SC2_RATCHET_SKIP_TTL) == 24);
    CHECK(sc2_ratchet_skipped_count(&B) == 7);
    CHECK_RC(DEC(20, 100u + SC2_RATCHET_SKIP_TTL), SC2_ERR_REPLAY);
    CHECK_RC(DEC(35, 100u + SC2_RATCHET_SKIP_TTL), SC2_OK);
    /* decrypt expires on its work copy; a failed decrypt commits nothing */
    CHECK_RC(DEC(36, 200u + SC2_RATCHET_SKIP_TTL), SC2_ERR_REPLAY);
    CHECK(sc2_ratchet_skipped_count(&B) == 6);
    CHECK_RC(DEC(36, 200u + SC2_RATCHET_SKIP_TTL - 1u), SC2_OK);
    CHECK(sc2_ratchet_skipped_count(&B) == 5);
    /* truncated ciphertext and bad header values */
    CHECK_RC(sc2_ratchet_decrypt(&B, &g_work, &g_msgs[39].hdr, g_msgs[39].nonce, g_msgs[39].ct, 15, pt, sizeof(pt),
                                 &got, 0, &xs),
             SC2_ERR_FORMAT);
    sc2_dm_header_t h = g_msgs[39].hdr;
    memset(h.dh, 0, 32);
    CHECK(sc2_ratchet_decrypt(&B, &g_work, &h, g_msgs[39].nonce, g_msgs[39].ct, g_msgs[39].ct_len, pt, sizeof(pt),
                              &got, 0, &xs) != SC2_OK);
    CHECK_RC(sc2_ratchet_decrypt(&B, &g_work, &g_msgs[39].hdr, g_msgs[39].nonce, g_msgs[39].ct, g_msgs[39].ct_len, pt,
                                 10, &got, 0, &xs),
             SC2_ERR_BOUNDS);
    CHECK_RC(DEC(39, 0), SC2_OK);
    CHECK(sc2_ratchet_expire(&B, 200u + SC2_RATCHET_SKIP_TTL) == 4);
    CHECK(sc2_ratchet_skipped_count(&B) == 0);
    CHECK_RC(DEC(38, 0), SC2_ERR_REPLAY);
    /* oversize plaintext */
    static uint8_t big[SC2_DM_MAX_PLAINTEXT + 1];
    CHECK_RC(sc2_ratchet_encrypt(&A, &h, g_msgs[0].nonce, g_msgs[0].ct, sizeof(g_msgs[0].ct), &got, big,
                                 sizeof(big), &xs),
             SC2_ERR_BOUNDS);
    CHECK_RC(sc2_ratchet_encrypt(&A, &h, g_msgs[0].nonce, g_msgs[0].ct, sizeof(g_msgs[0].ct), &got, big,
                                 SC2_DM_MAX_PLAINTEXT, &xs),
             SC2_OK);
    CHECK(got == 8192u + 16u);
#undef DEC
}

/* Randomised two-party conversation with reordering and drops. */
static void test_ratchet_random(void)
{
    sc2_rng_t xs = {sc2t_xs_fn, NULL};
    sc2_identity_t a, b;
    sc2_spk_t spk;
    sc2_opk_t opk;
    sc2_identity_generate(&a, &xs);
    sc2_identity_generate(&b, &xs);
    sc2_spk_generate(&spk, 9, &b, &xs);
    sc2_opk_generate(&opk, 4, &xs);
    sc2_bundle_t bundle;
    sc2_bundle_from_keys(&bundle, &b, &spk, &opk);
    uint8_t sk[32], ad[64], aid[16], bid[16];
    sc2_device_id(aid, a.ik_pub);
    sc2_device_id(bid, b.ik_pub);
    sc2_x3dh_hdr_t x3;
    static sc2_ratchet_t P[2];
    int b_ready = 0;
    CHECK_RC(sc2_x3dh_initiate(sk, ad, &x3, &a, &bundle, &xs), SC2_OK);
    CHECK_RC(sc2_ratchet_init_initiator(&P[0], sk, ad, aid, bid, spk.pub, &x3, &xs), SC2_OK);
    static msg_t q[2][8];
    size_t qn[2] = {0, 0};
    static uint8_t pt[SC2_DM_MAX_CIPHERTEXT];
    unsigned delivered = 0;
    for (int round = 0; round < 600; round++) {
        int who = (int)(sc2t_xs() % 2u);
        if (who == 1 && !b_ready) {
            who = 0;
        }
        uint64_t act = sc2t_xs() % 10u;
        if (act < 5 && qn[who] < 8) {
            msg_t *m = &q[who][qn[who]];
            char t[48];
            int tl = snprintf(t, sizeof(t), "r%d from %d", round, who);
            CHECK_RC(sc2_ratchet_encrypt(&P[who], &m->hdr, m->nonce, m->ct, sizeof(m->ct), &m->ct_len,
                                         (const uint8_t *)t, (size_t)tl, &xs),
                     SC2_OK);
            m->ct[m->ct_len] = (uint8_t)tl; /* remember length after the ciphertext (buffer has room) */
            memcpy(m->ct + m->ct_len + 1u, t, (size_t)tl);
            qn[who]++;
        } else if (qn[who] > 0) {
            size_t pick = (size_t)(sc2t_xs() % qn[who]);
            msg_t *m = &q[who][pick];
            int rx = 1 - who;
            if (rx == 1 && !b_ready) {
                uint8_t sk2[32], ad2[64];
                CHECK_RC(sc2_x3dh_respond(sk2, ad2, &b, &spk, &opk, a.ik_pub, a.dk_pub, a.dk_sig, &m->hdr.x3dh),
                         SC2_OK);
                CHECK_RC(sc2_ratchet_init_responder(&P[1], sk2, ad2, bid, aid, &spk), SC2_OK);
                b_ready = 1;
            }
            int drop = act == 9;
            if (!drop) {
                size_t got;
                CHECK_RC(sc2_ratchet_decrypt(&P[rx], &g_work, &m->hdr, m->nonce, m->ct, m->ct_len, pt, sizeof(pt),
                                             &got, (uint64_t)round, &xs),
                         SC2_OK);
                CHECK(got == m->ct[m->ct_len] && memcmp(pt, m->ct + m->ct_len + 1u, got) == 0);
                delivered++;
            }
            q[who][pick] = q[who][qn[who] - 1u];
            qn[who]--;
        }
    }
    CHECK(delivered > 100);
}

static void test_room(void)
{
    uint8_t ek[32], room[16], sender[16], want[64], mk[32];
    sc2t_hexn(ek, 32, SC2V_ROOM_EPOCH_KEY);
    sc2t_hexn(room, 16, SC2V_ROOM_ROOM);
    sc2t_hexn(sender, 16, SC2V_ROOM_SENDER);
    sc2_room_chain_t ch, ch2;
    CHECK_RC(sc2_room_chain_init(&ch, ek, room, SC2V_ROOM_EPOCH, sender), SC2_OK);
    sc2t_hexn(want, 32, SC2V_ROOM_CK0);
    CHECK_MEM(ch.ck, want, 32);
    CHECK(ch.next == 0);
    ch2 = ch;
    const char *mks[3] = {SC2V_ROOM_MK0, SC2V_ROOM_MK1, SC2V_ROOM_MK2};
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t n;
        CHECK_RC(sc2_room_chain_next(&ch, mk, &n), SC2_OK);
        CHECK(n == i);
        sc2t_hexn(want, 32, mks[i]);
        CHECK_MEM(mk, want, 32);
    }
    /* receiver side: jump to n=2 without commit, then commit */
    CHECK_RC(sc2_room_chain_key(&ch2, 2, mk, 0), SC2_OK);
    CHECK_MEM(mk, want, 32);
    CHECK(ch2.next == 0);
    CHECK_RC(sc2_room_chain_key(&ch2, 2, mk, 1), SC2_OK);
    CHECK(ch2.next == 3);
    CHECK_RC(sc2_room_chain_key(&ch2, 2, mk, 1), SC2_ERR_REPLAY);
    CHECK_RC(sc2_room_chain_key(&ch2, 3u + SC2_ROOM_MAX_ADVANCE, mk, 0), SC2_ERR_SKIP);
    CHECK_RC(sc2_room_chain_key(&ch2, 3u + SC2_ROOM_MAX_ADVANCE - 1u, mk, 0), SC2_OK);

    uint8_t hdr[SC2_ROOM_HDR_LEN], seed[32], ik[32], nonce[24], sig[64];
    CHECK_RC(sc2_room_header(hdr, room, SC2V_ROOM_EPOCH, sender, 2), SC2_OK);
    size_t hl;
    uint8_t *wh = sc2t_hex(SC2V_ROOM_HDR, &hl);
    CHECK(hl == SC2_ROOM_HDR_LEN);
    CHECK_MEM(hdr, wh, hl);
    free(wh);
    sc2t_hexn(seed, 32, SC2V_ROOM_SEED);
    sc2t_hexn(ik, 32, SC2V_ROOM_IK_PUB);
    sc2t_hexn(nonce, 24, SC2V_ROOM_NONCE);
    size_t pl, cl, got;
    uint8_t *pt = sc2t_hex(SC2V_ROOM_PT, &pl);
    uint8_t *wct = sc2t_hex(SC2V_ROOM_CT, &cl);
    static uint8_t ct[8192 + 16], out[8192 + 16];
    sc2t_hexn(mk, 32, SC2V_ROOM_MK2);
    CHECK_RC(sc2_room_seal(ct, sizeof(ct), &got, sig, mk, nonce, hdr, pt, pl, seed), SC2_OK);
    CHECK(got == cl);
    CHECK_MEM(ct, wct, cl);
    sc2t_hexn(want, 64, SC2V_ROOM_SIG);
    CHECK_MEM(sig, want, 64);
    CHECK_RC(sc2_room_verify(sig, hdr, nonce, ct, cl, ik), SC2_OK);
    CHECK_RC(sc2_room_open(out, sizeof(out), &got, mk, nonce, hdr, ct, cl, sig, ik), SC2_OK);
    CHECK(got == pl && memcmp(out, pt, pl) == 0);
    /* signature binds header, nonce and ciphertext; AEAD binds header */
    ct[0] ^= 1;
    CHECK_RC(sc2_room_open(out, sizeof(out), &got, mk, nonce, hdr, ct, cl, sig, ik), SC2_ERR_AUTH);
    ct[0] ^= 1;
    nonce[0] ^= 1;
    CHECK_RC(sc2_room_verify(sig, hdr, nonce, ct, cl, ik), SC2_ERR_AUTH);
    nonce[0] ^= 1;
    hdr[44] ^= 1;
    CHECK_RC(sc2_room_verify(sig, hdr, nonce, ct, cl, ik), SC2_ERR_AUTH);
    hdr[44] ^= 1;
    ik[0] ^= 1;
    CHECK(sc2_room_open(out, sizeof(out), &got, mk, nonce, hdr, ct, cl, sig, ik) != SC2_OK);
    ik[0] ^= 1;
    mk[0] ^= 1;
    CHECK_RC(sc2_room_open(out, sizeof(out), &got, mk, nonce, hdr, ct, cl, sig, ik), SC2_ERR_AUTH);
    mk[0] ^= 1;
    CHECK_RC(sc2_room_open(out, 10, &got, mk, nonce, hdr, ct, cl, sig, ik), SC2_ERR_BOUNDS);
    CHECK_RC(sc2_room_seal(ct, 100, &got, sig, mk, nonce, hdr, pt, pl, seed), SC2_ERR_BOUNDS);
    free(pt);
    free(wct);
}

static void test_safety(void)
{
    for (size_t i = 0; i < SC2V_COUNT(SC2V_SAFETY); i++) {
        const sc2v_safety_t *v = &SC2V_SAFETY[i];
        uint8_t a[32], b[32], h[32], h2[32], want[32];
        sc2t_hexn(a, 32, v->ik_a);
        sc2t_hexn(b, 32, v->ik_b);
        sc2t_hexn(want, 32, v->hash);
        CHECK_RC(sc2_safety_hash(h, a, b), SC2_OK);
        CHECK_MEM(h, want, 32);
        CHECK_RC(sc2_safety_hash(h2, b, a), SC2_OK);
        CHECK_MEM(h2, h, 32);
        char s[SC2_SAFETY_SHORT_LEN + 1], f[SC2_SAFETY_FULL_LEN + 1], w[SC2_SAFETY_WORDS_MAX + 1];
        CHECK_RC(sc2_safety_short(s, h), SC2_OK);
        CHECK(strcmp(s, v->shrt) == 0 && strlen(s) == SC2_SAFETY_SHORT_LEN);
        CHECK_RC(sc2_safety_full(f, h), SC2_OK);
        CHECK(strcmp(f, v->full) == 0 && strlen(f) == SC2_SAFETY_FULL_LEN);
        CHECK_RC(sc2_safety_words(w, sizeof(w), h, '-'), SC2_OK);
        if (strcmp(w, v->words) != 0) {
            fprintf(stderr, "words '%s' vs '%s'\n", w, v->words);
        }
        CHECK(strcmp(w, v->words) == 0);
        CHECK_RC(sc2_safety_words(w, strlen(v->words), h, '-'), SC2_ERR_BOUNDS);
        CHECK_RC(sc2_fingerprint_short(s, a), SC2_OK);
        CHECK(strcmp(s, v->fp_a) == 0);
    }
    CHECK(strcmp(sc2_wordlist_en(0), "abandon") == 0);
    CHECK(strcmp(sc2_wordlist_en(2047), "zoo") == 0);
    CHECK(sc2_wordlist_en(2048) == NULL);
    size_t maxlen = 0;
    for (uint16_t i = 0; i < SC2_WORDLIST_SIZE; i++) {
        size_t l = strlen(sc2_wordlist_en(i));
        maxlen = l > maxlen ? l : maxlen;
        if (i) {
            CHECK(strcmp(sc2_wordlist_en((uint16_t)(i - 1u)), sc2_wordlist_en(i)) < 0);
        }
    }
    CHECK(maxlen == 8);
    /* SHA-256 of the newline-joined list equals the upstream BIP-39 english.txt */
    sc2_sha256_ctx c;
    sc2_sha256_init(&c);
    for (uint16_t i = 0; i < SC2_WORDLIST_SIZE; i++) {
        const char *wd = sc2_wordlist_en(i);
        sc2_sha256_update(&c, (const uint8_t *)wd, strlen(wd));
        sc2_sha256_update(&c, (const uint8_t *)"\n", 1);
    }
    uint8_t d[32], want[32];
    sc2_sha256_final(&c, d);
    sc2t_hexn(want, 32, "2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda");
    CHECK_MEM(d, want, 32);
}

int main(void)
{
    test_identity();
    test_x3dh();
    test_ratchet_transcript();
    test_ratchet_store();
    test_ratchet_random();
    test_room();
    test_safety();
    return sc2t_report("sc2_session_test");
}
