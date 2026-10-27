/* sc2-sync relay choice: failover, return to the primary, fallback visits,
 * sessions the relay closes on purpose. */
#include <assert.h>
#include <stdio.h>

#include "solar_os_sc2_relay_policy.h"

static void single_relay(void)
{
    sc2rp_t p;
    sc2rp_reset(&p, 1, 0);
    for (int i = 0; i < 10; i++) assert(sc2rp_failed(&p) == SC2RP_STAY && p.relay == 0);
    sc2rp_ready(&p, 1000);
    assert(sc2rp_tick(&p, 10u * SC2RP_VISIT_MS, true) == SC2RP_STAY && !p.visiting);
}

static void failover_and_back(void)
{
    sc2rp_t p;
    uint64_t t = 0;
    sc2rp_reset(&p, 3, t);
    /* primary down: one retry, then the first fallback */
    assert(sc2rp_failed(&p) == SC2RP_STAY && p.relay == 0);
    assert(sc2rp_failed(&p) == SC2RP_SWITCH && p.relay == 1 && p.home == 1 && !p.visiting);
    /* fallback 1 down too: on to 2, then wrap to the primary */
    assert(sc2rp_failed(&p) == SC2RP_STAY);
    assert(sc2rp_failed(&p) == SC2RP_SWITCH && p.relay == 2 && p.home == 2);
    sc2rp_ready(&p, t += 1000);
    assert(p.fails == 0 && p.seen == 4);
    /* busy: no visit even when due */
    assert(sc2rp_tick(&p, t += SC2RP_FIRST_VISIT_MS, false) == SC2RP_STAY);
    /* idle and due: probe the primary */
    assert(sc2rp_tick(&p, t, true) == SC2RP_SWITCH_NOW && p.relay == 0 && p.visiting && p.home == 2);
    /* the probe fails: straight home, no backoff */
    assert(sc2rp_failed(&p) == SC2RP_SWITCH_NOW && p.relay == 2 && !p.visiting);
    sc2rp_ready(&p, t += 1000);
    assert(sc2rp_tick(&p, t + 1, true) == SC2RP_STAY); /* not due yet */
    assert(sc2rp_tick(&p, t += SC2RP_FIRST_VISIT_MS, true) == SC2RP_SWITCH_NOW && p.relay == 0 && p.visiting);
    /* the primary is back: stay there, it is home again */
    sc2rp_ready(&p, t += 1000);
    assert(p.relay == 0 && p.home == 0 && !p.visiting);
    /* home on the primary again: the fallbacks get visits */
    assert(sc2rp_tick(&p, p.visit_at, true) == SC2RP_SWITCH_NOW && p.relay == 1 && p.visiting && p.home == 0);
}

static void visits_from_primary(void)
{
    sc2rp_t p;
    uint64_t t = 0;
    sc2rp_reset(&p, 3, t);
    sc2rp_ready(&p, t += 100);
    assert(sc2rp_tick(&p, t += 1000, true) == SC2RP_STAY);
    /* first visit soon (fallbacks need our keys), round robin 1, 2, 1, ... */
    assert(sc2rp_tick(&p, t = SC2RP_FIRST_VISIT_MS, true) == SC2RP_SWITCH_NOW && p.relay == 1 && p.visiting);
    sc2rp_ready(&p, t += 500);
    assert(p.home == 0 && p.visiting);
    assert(sc2rp_tick(&p, t + SC2RP_VISIT_STAY_MS - 1, true) == SC2RP_STAY && p.relay == 1);
    assert(sc2rp_tick(&p, t += SC2RP_VISIT_STAY_MS, true) == SC2RP_SWITCH_NOW && p.relay == 0 && !p.visiting);
    sc2rp_ready(&p, t += 500);
    /* relay 2 not seen yet: the short interval again */
    assert(sc2rp_tick(&p, p.visit_at, true) == SC2RP_SWITCH_NOW && p.relay == 2);
    t = p.visit_at - SC2RP_VISIT_MS; /* every relay seen now: the long interval */
    assert(p.visit_at >= SC2RP_VISIT_MS);
    sc2rp_ready(&p, t);
    assert(p.seen == 7);
    assert(sc2rp_tick(&p, t + SC2RP_VISIT_STAY_MS, true) == SC2RP_SWITCH_NOW && p.relay == 0);
    sc2rp_ready(&p, t + SC2RP_VISIT_STAY_MS + 10);
    assert(sc2rp_tick(&p, p.visit_at - 1, true) == SC2RP_STAY);
    assert(sc2rp_tick(&p, p.visit_at, true) == SC2RP_SWITCH_NOW && p.relay == 1);
    /* a failed visit to a fallback returns to the primary at once */
    assert(sc2rp_failed(&p) == SC2RP_SWITCH_NOW && p.relay == 0 && p.home == 0 && !p.visiting);
    /* a session drop on the visited relay counts like a failed visit */
    assert(sc2rp_tick(&p, p.visit_at, true) == SC2RP_SWITCH_NOW && p.relay == 2);
    sc2rp_ready(&p, p.visit_at);
    assert(sc2rp_failed(&p) == SC2RP_SWITCH_NOW && p.relay == 0);
}

/* idle, evicted, expired or replaced sessions: no failure count, no failover */
static void announced_closes(void)
{
    sc2rp_t p;
    uint64_t t = 0;
    sc2rp_reset(&p, 3, t);
    sc2rp_ready(&p, t += 100);
    assert(sc2rp_failed(&p) == SC2RP_STAY && p.fails == 1);
    for (int i = 0; i < 10; i++) {
        assert(sc2rp_closed(&p) == SC2RP_STAY && p.relay == 0 && p.home == 0 && p.fails == 1);
    }
    /* the next real failure still counts from where it was */
    assert(sc2rp_failed(&p) == SC2RP_SWITCH && p.relay == 1);
    sc2rp_ready(&p, t += 100);
    assert(p.fails == 0);
    assert(sc2rp_closed(&p) == SC2RP_STAY && p.relay == 1 && p.home == 1);
    /* on a visit it ends the visit: back home at once */
    assert(sc2rp_tick(&p, t += SC2RP_FIRST_VISIT_MS, true) == SC2RP_SWITCH_NOW && p.relay == 0 && p.visiting);
    sc2rp_ready(&p, t += 100);
    assert(p.home == 0 && !p.visiting); /* the primary is back */
    assert(sc2rp_tick(&p, p.visit_at, true) == SC2RP_SWITCH_NOW && p.relay != 0 && p.visiting);
    assert(sc2rp_closed(&p) == SC2RP_SWITCH_NOW && p.relay == 0 && !p.visiting && p.fails == 0);
}

static void clamp(void)
{
    sc2rp_t p;
    sc2rp_reset(&p, 0, 0);
    assert(p.relays == 1);
    sc2rp_reset(&p, 99, 0);
    assert(p.relays == SC2RP_RELAYS_MAX);
}

int main(void)
{
    single_relay();
    failover_and_back();
    visits_from_primary();
    announced_closes();
    clamp();
    puts("sc2_relay_policy_test: failover, return to primary, visits, announced closes OK");
    return 0;
}
