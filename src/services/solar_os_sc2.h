#pragma once
/*
 * SolarOS SC2 (SolarOS Chat v2) security service.
 *
 * Owns the device's SC2 identity, prekeys, peer bundles, pairwise ratchet
 * sessions and room sender chains. Secrets are stored on internal flash under
 * /.sc2/ (or /flash/.sc2/ on SD-capable boards) and never leave this service:
 * callers pass plaintext and wire frames in and get frames, plaintext and
 * public metadata out. Every call is serialised by a service mutex; work
 * buffers (ratchet states, ciphertext, frames) come from PSRAM when present.
 *
 * The service starts lazily on first use, so the identity is generated the
 * first time `sc2 id`, `solaros.sc2` or a provider needs it, not at boot.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_sc2_engine.h"
#include "solar_os_sc2_selftest.h"

#define SOLAR_OS_SC2_STORE_DIR ".sc2"
#define SOLAR_OS_SC2_FRAME_MAX SC2E_FRAME_MAX
#define SOLAR_OS_SC2_INNER_MAX SC2E_INNER_MAX
/* SC2 core errors are reported as SOLAR_OS_SC2_ERR_BASE - rc (rc < 0). */
#define SOLAR_OS_SC2_ERR_BASE 0x7C200
/* 2024-01-01: an earlier wall clock means "not set" (timestamps then 0). */
#define SOLAR_OS_SC2_MIN_WALL_CLOCK 1704067200u
#define SOLAR_OS_SC2_ESP_ERR(rc) ((esp_err_t)(SOLAR_OS_SC2_ERR_BASE - (rc)))

typedef struct {
    bool ready;
    bool created;        /* identity generated during this boot */
    bool work_in_psram;  /* work buffers are allocated from external RAM */
    char device_id[SC2_DEVICE_ID_HEX_LEN + 1];
    char fingerprint[SC2_SAFETY_SHORT_LEN + 1];
    uint32_t spk_id;
    uint64_t spk_created;
    uint32_t opk_count;
    size_t peers;
    size_t sessions;
    size_t rooms;
    const char *core_version;
    const char *hash_backend;
    char store_path[SC2E_NAME_MAX + 64];
} solar_os_sc2_status_t;

esp_err_t solar_os_sc2_init(void);
/* Direct engine access for the relay client (sc2-sync). Returns NULL when the
 * service cannot start; otherwise unlock exactly once. Keep calls short. */
sc2e_t *solar_os_sc2_engine_lock(void);
void solar_os_sc2_engine_unlock(void);
/* Text for SC2 and ESP errors returned by this service. */
const char *solar_os_sc2_err_text(esp_err_t err);

esp_err_t solar_os_sc2_get_status(solar_os_sc2_status_t *status);
esp_err_t solar_os_sc2_identity(sc2e_identity_info_t *info);
esp_err_t solar_os_sc2_peer_info(const char *peer, sc2e_peer_info_t *info);
esp_err_t solar_os_sc2_safety(const char *peer, sc2e_safety_t *safety);
esp_err_t solar_os_sc2_forget_peer(const char *peer);

/* Peer references for shell commands: a full 32-hex device ID, a saved alias
 * or a unique hex prefix (>= SOLAR_OS_SC2_PREFIX_MIN) of a stored peer bundle
 * or DM session. ESP_ERR_NOT_FOUND unknown, ESP_ERR_INVALID_STATE ambiguous,
 * ESP_ERR_INVALID_SIZE prefix too short; solar_os_sc2_peer_ref_error() has
 * the text. Aliases: up to SOLAR_OS_SC2_ALIAS_MAX names in <store>/alias.txt;
 * names are 1..16 of [A-Za-z0-9._-] and not all hex. device NULL deletes. */
#define SOLAR_OS_SC2_PREFIX_MIN 6u
#define SOLAR_OS_SC2_ALIAS_MAX 16u
#define SOLAR_OS_SC2_ALIAS_NAME_MAX 16u
esp_err_t solar_os_sc2_resolve_peer(const char *ref, char out[SC2_DEVICE_ID_HEX_LEN + 1]);
/* Sorted unique peer/session IDs; after may be empty to start. */
bool solar_os_sc2_peer_next(const char *after, char out[SC2_DEVICE_ID_HEX_LEN + 1]);
const char *solar_os_sc2_peer_ref_error(esp_err_t err);
bool solar_os_sc2_alias_name_valid(const char *name);
esp_err_t solar_os_sc2_alias_set(const char *name, const char *device);
esp_err_t solar_os_sc2_alias_get(size_t index, char name[SOLAR_OS_SC2_ALIAS_NAME_MAX + 1],
                                 char device[SC2_DEVICE_ID_HEX_LEN + 1]);

/* Safety-number verification, kept per peer in <store>/verified.txt (up to
 * SOLAR_OS_SC2_VERIFIED_MAX peers). Marking verified needs the peer's keys
 * (ESP_ERR_NOT_FOUND otherwise); unverifying an unknown peer is a no-op. */
#define SOLAR_OS_SC2_VERIFIED_MAX 64u
esp_err_t solar_os_sc2_verified_get(const char *device, bool *verified);
esp_err_t solar_os_sc2_verified_set(const char *device, bool verified);

/* One-time notice for public rooms (signed posts are public and
 * non-deniable): seen state per room in <store>/notice.txt, up to
 * SOLAR_OS_SC2_NOTICE_MAX rooms. Any read error counts as not seen. */
#define SOLAR_OS_SC2_NOTICE_MAX 64u
#define SOLAR_OS_SC2_PUBLIC_NOTICE                                                                  \
    "Public room: posts are signed with your device key and readable by anyone on the relay. "   \
    "Signed posts are non-deniable: anyone can prove you wrote them. Do not post secrets here."
bool solar_os_sc2_notice_seen(const char *room);
esp_err_t solar_os_sc2_notice_mark(const char *room);

esp_err_t solar_os_sc2_keys_publish(const char *rid, uint32_t opk_target, char *out, size_t cap, size_t *out_len);
esp_err_t solar_os_sc2_add_bundle(const char *line, size_t len, char device[SC2_DEVICE_ID_HEX_LEN + 1]);

esp_err_t solar_os_sc2_dm_encrypt(const char *peer, const char *rid, const char *inner, size_t inner_len, char *out,
                                  size_t cap, size_t *out_len);
esp_err_t solar_os_sc2_dm_decrypt(const char *line, size_t len, sc2e_dm_result_t *result, char *pt, size_t pt_cap,
                                  size_t *pt_len);

esp_err_t solar_os_sc2_room_create(const char *room, char room_out[SC2_DEVICE_ID_HEX_LEN + 1], uint32_t *epoch);
esp_err_t solar_os_sc2_room_rotate(const char *room, uint32_t *epoch);
esp_err_t solar_os_sc2_room_share(const char *room, const char *peer, const char *rid, char *out, size_t cap,
                                  size_t *out_len);
esp_err_t solar_os_sc2_room_encrypt(const char *room, const char *rid, const char *inner, size_t inner_len, char *out,
                                    size_t cap, size_t *out_len);
esp_err_t solar_os_sc2_room_decrypt(const char *line, size_t len, sc2e_room_result_t *result, char *pt, size_t pt_cap,
                                    size_t *pt_len);
esp_err_t solar_os_sc2_room_info(const char *room, sc2e_room_info_t *info);
esp_err_t solar_os_sc2_room_forget(const char *room);

esp_err_t solar_os_sc2_post_sign(const char *relay_id, const char *room, const char *name, const char *body,
                                 size_t body_len, uint64_t ts, const char *ref, bool deleted, const char *rid,
                                 char *out, size_t cap, size_t *out_len);
esp_err_t solar_os_sc2_post_verify(const char *relay_id, const char *line, size_t len, size_t max_body,
                                   sc2e_post_result_t *result, char *body, size_t body_cap);

/* Known-answer and session selftest against an in-memory store; never
 * touches /.sc2/. */
esp_err_t solar_os_sc2_selftest(sc2e_report_fn report, void *ctx, unsigned *passed, unsigned *failed);
esp_err_t solar_os_sc2_bench(uint32_t iterations, sc2e_bench_fn report, void *ctx);

/* Work buffers for callers (frames, plaintext): PSRAM preferred, wiped on free. */
void *solar_os_sc2_buffer_alloc(size_t len);
void solar_os_sc2_buffer_free(void *ptr, size_t len);
