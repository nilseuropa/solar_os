/*
 * SC2 selftest and benchmark: known-answer tests for every primitive plus a
 * complete two-device session flow (X3DH, double ratchet, room epoch keys,
 * sender chains, public posts, persistence) run against an in-memory store.
 * Portable: the firmware runs it from `sc2 selftest`, host tests link it too.
 * SPDX-License-Identifier: MIT
 */
#ifndef SOLAR_OS_SC2_SELFTEST_H
#define SOLAR_OS_SC2_SELFTEST_H

#include "solar_os_sc2_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *(*alloc)(void *ctx, size_t len);              /* zeroed */
    void (*release)(void *ctx, void *ptr, size_t len);  /* wipe and free */
    uint64_t (*clock_us)(void *ctx);                    /* monotonic, bench only */
    sc2_rng_t rng;
    void *ctx;
} sc2e_test_host_t;

typedef void (*sc2e_report_fn)(void *ctx, const char *name, int rc);

typedef struct {
    const char *name;
    uint32_t iterations;
    uint64_t total_us;
    int rc;
} sc2e_bench_result_t;

typedef void (*sc2e_bench_fn)(void *ctx, const sc2e_bench_result_t *result);

/* In-memory record store implementing the sc2e_platform_t storage hooks. */
typedef struct sc2e_memstore sc2e_memstore_t;

int sc2e_memstore_create(sc2e_memstore_t **out, const sc2e_test_host_t *host);
void sc2e_memstore_destroy(sc2e_memstore_t *store);
/* Fill pf with this store, the host allocator and RNG, and a fixed clock. */
void sc2e_memstore_platform(sc2e_memstore_t *store, sc2e_platform_t *pf);
/* Direct access for tests: pointer to a stored record or NULL. */
uint8_t *sc2e_memstore_record(sc2e_memstore_t *store, const char *name, size_t *len);

/* Relay emulation used by the selftest and host tests. */
int sc2e_relay_dm(const char *send, size_t len, uint64_t seq, char *out, size_t cap, size_t *out_len);
int sc2e_relay_room(const char *send, size_t len, uint64_t seq, char *out, size_t cap, size_t *out_len);
int sc2e_relay_post(const char *send, size_t len, uint64_t seq, char *out, size_t cap, size_t *out_len);
/* Turn a keys.publish frame into the keys.bundle a claimer receives,
 * handing out the first OPK when present. */
int sc2e_relay_bundle(const char *publish, size_t len, const sc2e_identity_info_t *owner, char *out, size_t cap,
                      size_t *out_len);

/* Run every check; report() is called once per check. Returns SC2_OK when
 * all checks pass. */
int sc2e_selftest(const sc2e_test_host_t *host, sc2e_report_fn report, void *ctx, unsigned *passed,
                  unsigned *failed);

/* Time the primitives and a full DM/room round trip. iterations scales the
 * slow operations (minimum 1). */
int sc2e_bench(const sc2e_test_host_t *host, uint32_t iterations, sc2e_bench_fn report, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
