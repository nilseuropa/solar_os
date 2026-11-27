#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_sc2_types_v1.h"

#define SOLAR_OS_NATIVE_SC2_SERVICE "messaging.sc2"
#define SOLAR_OS_NATIVE_SC2_ABI 1U

/* Additional operation results for SC2 protocol failures. */
#define SOLAR_OS_NATIVE_SC2_ERROR_FORMAT (-256)
#define SOLAR_OS_NATIVE_SC2_ERROR_AUTH (-257)
#define SOLAR_OS_NATIVE_SC2_ERROR_KEY (-258)
#define SOLAR_OS_NATIVE_SC2_ERROR_RANDOM (-259)
#define SOLAR_OS_NATIVE_SC2_ERROR_REPLAY (-260)
#define SOLAR_OS_NATIVE_SC2_ERROR_GAP (-261)

/* Device identity, trust and message operations use the same persistent
 * state as the system shell and relay jobs. Serialize each application's
 * sends with its own transport, and retain wire frames unchanged until
 * accepted by the relay. All buffers and callback arguments are borrowed;
 * a callback runs on the caller's task before the operation returns. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*initialize)(void);
    int (*status)(solar_os_sc2_status_v1_t *status);
    int (*identity)(solar_os_sc2_identity_v1_t *info);
    int (*peer_info)(const char *peer, solar_os_sc2_peer_v1_t *info);
    int (*safety)(const char *peer, solar_os_sc2_safety_v1_t *safety);
    int (*forget_peer)(const char *peer);
    int (*resolve_peer)(const char *ref, char out[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1]);
    bool (*peer_next)(const char *after, char out[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1]);
    bool (*alias_name_valid)(const char *name);
    int (*alias_set)(const char *name, const char *device);
    int (*alias_get)(size_t index, char name[SOLAR_OS_SC2_ALIAS_LENGTH + 1],
                     char device[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1]);
    int (*verified_get)(const char *device, bool *verified);
    int (*verified_set)(const char *device, bool verified);
    bool (*notice_seen)(const char *room);
    int (*notice_mark)(const char *room);
    int (*keys_publish)(const char *rid, uint32_t opk_target, char *out, size_t cap,
                        size_t *out_len);
    int (*add_bundle)(const char *line, size_t len, char device[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1]);
    int (*dm_encrypt)(const char *peer, const char *rid, const char *inner, size_t inner_len,
                      char *out, size_t cap, size_t *out_len);
    int (*dm_decrypt)(const char *line, size_t len, solar_os_sc2_dm_v1_t *result, char *pt,
                      size_t pt_cap, size_t *pt_len);
    int (*room_create)(const char *room, char room_out[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1],
                       uint32_t *epoch);
    int (*room_rotate)(const char *room, uint32_t *epoch);
    int (*room_share)(const char *room, const char *peer, const char *rid, char *out, size_t cap,
                      size_t *out_len);
    int (*room_encrypt)(const char *room, const char *rid, const char *inner, size_t inner_len,
                        char *out, size_t cap, size_t *out_len);
    int (*room_decrypt)(const char *line, size_t len, solar_os_sc2_room_message_v1_t *result,
                        char *pt, size_t pt_cap, size_t *pt_len);
    int (*room_info)(const char *room, solar_os_sc2_room_v1_t *info);
    int (*room_forget)(const char *room);
    int (*post_sign)(const char *relay_id, const char *room, const char *name, const char *body,
                     size_t body_len, uint64_t ts, const char *ref, bool deleted, const char *rid,
                     char *out, size_t cap, size_t *out_len);
    int (*post_verify)(const char *relay_id, const char *line, size_t len, size_t max_body,
                       solar_os_sc2_post_v1_t *result, char *body, size_t body_cap);
    int (*selftest)(solar_os_sc2_selftest_fn report, void *ctx, unsigned *passed, unsigned *failed);
    int (*bench)(uint32_t iterations, solar_os_sc2_benchmark_fn report, void *ctx);
} solar_os_native_sc2_api_v1_t;
