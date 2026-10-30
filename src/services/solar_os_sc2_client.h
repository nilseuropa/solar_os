/*
 * SC2 relay client: a portable, sans-IO state machine for one relay
 * connection (the device side of the sc2-sync job).
 *
 * The host owns the transport (TCP or TLS), the clock and the engine lock;
 * it feeds received bytes in with sc2c_feed(), calls sc2c_tick() regularly
 * and gets frames to write through host->send. Everything that reaches the
 * application (messages, delivery results, room state) comes out through
 * host->event, synchronously, while the client still owns the buffers.
 *
 * Covered: hello/auth, keys.publish (and top-up on keys.low), sync with
 * inbox/room cursors and ack, sealed DMs (keys.claim + X3DH + ratchet),
 * public rooms (join, signed posts out, verified posts in), private rooms
 * (create with a signed genesis commit and an invite, join with an invite
 * proof plus an end-to-end invite proof to the admin, authenticated membership
 * checkpoints, admin auto-add with commit + rotate + epoch key DMs to members,
 * sealed room messages), a persisted sealed outbox replayed after
 * reconnects (the relay dedups), ping keepalive and a backoff helper.
 *
 * Reconnects: the relay names the reason in an error frame before it closes
 * a session on purpose (408 idle, 503 reconnect, 503 evicted).
 * sc2c_close_reason() keeps that reason after the disconnect so the host
 * can tell such a close from a failure: it reconnects after a short pause
 * (sc2c_reconnect_ms()) without counting a failed attempt and without
 * failing over to another relay. 409 replaced (this device logged in on
 * another connection) backs off instead, so two connections with the same
 * device key do not take the slot from each other every second; it does
 * not fail over either. Every other drop goes through the exponential
 * backoff.
 *
 * RAM: three heap buffers per connection (receive line, transmit frame,
 * plaintext work), about 37 KiB with SC2_LOW_RAM and 41 KiB otherwise, plus
 * this struct (about 3 KiB). Engine work memory comes on top per call.
 *
 * Persistent records (through the engine platform store, per relay tag):
 *   sc2c/<tag>_o<slot>   sealed outbox frame awaiting the relay ack
 *   sc2c/<tag>_r<room>   private room: roster (admin), invite, admin key
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SOLAR_OS_SC2_CLIENT_H
#define SOLAR_OS_SC2_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sc2_commit.h"
#include "sc2_frame.h"
#include "sc2_relaykey.h"
#include "solar_os_sc2_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2C_OUTBOX_SLOTS 16u
#define SC2C_ROOMS_MAX 16u
#define SC2C_CLAIMS_MAX 4u
#define SC2C_TAG_MAX 8u
#define SC2C_KEY_MAX 64u /* room or peer key (fits a Messaging provider key) */
#define SC2C_NICK_MAX SC2_POST_NAME_MAX
#if SC2_LOW_RAM
#define SC2C_WORK_MAX 4608u /* Messaging body (4096) plus inner JSON */
#else
#define SC2C_WORK_MAX (SC2_DM_MAX_PLAINTEXT + 1u)
#endif
#define SC2C_TX_MAX (SC2E_FRAME_MAX + 128u)
#define SC2C_PING_MS 45000u
#define SC2C_DEAD_MS 120000u
#define SC2C_OPK_TARGET 16u
#define SC2C_SOFT_RECONNECT_MS 1000u /* pause after a soft close, plus up to 2 s jitter */

/* Return codes beyond the SC2_* ones. */
#define SC2C_AGAIN 1 /* not yet possible (offline or keys being fetched); retry later */

typedef enum {
    SC2C_DM = 1,
    SC2C_PRIVATE = 2,
    SC2C_PUBLIC = 3,
} sc2c_kind_t;

typedef enum {
    SC2C_OFFLINE = 0,
    SC2C_HELLO,  /* hello sent, waiting for welcome */
    SC2C_AUTH,   /* auth sent */
    SC2C_SYNCING,
    SC2C_READY,  /* initial sync done, live traffic flowing */
} sc2c_conn_state_t;

/* Why the relay ended the session, from the error frame it sent before
 * closing. Set only for an authenticated session; cleared by
 * sc2c_on_connected() and still readable after sc2c_on_disconnected(). */
typedef enum {
    SC2C_CLOSE_NONE = 0,  /* no reason given: the drop counts as a failure */
    SC2C_CLOSE_IDLE,      /* 408 idle: only pings for the relay's idle timeout */
    SC2C_CLOSE_RECONNECT, /* 503 reconnect: the relay's session lifetime ran out */
    SC2C_CLOSE_EVICTED,   /* 503 evicted: relay full, this idle session made room */
    SC2C_CLOSE_REPLACED,  /* 409 replaced: a newer login of this device key took over */
} sc2c_close_t;

typedef enum {
    SC2C_ROOM_IDLE = 0,
    SC2C_ROOM_JOINING,   /* room.join sent */
    SC2C_ROOM_REQUESTED, /* private: join request waits for an admin */
    SC2C_ROOM_WAITKEY,   /* private: member, epoch key not here yet */
    SC2C_ROOM_JOINED,
    SC2C_ROOM_CREATING,  /* private: genesis commit sent */
    SC2C_ROOM_FAILED,
    SC2C_ROOM_LEFT,
} sc2c_room_state_t;

typedef enum {
    SC2C_EV_STATE = 1, /* connection state changed (ev->code = sc2c_conn_state_t) */
    SC2C_EV_MESSAGE,   /* inbound text: kind, conv, from, name, body, seq, ts, msg_id */
    SC2C_EV_SENT,      /* outbox item acked by the relay: token, token_key, seq */
    SC2C_EV_FAILED,    /* outbox item refused: token, token_key, code, text */
    SC2C_EV_ROOM,      /* room state changed: conv, code = sc2c_room_state_t, text */
    SC2C_EV_CREATED,   /* private room created: conv = room, text = invite (sc2i:...) */
    SC2C_EV_CURSOR,    /* room cursor advanced: conv, seq (persist it if wanted) */
    SC2C_EV_NOTICE,    /* informational: code (relay error code or 0), text */
    SC2C_EV_RELAY_KEY, /* pinned relay key rotated (signed by the old key): persist c->relay_key */
} sc2c_event_type_t;

/* NOTICE code when the relay key does not match the pin (login refused). */
#define SC2C_NOTICE_RELAY_KEY 4901

typedef struct {
    sc2c_event_type_t type;
    sc2c_kind_t kind;
    const char *conv;   /* peer device hex (DM) or room key */
    const char *from;   /* sender device hex */
    const char *name;   /* public post display name ("" otherwise) */
    const char *body;   /* NUL terminated */
    size_t body_len;
    const char *msg_id; /* sender's message id ("" when absent) */
    uint64_t seq;       /* relay seq (inbox seq for DMs, room seq otherwise) */
    uint64_t ts;        /* sender timestamp, seconds (0 when absent) */
    uint32_t token;     /* SENT / FAILED */
    uint64_t token_key;
    int code;
    const char *text;
    bool replay;        /* came from sync rather than live */
} sc2c_event_t;

typedef struct {
    /* Write one frame (the client adds no newline; the host appends "\n").
     * Return 0 on success; a failure is treated as a lost connection. */
    int (*send)(void *ctx, const char *line, size_t len);
    void (*event)(void *ctx, const sc2c_event_t *ev);
    /* Engine access: the client holds it only for the duration of a call. */
    sc2e_t *(*engine_lock)(void *ctx);
    void (*engine_unlock)(void *ctx);
    /* Buffers (prefer PSRAM); release wipes. */
    void *(*alloc)(void *ctx, size_t len);
    void (*release)(void *ctx, void *ptr, size_t len);
    /* Monotonic milliseconds and wall-clock seconds (0 when unknown). */
    uint64_t (*mono_ms)(void *ctx);
    uint64_t (*wall_s)(void *ctx);
    void *ctx;
} sc2c_host_t;

typedef struct {
    char key[SC2C_KEY_MAX + 1];
    uint8_t kind;  /* SC2C_PRIVATE / SC2C_PUBLIC */
    uint8_t state; /* sc2c_room_state_t */
    bool want;     /* keep joined across reconnects */
    bool synced;   /* room part of the current session's sync */
    uint64_t cursor;
    uint32_t share_pending; /* admin: roster indices still owed the epoch key */
    uint8_t commit_op;      /* admin: commit in flight (0 none) */
    uint8_t commit_target[SC2_DEVICE_ID_LEN];
    uint8_t add_n;          /* admin: join requests waiting for a commit */
    uint8_t add_q[4][SC2_DEVICE_ID_LEN];
    uint64_t admission_ms; /* last end-to-end invite proof sent; 0 = retry now */
    uint64_t commit_hold; /* don't sync past a commit whose signer key is being fetched */
} sc2c_room_t;

typedef struct {
    bool used;
    bool inflight;
    uint8_t kind;
    uint32_t token;
    uint64_t token_key;
    char target[SC2C_KEY_MAX + 1];
} sc2c_slot_t;

typedef struct {
    char device[SC2_DEVICE_ID_HEX_LEN + 1];
    uint64_t asked_ms;
} sc2c_claim_t;

/* Persistent private room record (loaded into one scratch copy at a time). */
typedef struct {
    uint8_t flags; /* SC2C_RF_* */
    uint8_t secret[SC2_INVITE_SECRET_LEN];
    uint8_t admin_id[SC2_DEVICE_ID_LEN];
    uint8_t admin_ik[SC2_KEY_LEN];
    sc2_roster_t roster; /* authenticated roster and commit head */
    char title[SC2E_ROOM_TITLE_MAX + 1];
} sc2c_roomrec_t;

#define SC2C_RF_ADMIN 0x01u
#define SC2C_RF_INVITE 0x02u /* secret/admin fields valid */

typedef struct {
    sc2c_host_t host;
    char tag[SC2C_TAG_MAX + 1];
    char nick[SC2C_NICK_MAX + 1];
    char admit[65];
    char expect_relay[SC2_RELAY_ID_MAX + 1]; /* empty: accept any */
    bool has_relay_key;                      /* pinned: welcome must prove this key */
    uint8_t relay_key[SC2_KEY_LEN];
    uint8_t hello_nonce[SC2_HELLO_NONCE_LEN];
    char relay_id[SC2_RELAY_ID_MAX + 1];
    char invite_url[SC2_INVITE_URL_MAX + 1];
    char self_hex[SC2_DEVICE_ID_HEX_LEN + 1];
    uint8_t self_id[SC2_DEVICE_ID_LEN];
    uint8_t state;   /* sc2c_conn_state_t */
    uint8_t closed;  /* sc2c_close_t of the current or last session */
    uint8_t replaced; /* sessions in a row that ended with 409 replaced */
    bool auto_admit; /* admin: add only after an authenticated DM proves the invite secret */
    bool no_opks;    /* keys.publish without one-time prekeys */
    bool sync_more;
    uint32_t pub_max; /* public body limit from room.state (default 1024) */
    uint64_t inbox_done; /* highest inbox seq processed in this session */
    uint64_t dm_hold;    /* first inbox seq held back until its sender's keys arrive (0 none) */
    uint64_t dm_hold_ms;
    uint64_t last_rx_ms;
    uint64_t last_tx_ms;
    uint64_t retry_ms;   /* next outbox/share retry */
    sc2_lines_t *rx;
    char *tx;
    char *work;
    sc2c_room_t rooms[SC2C_ROOMS_MAX];
    sc2c_slot_t slots[SC2C_OUTBOX_SLOTS];
    sc2c_claim_t claims[SC2C_CLAIMS_MAX];
    sc2c_roomrec_t rec; /* scratch */
} sc2c_t;

typedef struct {
    const char *tag;          /* store prefix for this relay: [a-z0-9_], 1..8 chars */
    const char *nick;         /* public post display name */
    const char *admit;        /* admission code or NULL */
    const char *expect_relay; /* relay id to insist on, or NULL */
    const char *invite_url;   /* relay URL written into invites, or NULL */
    bool auto_admit;
    bool no_opks;             /* publish the signed prekey only (fallback relays) */
    const uint8_t *relay_key; /* pinned relay key (32 bytes) or NULL */
} sc2c_config_t;

/* Allocate buffers and load the persisted outbox index. */
int sc2c_init(sc2c_t *c, const sc2c_host_t *host, const sc2c_config_t *cfg);
void sc2c_free(sc2c_t *c);

/* Transport events. on_connected sends hello and clears the close reason;
 * on_disconnected resets the session (outbox items stay persisted and are
 * replayed on the next login) and keeps the close reason. */
int sc2c_on_connected(sc2c_t *c);
void sc2c_on_disconnected(sc2c_t *c);
/* Received bytes (any split). Returns SC2_OK, or an error when the
 * connection should be dropped (auth refused, relay mismatch, send failed).
 * After SC2_OK, check sc2c_close_reason(): when it is set the relay is
 * about to close the session and the host may drop it right away. */
int sc2c_feed(sc2c_t *c, const uint8_t *data, size_t len);
/* Keepalive and retries. Returns SC2_ERR_STATE when the relay went silent
 * for SC2C_DEAD_MS (drop the connection). */
int sc2c_tick(sc2c_t *c);
bool sc2c_ready(const sc2c_t *c);

/* Rooms. Public keys are "pub:name", private ones 32 hex. cursor is the
 * last room seq already seen (0 for none). */
int sc2c_room_add(sc2c_t *c, const char *key, uint64_t cursor);
int sc2c_room_remove(sc2c_t *c, const char *key);
const sc2c_room_t *sc2c_room_get(const sc2c_t *c, const char *key);
/* Create a private room (needs READY). The room key and invite text arrive
 * in SC2C_EV_CREATED. room_out gets the new key right away. */
int sc2c_room_create(sc2c_t *c, char room_out[SC2_DEVICE_ID_HEX_LEN + 1]);
int sc2c_room_create_named(sc2c_t *c, const char *title, char room_out[SC2_DEVICE_ID_HEX_LEN + 1]);
/* Copies the locally persisted admin-authenticated title; empty for legacy rooms. */
int sc2c_room_get_title(sc2c_t *c, const char *room, char *out, size_t cap);
/* Accept an invite text "sc2i:..." and start joining its room. */
int sc2c_room_join_invite(sc2c_t *c, const char *text, char room_out[SC2_DEVICE_ID_HEX_LEN + 1]);
/* Re-create the invite text of a room this device administers. */
int sc2c_room_invite_text(sc2c_t *c, const char *room, char *out, size_t cap);

/* Queue text to a device (DM), private room or public room. The frame is
 * sealed, persisted and transmitted; SENT/FAILED report the outcome with
 * token/token_key. Returns SC2_OK when sealed and queued, SC2C_AGAIN when
 * the caller should keep the request and retry later (offline, peer keys
 * being fetched, outbox full), or an error for a request that can never
 * be delivered. A request whose token_key is already in the outbox is
 * accepted again without sealing a second copy. */
int sc2c_submit(sc2c_t *c, sc2c_kind_t kind, const char *target, const char *body, size_t body_len, uint32_t token,
                uint64_t token_key);
size_t sc2c_outbox_count(const sc2c_t *c);

/* Exponential reconnect delay with jitter: 1 s, 2 s, 4 s ... capped at
 * 60 s. *attempt counts consecutive failures; reset it after a login. */
uint32_t sc2c_backoff_ms(uint32_t *attempt, uint32_t random);

/* Reason the relay gave for ending the session (SC2C_CLOSE_NONE if none). */
sc2c_close_t sc2c_close_reason(const sc2c_t *c);
/* True for reasons that are part of normal operation (idle, reconnect,
 * evicted): reconnect soon, do not count a failure, do not fail over. */
bool sc2c_close_soft(sc2c_close_t why);
const char *sc2c_close_name(sc2c_close_t why);
/* Delay before the next connection attempt after a session ended with
 * sc2c_close_reason(c). Soft closes: SC2C_SOFT_RECONNECT_MS plus jitter,
 * *attempt untouched. Replaced: the backoff, starting at 4 s and growing
 * with every replaced session in a row even though each login in between
 * resets *attempt. Otherwise the same as sc2c_backoff_ms(). */
uint32_t sc2c_reconnect_ms(const sc2c_t *c, uint32_t *attempt, uint32_t random);

const char *sc2c_state_name(int state);

#ifdef __cplusplus
}
#endif

#endif
