#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_meshcore_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MESHCORE_WORKER_STACK 7168U
#define SOLAR_OS_MESHCORE_PACKET_POOL_SIZE 16U
#define SOLAR_OS_MESHCORE_UPSTREAM_COMMIT \
    "03b6ef4b0de98fc70b49ef10a6d0d61f8381fb7a"

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
