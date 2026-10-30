/*
 * Host tests for the SC2 session engine and selftest used by the SolarOS
 * service (src/services/solar_os_sc2_engine.c, solar_os_sc2_selftest.c).
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>

#include "sc2_test_util.h"
#include "solar_os_sc2_engine.h"
#include "solar_os_sc2_selftest.h"

static size_t live_allocs;

static void *t_alloc(void *ctx, size_t len)
{
    (void)ctx;
    void *p = calloc(1, len);
    if (p) {
        live_allocs++;
    }
    return p;
}

static void t_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    if (ptr) {
        sc2_memzero(ptr, len);
        free(ptr);
        live_allocs--;
    }
}

static uint64_t t_clock(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static sc2e_test_host_t host(void)
{
    sc2e_test_host_t h = {t_alloc, t_release, t_clock, {sc2t_xs_fn, NULL}, NULL};
    return h;
}

static void report(void *ctx, const char *name, int rc)
{
    (void)ctx;
    if (rc != SC2_OK) {
        fprintf(stderr, "selftest %s: %s\n", name, sc2_strerror(rc));
    }
}

static void bench_report(void *ctx, const sc2e_bench_result_t *r)
{
    int *count = ctx;
    (*count)++;
    if (r->rc != SC2_OK) {
        fprintf(stderr, "bench %s: %s\n", r->name, sc2_strerror(r->rc));
    }
}

typedef struct {
    sc2e_memstore_t *store;
    sc2e_t e;
    sc2e_identity_info_t info;
} dev_t;

static char f1[SC2E_FRAME_MAX], f2[SC2E_FRAME_MAX], pt[SC2E_INNER_MAX + 1];
static uint64_t seq;

static void dev_open(dev_t *d, const sc2e_test_host_t *h)
{
    sc2e_platform_t pf;
    CHECK_RC(sc2e_memstore_create(&d->store, h), SC2_OK);
    sc2e_memstore_platform(d->store, &pf);
    CHECK_RC(sc2e_open(&d->e, &pf), SC2_OK);
    CHECK(d->e.created);
    CHECK_RC(sc2e_identity(&d->e, &d->info), SC2_OK);
}

static void dev_close(dev_t *d)
{
    sc2e_close(&d->e);
    sc2e_memstore_destroy(d->store);
}

/* Give `to` the bundle of `owner`, as fetched with keys.claim. */
static void give_bundle(dev_t *owner, dev_t *to, uint32_t opks)
{
    size_t n = 0, m = 0;
    char dev[SC2_DEVICE_ID_HEX_LEN + 1];
    CHECK_RC(sc2e_keys_publish(&owner->e, "k", opks, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_bundle(f1, n, &owner->info, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_add_bundle(&to->e, f2, m, dev), SC2_OK);
    CHECK(strcmp(dev, owner->info.device_id) == 0);
}

static int send_dm(dev_t *from, dev_t *to, const char *inner, sc2e_dm_result_t *res, size_t *pl)
{
    size_t n = 0, m = 0;
    int rc = sc2e_dm_encrypt(&from->e, to->info.device_id, NULL, inner, strlen(inner), f1, sizeof(f1), &n);
    if (rc == SC2_OK) {
        rc = sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_dm_decrypt(&to->e, f2, m, res, pt, sizeof(pt), pl);
    }
    return rc;
}

static void test_selftest_and_bench(void)
{
    sc2e_test_host_t h = host();
    unsigned passed = 0, failed = 0;
    CHECK_RC(sc2e_selftest(&h, report, NULL, &passed, &failed), SC2_OK);
    CHECK(failed == 0);
    CHECK(passed == 15);
    int count = 0;
    CHECK_RC(sc2e_bench(&h, 2, bench_report, &count), SC2_OK);
    CHECK(count == 10);
    CHECK(live_allocs == 0);
}

/* A fallback relay holds the signed prekey only: SPK-only X3DH works and
 * leaves the one-time prekeys for the primary. */
static void test_no_opk_bundle(void)
{
    sc2e_test_host_t h = host();
    dev_t a, b;
    sc2e_dm_result_t res;
    size_t pl = 0;
    dev_open(&a, &h);
    dev_open(&b, &h);
    give_bundle(&b, &a, 4);
    uint32_t before = a.e.pre.opk_count;
    give_bundle(&a, &b, SC2E_OPK_NONE);
    sc2e_peer_info_t pi;
    CHECK_RC(sc2e_peer_info(&b.e, a.info.device_id, &pi), SC2_OK);
    CHECK(pi.known && !pi.opk_available);
    const char *inner = "{\"t\":\"text\",\"body\":\"via fallback\"}";
    CHECK_RC(send_dm(&b, &a, inner, &res, &pl), SC2_OK);
    CHECK(pl == strlen(inner) && memcmp(pt, inner, pl) == 0);
    CHECK(a.e.pre.opk_count == before);
    CHECK_RC(send_dm(&a, &b, "{\"t\":\"text\",\"body\":\"reply\"}", &res, &pl), SC2_OK);
    dev_close(&a);
    dev_close(&b);
}

static void test_publish_and_bundle_rules(void)
{
    sc2e_test_host_t h = host();
    dev_t a, b;
    size_t n = 0, m = 0;
    dev_open(&a, &h);
    dev_open(&b, &h);
    CHECK(strcmp(a.info.device_id, b.info.device_id) != 0);
    CHECK(a.info.opk_count == 0 && a.info.spk_id == 1);

    /* OPK top-up is capped and persisted. */
    CHECK_RC(sc2e_keys_publish(&a.e, NULL, 1000, f1, sizeof(f1), &n), SC2_OK);
    CHECK(a.e.pre.opk_count == SC2E_OPK_MAX);
    CHECK(strstr(f1, "\"type\":\"keys.publish\"") != NULL);
    CHECK(strstr(f1, "\"rid\"") == NULL);
    /* Fallback relays get the signed prekey only; the OPKs stay for the primary. */
    CHECK_RC(sc2e_keys_publish(&a.e, NULL, SC2E_OPK_NONE, f1, sizeof(f1), &n), SC2_OK);
    CHECK(a.e.pre.opk_count == SC2E_OPK_MAX);
    CHECK(strstr(f1, "\"opks\":[]") != NULL);
    CHECK_RC(sc2e_keys_publish(&a.e, NULL, 1000, f1, sizeof(f1), &n), SC2_OK);

    /* A bundle must hash to its device ID and carry valid signatures. */
    CHECK_RC(sc2e_relay_bundle(f1, n, &a.info, f2, sizeof(f2), &m), SC2_OK);
    char *dk_sig = strstr(f2, "\"dk_sig\":\"");
    CHECK(dk_sig != NULL);
    if (dk_sig) {
        char save = dk_sig[12];
        dk_sig[12] = save == 'A' ? 'B' : 'A';
        CHECK(sc2e_add_bundle(&b.e, f2, m, NULL) != SC2_OK);
        dk_sig[12] = save;
    }
    sc2e_identity_info_t wrong = a.info;
    memcpy(wrong.device_id, b.info.device_id, sizeof(wrong.device_id));
    CHECK_RC(sc2e_relay_bundle(f1, n, &wrong, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_add_bundle(&b.e, f2, m, NULL), SC2_ERR_AUTH);
    /* Own bundle is refused. */
    CHECK_RC(sc2e_relay_bundle(f1, n, &a.info, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_add_bundle(&a.e, f2, m, NULL), SC2_ERR_ARG);
    CHECK_RC(sc2e_add_bundle(&b.e, f2, m, NULL), SC2_OK);

    sc2e_peer_info_t pi;
    CHECK_RC(sc2e_peer_info(&b.e, a.info.device_id, &pi), SC2_OK);
    CHECK(pi.known && !pi.has_session && pi.opk_available);
    sc2e_counts_t c;
    CHECK_RC(sc2e_counts(&b.e, &c), SC2_OK);
    CHECK(c.peers == 1 && c.sessions == 0 && c.rooms == 0);

    /* Unknown peer: no bundle, no session. */
    CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, "{\"t\":\"text\"}", 12, f1, sizeof(f1), &n),
             SC2_ERR_NOT_FOUND);
    CHECK_RC(sc2e_dm_encrypt(&a.e, "zz", NULL, "{\"t\":\"text\"}", 12, f1, sizeof(f1), &n), SC2_ERR_ARG);
    dev_close(&a);
    dev_close(&b);
}

static void test_dm_policies(void)
{
    sc2e_test_host_t h = host();
    dev_t a, b, c;
    sc2e_dm_result_t res;
    size_t pl = 0, n = 0;
    dev_open(&a, &h);
    dev_open(&b, &h);
    dev_open(&c, &h);
    give_bundle(&b, &a, 1);
    give_bundle(&a, &b, 0);

    /* First message consumes Bob's only OPK; Bob's prekeys shrink. */
    CHECK_RC(send_dm(&a, &b, "{\"t\":\"text\",\"body\":\"1\"}", &res, &pl), SC2_OK);
    CHECK(res.new_session && res.kind == SC2E_DM_MESSAGE);
    CHECK(b.e.pre.opk_count == 0);
    /* Alice's stored copy of the bundle no longer offers that OPK. */
    sc2e_peer_info_t pi;
    CHECK_RC(sc2e_peer_info(&a.e, b.info.device_id, &pi), SC2_OK);
    CHECK(pi.known && pi.has_session && !pi.opk_available);

    /* Out-of-order delivery inside one chain. */
    char m1[SC2E_FRAME_MAX], m2[SC2E_FRAME_MAX];
    size_t l1 = 0, l2 = 0, x = 0;
    CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, "{\"t\":\"text\",\"body\":\"2\"}", 23, f1, sizeof(f1), &x),
             SC2_OK);
    CHECK_RC(sc2e_relay_dm(f1, x, ++seq, m1, sizeof(m1), &l1), SC2_OK);
    CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, "{\"t\":\"text\",\"body\":\"3\"}", 23, f1, sizeof(f1), &x),
             SC2_OK);
    CHECK_RC(sc2e_relay_dm(f1, x, ++seq, m2, sizeof(m2), &l2), SC2_OK);
    CHECK_RC(sc2e_dm_decrypt(&b.e, m2, l2, &res, pt, sizeof(pt), &pl), SC2_OK);
    CHECK(strstr(pt, "\"3\"") != NULL);
    CHECK_RC(sc2e_dm_decrypt(&b.e, m1, l1, &res, pt, sizeof(pt), &pl), SC2_OK);
    CHECK(strstr(pt, "\"2\"") != NULL);
    CHECK(sc2e_dm_decrypt(&b.e, m1, l1, &res, pt, sizeof(pt), &pl) != SC2_OK);

    /* Too-small output buffer is reported, not truncated. */
    CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, "{\"t\":\"text\",\"body\":\"4\"}", 23, f1, sizeof(f1), &x),
             SC2_OK);
    CHECK_RC(sc2e_relay_dm(f1, x, ++seq, m1, sizeof(m1), &l1), SC2_OK);
    char tiny[8];
    CHECK_RC(sc2e_dm_decrypt(&b.e, m1, l1, &res, tiny, sizeof(tiny), &pl), SC2_ERR_BOUNDS);

    /* Unknown sender with X3DH: Bob has no bundle for Carol. */
    give_bundle(&b, &c, 0);
    CHECK_RC(send_dm(&c, &b, "{\"t\":\"text\"}", &res, &pl), SC2_ERR_NOT_FOUND);

    /* Epoch keys only from the room admin. */
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    uint32_t epoch = 0;
    CHECK_RC(sc2e_room_create(&a.e, NULL, room, &epoch), SC2_OK);
    CHECK_RC(sc2e_room_create(&a.e, room, NULL, NULL), SC2_ERR_STATE);
    CHECK_RC(sc2e_room_share(&a.e, room, b.info.device_id, NULL, f1, sizeof(f1), &n), SC2_OK);
    size_t m = 0;
    CHECK_RC(sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_dm_decrypt(&b.e, f2, m, &res, pt, sizeof(pt), &pl), SC2_OK);
    CHECK(res.kind == SC2E_DM_EPOCH_KEY && pl == 0 && pt[0] == '\0');
    /* Carol (who knows Bob) creates a room with the same ID and shares it:
     * Bob must ignore the foreign key because Alice is the admin. */
    give_bundle(&c, &b, 0);
    CHECK_RC(sc2e_room_create(&c.e, room, NULL, NULL), SC2_OK);
    CHECK_RC(sc2e_room_rotate(&c.e, room, &epoch), SC2_OK);
    CHECK_RC(sc2e_room_share(&c.e, room, b.info.device_id, NULL, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_dm_decrypt(&b.e, f2, m, &res, pt, sizeof(pt), &pl), SC2_OK);
    CHECK(res.kind == SC2E_DM_IGNORED);
    sc2e_room_info_t ri;
    CHECK_RC(sc2e_room_info(&b.e, room, &ri), SC2_OK);
    CHECK(ri.known && ri.epoch == 1 && strcmp(ri.admin, a.info.device_id) == 0);
    /* Non-members of the room cannot read its messages. */
    CHECK_RC(sc2e_room_encrypt(&a.e, room, NULL, "{\"t\":\"text\"}", 12, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_room(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    sc2e_room_result_t rr;
    CHECK(sc2e_room_decrypt(&c.e, f2, m, &rr, pt, sizeof(pt), &pl) != SC2_OK);
    CHECK_RC(sc2e_room_decrypt(&b.e, f2, m, &rr, pt, sizeof(pt), &pl), SC2_OK);
    /* The admin's own echo decrypts without moving its sending chain. */
    CHECK_RC(sc2e_room_decrypt(&a.e, f2, m, &rr, pt, sizeof(pt), &pl), SC2_OK);
    CHECK_RC(sc2e_room_info(&a.e, room, &ri), SC2_OK);
    CHECK(ri.is_admin && ri.sent == 1);
    /* Forget removes state. */
    CHECK_RC(sc2e_room_forget(&b.e, room), SC2_OK);
    CHECK_RC(sc2e_room_decrypt(&b.e, f2, m, &rr, pt, sizeof(pt), &pl), SC2_ERR_NOT_FOUND);
    CHECK_RC(sc2e_forget_peer(&b.e, a.info.device_id), SC2_OK);
    CHECK_RC(sc2e_peer_info(&b.e, a.info.device_id, &pi), SC2_OK);
    CHECK(!pi.known);
    dev_close(&a);
    dev_close(&b);
    dev_close(&c);
}

static void test_post_rules(void)
{
    sc2e_test_host_t h = host();
    dev_t a;
    size_t n = 0, m = 0;
    sc2e_post_result_t pr;
    char body[SC2_POST_BODY_MAX + 1];
    dev_open(&a, &h);
    CHECK_RC(sc2e_post_sign(&a.e, "relay", "pub:general", "Ana", "", 0, 1, NULL, true, NULL, f1, sizeof(f1), &n),
             SC2_ERR_ARG);
    CHECK_RC(sc2e_post_sign(&a.e, "relay", "pub:general", "Ana", "", 0, 1, "00112233aabbccdd", true, NULL, f1,
                            sizeof(f1), &n),
             SC2_OK);
    CHECK_RC(sc2e_relay_post(f1, n, 7, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_post_verify("relay", f2, m, 1024, &pr, body, sizeof(body)), SC2_OK);
    CHECK(pr.valid && pr.deleted && strcmp(pr.ref, "00112233aabbccdd") == 0 && pr.seq == 7);
    CHECK_RC(sc2e_post_sign(&a.e, "relay", "pub:general", "Ana", "\xff", 1, 1, NULL, false, NULL, f1, sizeof(f1), &n),
             SC2_ERR_ARG);
    static char big[SC2_POST_BODY_MAX + 2];
    memset(big, 'x', sizeof(big) - 1);
    CHECK_RC(sc2e_post_sign(&a.e, "relay", "pub:general", "Ana", big, SC2_POST_BODY_MAX + 1, 1, NULL, false, NULL,
                            f1, sizeof(f1), &n),
             SC2_ERR_ARG);
    /* A 2000-byte body is fine on the wire but over a 1024-byte room limit. */
    CHECK_RC(sc2e_post_sign(&a.e, "relay", "pub:general", "Ana", big, 2000, 1, NULL, false, NULL, f1, sizeof(f1), &n),
             SC2_OK);
    CHECK_RC(sc2e_relay_post(f1, n, 8, f2, sizeof(f2), &m), SC2_OK);
    CHECK(sc2e_post_verify("relay", f2, m, 1024, &pr, body, sizeof(body)) != SC2_OK);
    CHECK(!pr.valid);
    dev_close(&a);
}

static void test_open_errors(void)
{
    sc2e_test_host_t h = host();
    sc2e_memstore_t *store = NULL;
    sc2e_platform_t pf;
    sc2e_t e;
    CHECK_RC(sc2e_memstore_create(&store, &h), SC2_OK);
    sc2e_memstore_platform(store, &pf);
    pf.rng.fn = sc2t_fail_fn;
    CHECK_RC(sc2e_open(&e, &pf), SC2_ERR_RNG);
    CHECK(!e.open);
    pf.rng.fn = sc2t_xs_fn;
    CHECK_RC(sc2e_open(&e, &pf), SC2_OK);
    char before[SC2_DEVICE_ID_HEX_LEN + 1];
    memcpy(before, e.self_hex, sizeof(before));
    sc2e_close(&e);
    /* A corrupted identity record is an error, never a silent new identity. */
    size_t len = 0;
    uint8_t *rec = sc2e_memstore_record(store, "identity", &len);
    CHECK(rec != NULL && len > 20);
    if (rec) {
        rec[20] ^= 1u;
        CHECK_RC(sc2e_open(&e, &pf), SC2_ERR_FORMAT);
        rec[20] ^= 1u;
    }
    CHECK_RC(sc2e_open(&e, &pf), SC2_OK);
    CHECK(!e.created && strcmp(before, e.self_hex) == 0);
    sc2e_close(&e);
    CHECK_RC(sc2e_open(&e, NULL), SC2_ERR_ARG);
    sc2e_memstore_destroy(store);
}

/* ---------------------------------------------------------------------- */
/* RAM budget: peak engine work memory (pf.alloc) per operation.           */
/* The platform's own load buffer (one SC2 slot file) comes on top.        */
/* ---------------------------------------------------------------------- */

static size_t w_live, w_peak;

static void *w_alloc(void *ctx, size_t len)
{
    (void)ctx;
    void *p = calloc(1, len + sizeof(size_t));
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, &len, sizeof(len));
    w_live += len;
    if (w_live > w_peak) {
        w_peak = w_live;
    }
    return (uint8_t *)p + sizeof(size_t);
}

static void w_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    if (ptr != NULL) {
        uint8_t *base = (uint8_t *)ptr - sizeof(size_t);
        size_t got;
        memcpy(&got, base, sizeof(got));
        CHECK(got == len);
        sc2_memzero(base, len + sizeof(size_t));
        free(base);
        w_live -= len;
    }
}

static void dev_open_tracked(dev_t *d, const sc2e_test_host_t *h)
{
    sc2e_platform_t pf;
    CHECK_RC(sc2e_memstore_create(&d->store, h), SC2_OK);
    sc2e_memstore_platform(d->store, &pf);
    pf.alloc = w_alloc;
    pf.release = w_release;
    CHECK_RC(sc2e_open(&d->e, &pf), SC2_OK);
    CHECK_RC(sc2e_identity(&d->e, &d->info), SC2_OK);
}

#define RAM_BUDGET_OPS 10
static const char *const RAM_OPS[RAM_BUDGET_OPS] = {
    "keys.publish", "add_bundle", "dm_encrypt (X3DH)", "dm_decrypt (new session)", "dm_decrypt (8 KiB)",
    "room_create", "room_share", "dm_decrypt (epoch key)", "room_encrypt", "room_decrypt",
};

static void test_ram_budget(void)
{
    sc2e_test_host_t h = host();
    dev_t a, b;
    size_t peak[RAM_BUDGET_OPS] = {0};
    size_t n = 0, m = 0, pl = 0;
    sc2e_dm_result_t res;
    sc2e_room_result_t rr;
    char dev[SC2_DEVICE_ID_HEX_LEN + 1];
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    static char big[SC2E_INNER_MAX + 1];
    dev_open_tracked(&a, &h);
    dev_open_tracked(&b, &h);
#define MEASURE(i, expr)        \
    do {                        \
        w_peak = w_live;        \
        expr;                   \
        peak[i] = w_peak - w_live; \
    } while (0)
    MEASURE(0, CHECK_RC(sc2e_keys_publish(&b.e, "k", 4, f1, sizeof(f1), &n), SC2_OK));
    CHECK_RC(sc2e_relay_bundle(f1, n, &b.info, f2, sizeof(f2), &m), SC2_OK);
    MEASURE(1, CHECK_RC(sc2e_add_bundle(&a.e, f2, m, dev), SC2_OK));
    CHECK_RC(sc2e_keys_publish(&a.e, "k", 0, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_bundle(f1, n, &a.info, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_add_bundle(&b.e, f2, m, dev), SC2_OK);
    MEASURE(2, CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, "{\"t\":\"text\"}", 12, f1, sizeof(f1), &n),
                        SC2_OK));
    CHECK_RC(sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    MEASURE(3, CHECK_RC(sc2e_dm_decrypt(&b.e, f2, m, &res, pt, sizeof(pt), &pl), SC2_OK));
    CHECK(res.new_session);
    /* The largest DM the protocol allows, decrypted in place. */
    const char *head = "{\"t\":\"text\",\"body\":\"";
    size_t hl = strlen(head);
    memcpy(big, head, hl);
    memset(big + hl, 'x', SC2E_INNER_MAX - hl - 2u);
    memcpy(big + SC2E_INNER_MAX - 2u, "\"}", 3);
    CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, big, SC2E_INNER_MAX, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    MEASURE(4, CHECK_RC(sc2e_dm_decrypt(&b.e, f2, m, &res, pt, sizeof(pt), &pl), SC2_OK));
    CHECK(pl == SC2E_INNER_MAX && memcmp(pt, big, SC2E_INNER_MAX) == 0);
    /* Alice loses her session and starts over with X3DH: Bob's stored
     * session fails first (in place, leaving the ciphertext intact) and the
     * fresh session then opens the same buffer. */
    CHECK_RC(sc2e_forget_peer(&a.e, b.info.device_id), SC2_OK);
    CHECK_RC(sc2e_keys_publish(&b.e, "k", 4, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_bundle(f1, n, &b.info, f2, sizeof(f2), &m), SC2_OK);
    CHECK_RC(sc2e_add_bundle(&a.e, f2, m, dev), SC2_OK);
    CHECK_RC(sc2e_dm_encrypt(&a.e, b.info.device_id, NULL, big, SC2E_INNER_MAX, f1, sizeof(f1), &n), SC2_OK);
    CHECK_RC(sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    memset(pt, 0, sizeof(pt));
    CHECK_RC(sc2e_dm_decrypt(&b.e, f2, m, &res, pt, sizeof(pt), &pl), SC2_OK);
    CHECK(res.new_session && pl == SC2E_INNER_MAX && memcmp(pt, big, SC2E_INNER_MAX) == 0);
    MEASURE(5, CHECK_RC(sc2e_room_create(&a.e, NULL, room, NULL), SC2_OK));
    MEASURE(6, CHECK_RC(sc2e_room_share(&a.e, room, b.info.device_id, NULL, f1, sizeof(f1), &n), SC2_OK));
    CHECK_RC(sc2e_relay_dm(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    MEASURE(7, CHECK_RC(sc2e_dm_decrypt(&b.e, f2, m, &res, pt, sizeof(pt), &pl), SC2_OK));
    CHECK(res.kind == SC2E_DM_EPOCH_KEY);
    MEASURE(8, CHECK_RC(sc2e_room_encrypt(&a.e, room, NULL, "{\"t\":\"text\"}", 12, f1, sizeof(f1), &n), SC2_OK));
    CHECK_RC(sc2e_relay_room(f1, n, ++seq, f2, sizeof(f2), &m), SC2_OK);
    MEASURE(9, CHECK_RC(sc2e_room_decrypt(&b.e, f2, m, &rr, pt, sizeof(pt), &pl), SC2_OK));
#undef MEASURE
    size_t worst = 0;
    printf("ram budget (SC2_LOW_RAM=%d): sizeof(sc2e_t)=%zu sizeof(sc2_ratchet_t)=%zu\n", SC2_LOW_RAM,
           sizeof(sc2e_t), sizeof(sc2_ratchet_t));
    for (size_t i = 0; i < RAM_BUDGET_OPS; i++) {
        printf("  %-26s %6zu bytes peak work memory\n", RAM_OPS[i], peak[i]);
        worst = peak[i] > worst ? peak[i] : worst;
    }
    printf("  %-26s %6zu bytes\n", "worst case", worst);
    CHECK(worst <= SC2E_WORK_BUDGET);
    CHECK(w_live == 0);
    dev_close(&a);
    dev_close(&b);
    CHECK(w_live == 0);
}

int main(void)
{
    test_selftest_and_bench();
    test_publish_and_bundle_rules();
    test_no_opk_bundle();
    test_dm_policies();
    test_post_rules();
    test_open_errors();
    test_ram_budget();
    CHECK(live_allocs == 0);
    return sc2t_report("sc2_engine_test");
}
