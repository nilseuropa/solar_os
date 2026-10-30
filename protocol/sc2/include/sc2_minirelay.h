/*
 * SC2 portable core: a minimal relay for small devices.
 *
 * Sans-IO, like the client. The host owns the sockets and hands each line
 * to sc2_mr_line(); frames go out through cfg->send (the host appends
 * "\n"). The subset:
 *   - hello / welcome (the relay key signs the client's nonce) / auth
 *   - keys.publish and keys.claim (prekey bundles; one-time prekeys
 *     handed out once each)
 *   - dm.send with an offline mailbox (store and forward, ack, dedup)
 *   - one public room with a small history ring (room.join/leave/post,
 *     sync with sync.gap when history rolled over)
 *   - sync / ack / ping / presence.set / room.list
 * Private rooms and moderation answer 501 not_supported. No rate limits
 * (like a self-hosted relay's defaults). Every store is a fixed array with
 * a hard cap (SC2_MR_*), allocated once through cfg->alloc (PSRAM on the
 * device); a full store refuses (503 server_full / 507 inbox_full /
 * 413 too_large) and never grows.
 *
 * Connection slots: the host has cfg->sessions slots and nothing more, so
 * the relay bounds how long a client can hold one, like the full relay:
 *   - one session per device key: a new login closes the older session of
 *     the same device with 409 replaced;
 *   - pings are not activity: an authenticated session that sends nothing
 *     but pings for cfg->idle_s gets 408 idle (sc2_mr_tick()); a login that
 *     is not finished within SC2_MR_AUTH_S gets 408 idle as well;
 *   - when every slot is taken, sc2_mr_evict() closes the longest-idle
 *     authenticated session (idle at least SC2_MR_EVICT_MIN_S) with
 *     503 evicted so the host can give its slot to the new connection.
 * Each of these sends a final error frame to that session, releases it and
 * calls cfg->kick so the host closes the connection.
 *
 * Legacy v1: a first line that is a hello without "proto":"sc2" returns
 * SC2_MR_LEGACY when cfg->legacy_v1 is set (the host then runs its v1
 * server on that connection), else the relay answers 426 and closes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_MINIRELAY_H
#define SC2_MINIRELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sc2_common.h"
#include "sc2_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_MR_SESSIONS_MAX 8u
#define SC2_MR_DEVICES 16u        /* registered devices */
#define SC2_MR_OPKS 8u            /* one-time prekeys kept per device */
#define SC2_MR_INBOX 24u          /* mailbox entries, all devices */
#define SC2_MR_INBOX_PER_DEVICE 12u
#define SC2_MR_DM_MAX 3200u       /* raw "env" JSON of one DM (about 2 KB of text) */
#define SC2_MR_PUB_HISTORY 24u    /* public room posts kept */
#define SC2_MR_POST_MAX 1700u     /* raw "post" JSON */
#define SC2_MR_PUB_BODY_MAX 1024u /* advertised public body limit */
#define SC2_MR_KEYS_LOW 5u
#define SC2_MR_ADMIT_MAX 96u
#define SC2_MR_ROOM_DEFAULT "pub:general"
#define SC2_MR_IDLE_S 300u     /* default idle timeout (no frame other than ping) */
#define SC2_MR_AUTH_S 30u      /* hello and auth must be done within this */
#define SC2_MR_EVICT_MIN_S 60u /* sc2_mr_evict() only takes sessions idle this long */

enum { SC2_MR_OK = 0, SC2_MR_CLOSE = 1, SC2_MR_LEGACY = 2 };

typedef struct {
    const char *relay_id;     /* [A-Za-z0-9._:-], 1..64 */
    const uint8_t *seed;      /* the relay's long-term Ed25519 seed (32) */
    sc2_rng_t rng;            /* challenges */
    uint64_t (*now_s)(void *ctx);
    int (*send)(void *ctx, unsigned session, const char *line, size_t len);
    void *(*alloc)(void *ctx, size_t len); /* zeroed */
    void (*release)(void *ctx, void *ptr, size_t len);
    void *ctx;
    const char *admit;        /* admission code for new devices, NULL: open */
    const char *room;         /* public room, NULL: SC2_MR_ROOM_DEFAULT */
    bool legacy_v1;
    unsigned sessions;        /* 1..SC2_MR_SESSIONS_MAX */
    unsigned idle_s;          /* idle timeout in seconds, 0: SC2_MR_IDLE_S */
    /* Monotonic seconds for the slot timers; NULL: now_s is used. With
     * neither, sessions never time out and sc2_mr_evict() finds nobody. */
    uint64_t (*mono_s)(void *ctx);
    /* The relay closed this session on its own (replaced, idle, evicted):
     * the final error frame went out through send and the session is
     * released. The host closes the connection; it may call sc2_mr_close()
     * for it but must not call back into sc2_mr_line(), sc2_mr_open(),
     * sc2_mr_tick() or sc2_mr_evict() from here. NULL: the host notices
     * through sc2_mr_active() turning false, and the next sc2_mr_line()
     * for that session returns SC2_MR_CLOSE. */
    void (*kick)(void *ctx, unsigned session);
} sc2_mr_config_t;

typedef struct {
    unsigned devices, inbox, posts, sessions;
    uint64_t dm_total, posts_total, refused_total;
    uint64_t replaced_total, idle_total, evicted_total;
} sc2_mr_stats_t;

typedef struct sc2_mr sc2_mr_t;

/* Bytes the stores take (one allocation). */
size_t sc2_mr_size(void);
int sc2_mr_init(sc2_mr_t **out, const sc2_mr_config_t *cfg);
void sc2_mr_free(sc2_mr_t *mr);
/* The relay key (Ed25519 public). */
void sc2_mr_relay_key(const sc2_mr_t *mr, uint8_t out[SC2_KEY_LEN]);

/* Connection events for session index s (< cfg->sessions). */
void sc2_mr_open(sc2_mr_t *mr, unsigned s);
void sc2_mr_close(sc2_mr_t *mr, unsigned s);
/* One line without its newline. SC2_MR_CLOSE: close the connection (the
 * error frame was sent). SC2_MR_LEGACY: v1 hello, the session is released
 * and the host handles this line itself. */
int sc2_mr_line(sc2_mr_t *mr, unsigned s, const char *line, size_t len);
/* Close sessions past their deadline (408 idle, then cfg->kick). Call it
 * about once a second. Returns the number of sessions closed. */
unsigned sc2_mr_tick(sc2_mr_t *mr);
/* Every slot is taken and a new connection arrived: close the authenticated
 * session that has been idle longest, if idle for SC2_MR_EVICT_MIN_S or
 * more (503 evicted, then cfg->kick), and return its index for the new
 * connection (sc2_mr_open() it). -1: every session is in use; refuse the
 * new connection (503 server_full). */
int sc2_mr_evict(sc2_mr_t *mr);
/* True when the session speaks SC2 (after its hello). */
bool sc2_mr_active(const sc2_mr_t *mr, unsigned s);
void sc2_mr_stats(const sc2_mr_t *mr, sc2_mr_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
