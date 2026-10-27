/*
 * SC2 portable core: relay-signed moderation events for public rooms
 * (room.mod).
 *
 * The signed bytes are a fixed 122-byte record, so a device rebuilds them
 * from the flat room.mod frame into a stack buffer and verifies them with
 * the relay key from welcome / the pin:
 *
 *   "SC2-MOD\x01"   8
 *   suite u8        1   (1)
 *   action u8       1   1 remove, 2 ban, 3 unban
 *   room           64   UTF-8 public room name, zero padded (1..63 bytes)
 *   seq u64         8   relay sequence number of the event in the room
 *   ts u64          8   relay time (unix seconds)
 *   ref             8   removed message ID (remove), else zero
 *   device         16   author (remove) or banned key (ban/unban)
 *   until u64       8   ban end (unix seconds), 0 = indefinite; 0 otherwise
 *
 * The relay ID is not repeated: the relay key itself identifies the relay.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_MOD_H
#define SC2_MOD_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_MOD_LEN 122u
#define SC2_MOD_ROOM_FIELD 64u
#define SC2_MOD_ROOM_MAX 63u

enum { SC2_MOD_REMOVE = 1, SC2_MOD_BAN = 2, SC2_MOD_UNBAN = 3 };

typedef struct {
    uint8_t action;
    char room[SC2_MOD_ROOM_FIELD];
    size_t room_len;
    uint64_t seq;
    uint64_t ts;
    uint8_t ref[SC2_MSG_ID_LEN];
    uint8_t device[SC2_DEVICE_ID_LEN];
    uint64_t until;
} sc2_mod_t;

/* "remove", "ban" or "unban", or NULL for an unknown action. */
const char *sc2_mod_action_name(uint8_t action);
/* 0 for an unknown name. */
uint8_t sc2_mod_action_parse(const char *name, size_t len);

/* Encode checks: known action, room 1..63 bytes without NUL, remove has a
 * non-zero ref, every action has a non-zero device, until only on ban. */
int sc2_mod_encode(uint8_t out[SC2_MOD_LEN], const sc2_mod_t *m);
int sc2_mod_decode(sc2_mod_t *m, const uint8_t rec[SC2_MOD_LEN]);
int sc2_mod_sign(uint8_t sig[SC2_SIG_LEN], const sc2_mod_t *m, const uint8_t relay_seed[SC2_KEY_LEN]);
int sc2_mod_verify(const uint8_t sig[SC2_SIG_LEN], const sc2_mod_t *m, const uint8_t relay_pub[SC2_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif
