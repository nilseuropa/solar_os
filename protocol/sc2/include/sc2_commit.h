/*
 * SC2 portable core: private-room membership commits, rosters and invites.
 *
 * Everything here is fixed-size so a device can build, verify and apply a
 * commit with small stack buffers and no JSON nesting:
 *
 *   commit record (SC2_COMMIT_LEN = 130 bytes, signed as-is by an admin IK)
 *     "SC2-COMMIT\x01"   11
 *     suite u8            1   (1)
 *     room               16
 *     epoch u32           4   new epoch: 1 for genesis, else current + 1
 *     prev               32   commit ID of the previous commit (zero for genesis)
 *     by                 16   signing admin's device ID
 *     op u8               1   1 create, 2 add, 3 remove, 4 promote, 5 demote, 6 rotate
 *     target             16   device the op applies to (zero for rotate; = by for create)
 *     count u8            1   members after the op (1..32)
 *     roster_hash        32   SHA-256 over the sorted roster entries after the op,
 *                             each entry = device:16 || role:u8 (1 admin, 0 member)
 *
 *   commit ID = SHA-256(record); sig = Ed25519(IK_by, record).
 *
 * One change per commit. Several changes are several consecutive commits;
 * the admin distributes the epoch key only for the last one.
 *
 * Invites: invite_key = HMAC-SHA-256(secret:16, "SC2-INVITE\x01" || room:16),
 * registered with the relay by an admin; join proof =
 * HMAC-SHA-256(invite_key, "SC2-JOIN\x01" || joiner:16). The out-of-band
 * invite code is a fixed binary record (see sc2_invite_encode).
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_COMMIT_H
#define SC2_COMMIT_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_COMMIT_LEN 130u
#define SC2_COMMIT_ID_LEN 32u
#define SC2_ROOM_MEMBERS_MAX 32u
/* A low-memory build may cap the rosters it can track (>= 2). Commits
 * that would grow a room beyond the cap are refused with SC2_ERR_BOUNDS. */
#ifndef SC2_ROSTER_CAP
#define SC2_ROSTER_CAP SC2_ROOM_MEMBERS_MAX
#endif

enum {
    SC2_COMMIT_CREATE = 1,
    SC2_COMMIT_ADD = 2,
    SC2_COMMIT_REMOVE = 3,
    SC2_COMMIT_PROMOTE = 4,
    SC2_COMMIT_DEMOTE = 5,
    SC2_COMMIT_ROTATE = 6
};

#define SC2_ROLE_MEMBER 0u
#define SC2_ROLE_ADMIN 1u

typedef struct {
    uint8_t room[SC2_ROOM_ID_LEN];
    uint32_t epoch;
    uint8_t prev[SC2_COMMIT_ID_LEN];
    uint8_t by[SC2_DEVICE_ID_LEN];
    uint8_t op;
    uint8_t target[SC2_DEVICE_ID_LEN];
    uint8_t count;
    uint8_t roster_hash[32];
} sc2_commit_t;

typedef struct {
    uint8_t id[SC2_DEVICE_ID_LEN];
    uint8_t role;
} sc2_member_t;

/* Signed membership as a client tracks it. Entries are kept sorted by ID. */
typedef struct {
    uint8_t room[SC2_ROOM_ID_LEN];
    uint32_t epoch; /* 0 = no genesis yet */
    uint8_t head[SC2_COMMIT_ID_LEN];
    uint8_t count;
    sc2_member_t m[SC2_ROSTER_CAP];
} sc2_roster_t;

/* Record encoding. decode checks the label, suite, op and the op's shape
 * (genesis: epoch 1, zero prev, target == by, count 1; rotate: zero target). */
int sc2_commit_encode(uint8_t out[SC2_COMMIT_LEN], const sc2_commit_t *c);
int sc2_commit_decode(sc2_commit_t *c, const uint8_t rec[SC2_COMMIT_LEN]);
int sc2_commit_id(uint8_t out[SC2_COMMIT_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN]);
int sc2_commit_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t rec[SC2_COMMIT_LEN], const uint8_t ik_seed[SC2_KEY_LEN]);
/* Strict Ed25519 over the record, and device_id(ik_pub) must equal "by". */
int sc2_commit_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                      const uint8_t ik_pub[SC2_KEY_LEN]);

void sc2_roster_init(sc2_roster_t *r, const uint8_t room[SC2_ROOM_ID_LEN]);
/* Index of id, or -1. */
int sc2_roster_find(const sc2_roster_t *r, const uint8_t id[SC2_DEVICE_ID_LEN]);
int sc2_roster_is_admin(const sc2_roster_t *r, const uint8_t id[SC2_DEVICE_ID_LEN]);
/* SHA-256 over the sorted entries (device || role), streamed. */
int sc2_roster_hash(const sc2_roster_t *r, uint8_t out[32]);

/* Admin side: fill c with the next commit on top of r (r is not changed).
 * target is ignored for rotate and must be by for create. */
int sc2_commit_make(sc2_commit_t *c, const sc2_roster_t *r, const uint8_t by[SC2_DEVICE_ID_LEN], uint8_t op,
                    const uint8_t target[SC2_DEVICE_ID_LEN]);
/* Check a commit against r without changing it (SC2_OK if it would apply).
 * Rules: room matches; epoch is r->epoch + 1 and prev is r->head; by is an
 * admin (the creator for create, on an empty roster); add needs a
 * non-member, remove/promote/demote a member (promote a non-admin, demote
 * an admin), and the last admin can be neither removed nor demoted; count
 * and roster_hash must match the result. Rule failures return
 * SC2_ERR_STATE, a roster over SC2_ROSTER_CAP returns SC2_ERR_BOUNDS. */
int sc2_roster_check(const sc2_roster_t *r, const sc2_commit_t *c);
/* Decode rec, check it and apply it in place; head becomes its commit ID.
 * The caller verifies the signature first (sc2_commit_verify with the IK of
 * the commit's "by"). On any error r is left exactly as it was. */
int sc2_roster_apply(sc2_roster_t *r, const uint8_t rec[SC2_COMMIT_LEN]);

/* ---- Invites ---- */
#define SC2_INVITE_SECRET_LEN 16u
#define SC2_INVITE_KEY_LEN 32u
#define SC2_INVITE_URL_MAX 200u
#define SC2_INVITE_FIXED_LEN 86u
#define SC2_INVITE_CODE_MAX (SC2_INVITE_FIXED_LEN + SC2_INVITE_URL_MAX)

typedef struct {
    uint8_t room[SC2_ROOM_ID_LEN];
    uint8_t secret[SC2_INVITE_SECRET_LEN];
    uint8_t admin_id[SC2_DEVICE_ID_LEN];
    uint8_t admin_ik[SC2_KEY_LEN];
    size_t url_len;
    char url[SC2_INVITE_URL_MAX + 1];
} sc2_invite_t;

int sc2_invite_key(uint8_t out[SC2_INVITE_KEY_LEN], const uint8_t secret[SC2_INVITE_SECRET_LEN],
                   const uint8_t room[SC2_ROOM_ID_LEN]);
int sc2_invite_proof(uint8_t out[32], const uint8_t invite_key[SC2_INVITE_KEY_LEN],
                     const uint8_t joiner[SC2_DEVICE_ID_LEN]);
/* Invite code record:
 *   "SC2I" | version u8 (1) | room:16 | secret:16 | admin_id:16 | admin_ik:32 |
 *   url_len u8 (1..200) | url (printable ASCII, chat:// or chats://)
 * decode checks admin_id == device_id(admin_ik). The text form is
 * "sc2i:" + standard base64 of the record. */
int sc2_invite_encode(uint8_t *out, size_t cap, size_t *out_len, const sc2_invite_t *inv);
int sc2_invite_decode(sc2_invite_t *inv, const uint8_t *in, size_t len);
#define SC2_INVITE_TEXT_PREFIX "sc2i:"
/* Text form length including the NUL terminator. */
#define SC2_INVITE_TEXT_MAX (5u + ((SC2_INVITE_CODE_MAX + 2u) / 3u) * 4u + 1u)
int sc2_invite_to_text(char *out, size_t cap, size_t *out_len, const sc2_invite_t *inv);
int sc2_invite_from_text(sc2_invite_t *inv, const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif
