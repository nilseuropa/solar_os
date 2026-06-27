#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_OTA_DEFAULT_URL "https://hypergraph.cloud/solaros/latest"
#define SOLAR_OS_OTA_URL_MAX 160
#define SOLAR_OS_OTA_ARTIFACT_URL_MAX 220
#define SOLAR_OS_OTA_LABEL_MAX 17
#define SOLAR_OS_OTA_VERSION_MAX 32
#define SOLAR_OS_OTA_FLAVOR_MAX 32
#define SOLAR_OS_OTA_BOARD_MAX 64
#define SOLAR_OS_OTA_PROJECT_MAX 32
#define SOLAR_OS_OTA_STATE_MAX 16
#define SOLAR_OS_OTA_SHA256_MAX 65
#define SOLAR_OS_OTA_INDEX_FILE "index.json"
#define SOLAR_OS_OTA_INDEX_SIGNATURE_FILE "index.sig"
#define SOLAR_OS_OTA_MANIFEST_FILE "manifest.json"
#define SOLAR_OS_OTA_VERSION_FILE "version.txt"
#define SOLAR_OS_OTA_FIRMWARE_FILE "firmware.bin"

typedef enum {
    SOLAR_OS_OTA_PROGRESS_CONNECTING,
    SOLAR_OS_OTA_PROGRESS_IMAGE,
    SOLAR_OS_OTA_PROGRESS_WRITING,
    SOLAR_OS_OTA_PROGRESS_VERIFYING,
    SOLAR_OS_OTA_PROGRESS_DONE,
} solar_os_ota_progress_stage_t;

typedef struct {
    bool valid;
    int slot;
    uint8_t subtype;
    uint32_t address;
    uint32_t size;
    char label[SOLAR_OS_OTA_LABEL_MAX];
    char version[SOLAR_OS_OTA_VERSION_MAX];
    char project_name[SOLAR_OS_OTA_PROJECT_MAX];
    char state[SOLAR_OS_OTA_STATE_MAX];
} solar_os_ota_partition_t;

typedef struct {
    char url[SOLAR_OS_OTA_URL_MAX];
    char compiled_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
    char target_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
    uint8_t ota_partition_count;
    solar_os_ota_partition_t running;
    solar_os_ota_partition_t boot;
    solar_os_ota_partition_t next_update;
} solar_os_ota_status_t;

typedef struct {
    char compiled_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
    char target_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
    char current_version[SOLAR_OS_OTA_VERSION_MAX];
    char available_version[SOLAR_OS_OTA_VERSION_MAX];
    char board_id[SOLAR_OS_OTA_BOARD_MAX];
    char index_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    char index_sig_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    char manifest_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    char firmware_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    char image_sha256[SOLAR_OS_OTA_SHA256_MAX];
    uint32_t image_size;
    int status_code;
    int64_t content_length;
    bool image_size_known;
    bool index_signature_verified;
    bool update_available;
} solar_os_ota_check_result_t;

typedef struct {
    solar_os_ota_progress_stage_t stage;
    char firmware_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    char version[SOLAR_OS_OTA_VERSION_MAX];
    uint32_t bytes_read;
    uint32_t image_size;
    bool image_size_known;
    int status_code;
} solar_os_ota_progress_t;

typedef void (*solar_os_ota_progress_cb_t)(const solar_os_ota_progress_t *progress, void *user);

esp_err_t solar_os_ota_init(void);
void solar_os_ota_get_url(char *url, size_t len);
esp_err_t solar_os_ota_set_url(const char *url);
void solar_os_ota_get_flavor(char *flavor, size_t len);
esp_err_t solar_os_ota_set_flavor(const char *flavor);
esp_err_t solar_os_ota_get_index_url(char *index_url, size_t index_url_len);
esp_err_t solar_os_ota_get_status(solar_os_ota_status_t *status);
esp_err_t solar_os_ota_set_boot_slot(uint8_t slot);
esp_err_t solar_os_ota_check(solar_os_ota_check_result_t *result);
esp_err_t solar_os_ota_upgrade(solar_os_ota_progress_cb_t progress, void *user);
