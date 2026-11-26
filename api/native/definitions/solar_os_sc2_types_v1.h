#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public status and protocol results. Cryptographic keys held by the
 * running service are not part of any record. */
#define SOLAR_OS_SC2_PUBLIC_KEY_BASE64_LENGTH 44U
#define SOLAR_OS_SC2_DEVICE_ID_LENGTH 32U
#define SOLAR_OS_SC2_FINGERPRINT_LENGTH 29U
#define SOLAR_OS_SC2_SAFETY_FULL_LENGTH 71U
#define SOLAR_OS_SC2_SAFETY_WORDS_LENGTH 72U
#define SOLAR_OS_SC2_PUBLIC_ROOM_NAME_LENGTH 64U
#define SOLAR_OS_SC2_PUBLIC_AUTHOR_LENGTH 64U
#define SOLAR_OS_SC2_MESSAGE_ID_LENGTH 16U
#define SOLAR_OS_SC2_STORAGE_PATH_CAPACITY 112U
#define SOLAR_OS_SC2_ALIAS_LENGTH 16U

#define SOLAR_OS_SC2_FRAME_CAPACITY 16385U
#define SOLAR_OS_SC2_MESSAGE_CAPACITY 8192U
#define SOLAR_OS_SC2_PREKEYS_SIGNED_ONLY UINT32_MAX

typedef enum {
    SOLAR_OS_SC2_DM_MESSAGE = 0,
    SOLAR_OS_SC2_DM_EPOCH_KEY = 1,
    SOLAR_OS_SC2_DM_HISTORY = 2,
    SOLAR_OS_SC2_DM_IGNORED = 3,
} solar_os_sc2_dm_kind_v1_t;

typedef struct {
    bool ready;
    bool created;       /* identity generated during this boot */
    bool work_in_psram; /* work buffers are allocated from external RAM */
    char device_id[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    char fingerprint[SOLAR_OS_SC2_FINGERPRINT_LENGTH + 1];
    uint32_t spk_id;
    uint64_t spk_created;
    uint32_t opk_count;
    size_t peers;
    size_t sessions;
    size_t rooms;
    const char *core_version;
    const char *hash_backend;
    char store_path[SOLAR_OS_SC2_STORAGE_PATH_CAPACITY];
} solar_os_sc2_status_v1_t;

typedef struct {
    char device_id[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    char ik_b64[SOLAR_OS_SC2_PUBLIC_KEY_BASE64_LENGTH + 1];
    char dk_b64[SOLAR_OS_SC2_PUBLIC_KEY_BASE64_LENGTH + 1];
    char fingerprint[SOLAR_OS_SC2_FINGERPRINT_LENGTH + 1];
    uint32_t spk_id;
    uint64_t spk_created;
    uint32_t opk_count;
} solar_os_sc2_identity_v1_t;

typedef struct {
    char short_form[SOLAR_OS_SC2_FINGERPRINT_LENGTH + 1];
    char full_form[SOLAR_OS_SC2_SAFETY_FULL_LENGTH + 1];
    char words[SOLAR_OS_SC2_SAFETY_WORDS_LENGTH + 1];
} solar_os_sc2_safety_v1_t;

typedef struct {
    bool known;
    bool has_session;
    bool opk_available;
    uint32_t spk_id;
    char fingerprint[SOLAR_OS_SC2_FINGERPRINT_LENGTH + 1];
} solar_os_sc2_peer_v1_t;

typedef struct {
    char from[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    uint64_t seq;
    int32_t kind; /* solar_os_sc2_dm_kind_v1_t */
    bool new_session;
    char room[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1]; /* SOLAR_OS_SC2_DM_EPOCH_KEY only */
    uint32_t epoch;                               /* SOLAR_OS_SC2_DM_EPOCH_KEY only */
} solar_os_sc2_dm_v1_t;

typedef struct {
    char room[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    char from[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    uint64_t seq;
    uint32_t epoch;
    uint32_t n;
} solar_os_sc2_room_message_v1_t;

typedef struct {
    bool known;
    bool is_admin;
    char admin[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    uint32_t epoch;
    uint32_t sent;    /* own sender chain index in the current epoch */
    uint32_t senders; /* receiver chains held for the current epoch */
} solar_os_sc2_room_v1_t;

typedef struct {
    char room[SOLAR_OS_SC2_PUBLIC_ROOM_NAME_LENGTH + 1];
    char from[SOLAR_OS_SC2_DEVICE_ID_LENGTH + 1];
    char id[SOLAR_OS_SC2_MESSAGE_ID_LENGTH + 1];
    char ref[SOLAR_OS_SC2_MESSAGE_ID_LENGTH + 1]; /* empty when absent */
    char name[SOLAR_OS_SC2_PUBLIC_AUTHOR_LENGTH + 1];
    uint64_t seq;
    uint64_t ts;
    bool deleted;     /* t == "del" */
    bool valid;       /* signature and sender binding verified */
    const char *body; /* points into the caller's body buffer */
    size_t body_len;
} solar_os_sc2_post_v1_t;

typedef struct {
    const char *name;
    uint32_t iterations;
    uint64_t total_us;
    int32_t rc;
} solar_os_sc2_benchmark_v1_t;

typedef void (*solar_os_sc2_selftest_fn)(void *ctx, const char *name, int rc);

typedef void (*solar_os_sc2_benchmark_fn)(void *ctx, const solar_os_sc2_benchmark_v1_t *result);
