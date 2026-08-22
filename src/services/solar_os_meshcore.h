#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MESHCORE_NAME_MAX 31U
#define SOLAR_OS_MESHCORE_GROUP_CAPACITY 8U
#define SOLAR_OS_MESHCORE_GROUP_NAME_MAX 24U
#define SOLAR_OS_MESHCORE_PUBLIC_GROUP "Public"
#define SOLAR_OS_MESHCORE_PRIVATE_KEY_HEX_LEN 129U
#define SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN 65U
#define SOLAR_OS_MESHCORE_WORKER_STACK 7168U
#define SOLAR_OS_MESHCORE_PACKET_POOL_SIZE 16U
#define SOLAR_OS_MESHCORE_UPSTREAM_COMMIT \
    "03b6ef4b0de98fc70b49ef10a6d0d61f8381fb7a"

typedef enum {
    SOLAR_OS_MESHCORE_ADVERT_ZERO = 0,
    SOLAR_OS_MESHCORE_ADVERT_FLOOD,
} solar_os_meshcore_advert_t;

typedef struct {
    uint32_t id;
    bool builtin;
    bool enabled;
    char name[SOLAR_OS_MESHCORE_GROUP_NAME_MAX + 1U];
} solar_os_meshcore_channel_t;

typedef struct {
    bool initialized;
    bool running;
    bool context_in_psram;
    bool identity_set;
    bool public_channel_enabled;
    char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U];
    char public_key_hex[SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN];
    char radio[20];
    char profile[24];
    size_t channels;
    size_t contacts_loaded;
    size_t packet_pool_free;
    uint32_t transmitted;
    uint32_t received;
    uint32_t send_errors;
    uint32_t receive_errors;
    uint32_t adverts_received;
    uint32_t adverts_sent;
    uint32_t direct_received;
    uint32_t group_received;
    uint32_t acknowledgements;
    uint32_t retries;
    uint32_t duplicate_direct;
    uint32_t duplicate_flood;
    uint32_t stack_watermark_bytes;
    uint32_t generation;
    esp_err_t last_error;
} solar_os_meshcore_status_t;

esp_err_t solar_os_meshcore_init(void);
esp_err_t solar_os_meshcore_get_status(solar_os_meshcore_status_t *status);

esp_err_t solar_os_meshcore_identity_generate(bool force);
esp_err_t solar_os_meshcore_identity_import(const char *private_key_hex);
esp_err_t solar_os_meshcore_identity_public(
    char public_key_hex[SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN]);
esp_err_t solar_os_meshcore_identity_export_private(
    char private_key_hex[SOLAR_OS_MESHCORE_PRIVATE_KEY_HEX_LEN]);

esp_err_t solar_os_meshcore_name_get(
    char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U]);
esp_err_t solar_os_meshcore_name_set(const char *name);

size_t solar_os_meshcore_channel_snapshot(
    solar_os_meshcore_channel_t *channels,
    size_t max_channels);
/* Pass NULL for base64_psk to derive a public hashtag channel key. */
esp_err_t solar_os_meshcore_channel_add(const char *name,
                                        const char *base64_psk);
esp_err_t solar_os_meshcore_channel_remove(const char *name);
esp_err_t solar_os_meshcore_channel_public_set(bool enabled);

esp_err_t solar_os_meshcore_start(const char *radio,
                                  const char *profile,
                                  const char *owner);
void solar_os_meshcore_loop_once(void);
esp_err_t solar_os_meshcore_request_advert(
    solar_os_meshcore_advert_t advert);
void solar_os_meshcore_note_stack_watermark(uint32_t bytes);
void solar_os_meshcore_stop(void);

#ifdef __cplusplus
}
#endif
