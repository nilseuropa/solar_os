#pragma once
/*
 * sc2-sync relay choice (primary + fallbacks, one connection at a time).
 * Pure state machine, no I/O, so tests/host/sc2_relay_policy_test.c runs it
 * as it runs on the device.
 *
 *   home      the relay to stay on: the primary (0) unless it failed.
 *   failover  after SC2RP_FAILOVER_AFTER consecutive failures on home, the
 *             next relay becomes home (the caller's reconnect backoff paces
 *             the attempts).
 *   visits    every SC2RP_VISIT_MS while connected and idle: if home is a
 *             fallback, probe the primary (stay there when it works); if
 *             home is the primary, visit the fallbacks in turn so they hold
 *             our current prekey bundle and we drain their mailbox for peers
 *             that failed over. A visit lasts SC2RP_VISIT_STAY_MS after
 *             login; a failed visit returns home at once, without backoff.
 *             Until every relay has seen a login, visits come every
 *             SC2RP_FIRST_VISIT_MS.
 */
#include <stdbool.h>
#include <stdint.h>

#define SC2RP_RELAYS_MAX 8u
#define SC2RP_FAILOVER_AFTER 2u
#define SC2RP_VISIT_MS (15u * 60u * 1000u)
#define SC2RP_FIRST_VISIT_MS 60000u
#define SC2RP_VISIT_STAY_MS 30000u

typedef struct {
    uint8_t relay;  /* in use */
    uint8_t home;
    uint8_t relays; /* configured, 1..SC2RP_RELAYS_MAX */
    uint8_t fails;  /* consecutive failures on the relay in use */
    uint8_t seen;   /* bit mask: relays logged into since reset */
    uint8_t visit_next;
    bool visiting;
    uint64_t ready_at;
    uint64_t visit_at;
} sc2rp_t;

typedef enum {
    SC2RP_STAY = 0,   /* keep the relay in use */
    SC2RP_SWITCH,     /* relay changed: reconnect to p->relay (after the backoff) */
    SC2RP_SWITCH_NOW, /* relay changed: reconnect to p->relay at once */
} sc2rp_action_t;

static inline void sc2rp_reset(sc2rp_t *p, unsigned relays, uint64_t now_ms)
{
    p->relay = p->home = 0;
    p->relays = (uint8_t)(relays == 0 ? 1u : relays > SC2RP_RELAYS_MAX ? SC2RP_RELAYS_MAX : relays);
    p->fails = 0;
    p->seen = 0;
    p->visit_next = 0;
    p->visiting = false;
    p->ready_at = 0;
    p->visit_at = now_ms + SC2RP_FIRST_VISIT_MS;
}

static inline void sc2rp_go(sc2rp_t *p, uint8_t relay, bool visiting)
{
    p->relay = relay;
    p->visiting = visiting;
    p->fails = 0;
}

/* Login on the relay in use completed. */
static inline void sc2rp_ready(sc2rp_t *p, uint64_t now_ms)
{
    p->fails = 0;
    p->ready_at = now_ms;
    p->seen = (uint8_t)(p->seen | (1u << p->relay));
    if (p->visiting && p->relay < p->home) {
        p->home = p->relay; /* a more preferred relay is back: stay */
        p->visiting = false;
    }
}

/* Connecting, login or the session failed on the relay in use. */
static inline sc2rp_action_t sc2rp_failed(sc2rp_t *p)
{
    if (p->visiting) {
        sc2rp_go(p, p->home, false);
        return SC2RP_SWITCH_NOW;
    }
    if (p->relays > 1u && ++p->fails >= SC2RP_FAILOVER_AFTER) {
        p->home = (uint8_t)((p->relay + 1u) % p->relays);
        sc2rp_go(p, p->home, false);
        return SC2RP_SWITCH;
    }
    return SC2RP_STAY;
}

/* The relay ended a working session on purpose and said why (idle,
 * session lifetime, evicted, replaced). Not a failure: the failure count is
 * untouched and the relay in use stays, except that a visit ends and goes
 * home at once. */
static inline sc2rp_action_t sc2rp_closed(sc2rp_t *p)
{
    if (p->visiting) {
        sc2rp_go(p, p->home, false);
        return SC2RP_SWITCH_NOW;
    }
    return SC2RP_STAY;
}

/* Connected and logged in; idle: nothing waiting to be sent. */
static inline sc2rp_action_t sc2rp_tick(sc2rp_t *p, uint64_t now_ms, bool idle)
{
    if (p->visiting) {
        if (now_ms - p->ready_at < SC2RP_VISIT_STAY_MS) {
            return SC2RP_STAY;
        }
        sc2rp_go(p, p->home, false);
        return SC2RP_SWITCH_NOW;
    }
    if (p->relays < 2u || now_ms < p->visit_at || !idle) {
        return SC2RP_STAY;
    }
    uint8_t target = 0;
    if (p->home == 0) {
        p->visit_next = (uint8_t)(p->visit_next % (p->relays - 1u) + 1u);
        target = p->visit_next;
    }
    const unsigned all = (1u << p->relays) - 1u;
    p->visit_at = now_ms + (((p->seen | (1u << target)) & all) == all ? SC2RP_VISIT_MS : SC2RP_FIRST_VISIT_MS);
    sc2rp_go(p, target, true);
    return SC2RP_SWITCH_NOW;
}
