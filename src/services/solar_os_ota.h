#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_ota_abi.h"

#define SOLAR_OS_OTA_DEFAULT_URL "https://solar-os.eu/ota/latest"
#define SOLAR_OS_OTA_INDEX_FILE "index.json"
#define SOLAR_OS_OTA_INDEX_SIGNATURE_FILE "index.sig"
#define SOLAR_OS_OTA_MANIFEST_FILE "manifest.json"
#define SOLAR_OS_OTA_VERSION_FILE "version.txt"
#define SOLAR_OS_OTA_FIRMWARE_FILE "firmware.bin"

esp_err_t solar_os_ota_init(void);
void solar_os_ota_get_url(char *url, size_t len);
esp_err_t solar_os_ota_set_url(const char *url);
void solar_os_ota_get_flavor(char *flavor, size_t len);
esp_err_t solar_os_ota_set_flavor(const char *flavor);
bool solar_os_ota_available_flavors_checked(void);
size_t solar_os_ota_available_flavor_count(void);
bool solar_os_ota_get_available_flavor(size_t index, char *flavor, size_t len);
esp_err_t solar_os_ota_get_index_url(char *index_url, size_t index_url_len);
esp_err_t solar_os_ota_get_status(solar_os_ota_status_t *status);
esp_err_t solar_os_ota_set_boot_slot(uint8_t slot);
esp_err_t solar_os_ota_check(solar_os_ota_check_result_t *result);
esp_err_t solar_os_ota_upgrade(solar_os_ota_progress_cb_t progress, void *user);
