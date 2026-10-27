/*
 * SC2 portable core: relay-protocol frames.
 *
 * Line assembly (<= 16 KiB per line), the hello / auth handshake frames,
 * sealed DM and room envelopes, and signed public-room posts with their
 * binary signed bytes.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_FRAME_H
#define SC2_FRAME_H

#include <stdbool.h>
#include "sc2_commit.h"
#include "sc2_json.h"
#include "sc2_mod.h"
#include "sc2_ratchet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_LINE_MAX 16384u
#define SC2_RID_MAX 16u
#define SC2_TYPE_MAX 32u
#define SC2_ROOM_NAME_MAX 64u   /* "pub:general" style public room names */
#define SC2_POST_NAME_MAX 64u
#define SC2_POST_BODY_MAX 4096u /* wire maximum; public rooms advertise 1024 */
#define SC2_POST_SIGNED_MAX (8u + 1u + 1u + SC2_RELAY_ID_MAX + 1u + SC2_ROOM_NAME_MAX + 32u + 8u + 8u + 1u + \
                             1u + SC2_POST_NAME_MAX + 2u + SC2_POST_BODY_MAX + 8u)

/* ---- Line assembler ---- */
typedef struct {
    char buf[SC2_LINE_MAX + 2];
    size_t len;
    bool discarding;
} sc2_lines_t;

void sc2_lines_init(sc2_lines_t *l);
/* Consume bytes from data. Returns 1 with a complete line (without "\n"
 * or a trailing "\r", NUL-terminated, valid until the next call), 0 when
 * all input was consumed without completing a line, or SC2_ERR_BOUNDS once
 * for a line longer than SC2_LINE_MAX (the rest of it is discarded).
 * *consumed tells how much input was used; call again with the remainder.
 * Empty lines are skipped. */
int sc2_lines_feed(sc2_lines_t *l, const uint8_t *data, size_t len, size_t *consumed, const char **line,
                   size_t *line_len);

/* ---- Generic ---- */
/* Parse a line and fetch "type" (and "rid" if rid != NULL; empty when absent). */
int sc2_frame_peek(const char *line, size_t len, sc2_json_val_t *root, char type[SC2_TYPE_MAX + 1],
                   char rid[SC2_RID_MAX + 1]);

/* ---- Handshake ---- */
/* nonce (16 bytes, may be NULL) asks the relay to sign the welcome with its
 * key (sc2_relaykey.h). */
int sc2_frame_hello(char *out, size_t cap, size_t *out_len, const char *client, const char *admit,
                    const uint8_t *nonce);

typedef struct {
    uint32_t suite;
    char relay_id[SC2_RELAY_ID_MAX + 1];
    uint8_t relay_key[SC2_KEY_LEN];
    bool has_relay_key;
    uint8_t challenge[SC2_AUTH_CHALLENGE_LEN];
    bool legacy_v1;
    uint32_t limit_line;
    uint32_t limit_msg;
    bool has_relay_sig;                 /* "relay_sig": answer to the hello nonce */
    uint8_t relay_sig[SC2_SIG_LEN];
    bool has_rotation;                  /* "rotation": {"from", "sig"} */
    uint8_t rotation_from[SC2_KEY_LEN];
    uint8_t rotation_sig[SC2_SIG_LEN];
} sc2_welcome_t;

int sc2_frame_welcome_decode(const char *line, size_t len, sc2_welcome_t *out);
int sc2_frame_auth(char *out, size_t cap, size_t *out_len, const char *rid, const uint8_t ik_pub[SC2_KEY_LEN],
                   const uint8_t sig[SC2_SIG_LEN], bool resume);

/* ---- Sealed DM ---- */
typedef struct {
    uint32_t suite;
    uint8_t from[SC2_DEVICE_ID_LEN];
    sc2_dm_header_t hdr;
    uint8_t nonce[SC2_NONCE_LEN];
    const uint8_t *ct; /* encode: input; decode: points into the caller's buffer */
    size_t ct_len;
} sc2_dm_env_t;

/* {"type":"dm.send","rid":..,"to":..,"env":{...}}; rid may be NULL. */
int sc2_frame_dm_send(char *out, size_t cap, size_t *out_len, const char *rid,
                      const uint8_t to[SC2_DEVICE_ID_LEN], const sc2_dm_env_t *env);
/* Decode an "env" object (from dm.send or dm.msg) into env, ct into ct_buf. */
int sc2_dm_env_decode(const sc2_json_val_t *env_obj, sc2_dm_env_t *env, uint8_t *ct_buf, size_t ct_cap);
/* {"type":"dm.msg","seq":"..","env":{...}} */
int sc2_frame_dm_msg_decode(const char *line, size_t len, uint64_t *seq, sc2_dm_env_t *env, uint8_t *ct_buf,
                            size_t ct_cap);

/* ---- Sealed room message ---- */
typedef struct {
    uint32_t suite;
    uint32_t epoch;
    uint8_t from[SC2_DEVICE_ID_LEN];
    uint32_t n;
    uint8_t nonce[SC2_NONCE_LEN];
    const uint8_t *ct;
    size_t ct_len;
    uint8_t sig[SC2_SIG_LEN];
} sc2_room_env_t;

int sc2_frame_room_send(char *out, size_t cap, size_t *out_len, const char *rid,
                        const uint8_t room[SC2_ROOM_ID_LEN], const sc2_room_env_t *env);
int sc2_room_env_decode(const sc2_json_val_t *env_obj, sc2_room_env_t *env, uint8_t *ct_buf, size_t ct_cap);
/* {"type":"room.msg","room":..,"seq":"..","env":{...}} */
int sc2_frame_room_msg_decode(const char *line, size_t len, uint8_t room[SC2_ROOM_ID_LEN], uint64_t *seq,
                              sc2_room_env_t *env, uint8_t *ct_buf, size_t ct_cap);

/* ---- Public post ---- */
#define SC2_POST_T_TEXT 1u
#define SC2_POST_T_DEL 2u

typedef struct {
    uint32_t suite;
    uint8_t t;
    uint8_t from[SC2_DEVICE_ID_LEN];
    uint8_t ik[SC2_KEY_LEN];
    uint8_t id[SC2_MSG_ID_LEN];
    uint64_t ts;
    char name[SC2_POST_NAME_MAX + 1];
    size_t name_len;
    const char *body; /* encode: input; decode: points into the caller's buffer */
    size_t body_len;
    bool has_ref;
    uint8_t ref[SC2_MSG_ID_LEN];
    uint8_t sig[SC2_SIG_LEN];
} sc2_post_t;

/* Binary signed bytes. */
int sc2_post_signed_bytes(uint8_t *out, size_t cap, size_t *out_len, const char *relay_id, const char *room,
                          const sc2_post_t *post);
/* Sets post->ik, post->from and post->sig from the identity seed. */
int sc2_post_sign(sc2_post_t *post, const char *relay_id, const char *room, const uint8_t ik_seed[SC2_KEY_LEN]);
/* Checks a post the way clients and relays do: suite and type, length limits
 * (max_body: the room's body limit), valid UTF-8, from being the device ID of
 * ik, and the signature. */
int sc2_post_verify(const sc2_post_t *post, const char *relay_id, const char *room, size_t max_body);

int sc2_frame_room_post(char *out, size_t cap, size_t *out_len, const char *rid, const char *room,
                        const sc2_post_t *post);
int sc2_post_decode(const sc2_json_val_t *post_obj, sc2_post_t *post, char *body_buf, size_t body_cap);
/* {"type":"room.pub","room":..,"seq":"..","post":{...}} */
int sc2_frame_room_pub_decode(const char *line, size_t len, char room[SC2_ROOM_NAME_MAX + 1], uint64_t *seq,
                              sc2_post_t *post, char *body_buf, size_t body_cap);

/* ---- Private-room membership and invites ---- */
/* {"type":"room.create","rid":..,"room":"<hex>","kind":"private","commit":"<b64 130>","sig":"<b64 64>"}
 * with the genesis commit record. */
int sc2_frame_room_create_private(char *out, size_t cap, size_t *out_len, const char *rid,
                                  const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                                  const uint8_t sig[SC2_SIG_LEN]);
/* C->R {"type":"room.commit","rid":..,"room":..,"commit":..,"sig":..} */
int sc2_frame_room_commit(char *out, size_t cap, size_t *out_len, const char *rid,
                          const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                          const uint8_t sig[SC2_SIG_LEN]);
/* R->C {"type":"room.commit","room":..,"seq":"..","commit":..,"sig":..}. The
 * record is only decoded into rec; the caller checks it (sc2_commit_decode,
 * sc2_commit_verify, sc2_roster_apply). The record must name the frame's room. */
int sc2_frame_room_commit_decode(const char *line, size_t len, uint8_t room[SC2_ROOM_ID_LEN], uint64_t *seq,
                                 uint8_t rec[SC2_COMMIT_LEN], uint8_t sig[SC2_SIG_LEN]);
/* {"type":"room.invite","rid":..,"room":..,"key":"<b64 32>"[,"revoke":true]} (admins only) */
int sc2_frame_room_invite(char *out, size_t cap, size_t *out_len, const char *rid,
                          const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t key[SC2_INVITE_KEY_LEN], bool revoke);
/* {"type":"room.join","rid":..,"room":"<hex>"[,"invite":"<b64 32 proof>"]}; proof may be NULL. */
int sc2_frame_room_join_private(char *out, size_t cap, size_t *out_len, const char *rid,
                                const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t proof[32]);

/* ---- Relay-signed moderation ---- */
/* {"type":"room.mod","room":..,"seq":"..","action":"remove|ban|unban","ts":..,
 *  ["ref":"<16 hex>",]"device":"<32 hex>",["until":<u64>|null,]"by":"relay","sig":..}
 * ref only on remove (required), until only on ban (null or absent = 0 =
 * indefinite). Verify with sc2_mod_verify and the relay key. */
int sc2_frame_room_mod_decode(const char *line, size_t len, sc2_mod_t *mod, uint8_t sig[SC2_SIG_LEN]);
/* Relay side / tests: write the frame for mod and its signature. */
int sc2_frame_room_mod(char *out, size_t cap, size_t *out_len, const sc2_mod_t *mod, const uint8_t sig[SC2_SIG_LEN]);

#ifdef __cplusplus
}
#endif

#endif
