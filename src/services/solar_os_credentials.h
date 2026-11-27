#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_messaging_types.h"
#include "solar_os_native_credentials_abi.h"

esp_err_t solar_os_credentials_init(void);
esp_err_t solar_os_credentials_get_status(solar_os_credentials_status_t *status);
size_t solar_os_credentials_snapshot(solar_os_credential_info_t *records,
                                     size_t max_records);
esp_err_t solar_os_credentials_find(solar_os_messaging_provider_id_t provider,
                                    solar_os_credential_kind_t kind,
                                    const char *label,
                                    solar_os_credential_info_t *record);
esp_err_t solar_os_credentials_put(solar_os_messaging_provider_id_t provider,
                                   solar_os_credential_kind_t kind,
                                   const char *label,
                                   const void *secret,
                                   size_t secret_len,
                                   bool replace,
                                   solar_os_credential_id_t *record_id);
esp_err_t solar_os_credentials_read_secret(solar_os_credential_id_t record_id,
                                           void *secret,
                                           size_t secret_capacity,
                                           size_t *secret_len);
esp_err_t solar_os_credentials_remove(solar_os_credential_id_t record_id);
/* Removes only this instance's secrets, including a retry after a failed save. */
esp_err_t solar_os_credentials_remove_provider(solar_os_messaging_provider_id_t provider);
void solar_os_credentials_wipe(void *buffer, size_t length);
const char *solar_os_credential_kind_name(solar_os_credential_kind_t kind);
