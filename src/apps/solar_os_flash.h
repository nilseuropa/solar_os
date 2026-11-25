#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"

#define SOLAR_OS_FLASH_BOARD_ID_MAX 40U
#define SOLAR_OS_FLASH_BOARD_NAME_MAX 64U
#define SOLAR_OS_FLASH_FLAVOR_MAX 24U
#define SOLAR_OS_FLASH_VERSION_MAX 24U
#define SOLAR_OS_FLASH_CHIP_MAX 16U
#define SOLAR_OS_FLASH_ARCHIVE_MAX 128U
#define SOLAR_OS_FLASH_URL_MAX 320U

typedef struct {
  char board_id[SOLAR_OS_FLASH_BOARD_ID_MAX];
  char board_name[SOLAR_OS_FLASH_BOARD_NAME_MAX];
  char flavor[SOLAR_OS_FLASH_FLAVOR_MAX];
  char version[SOLAR_OS_FLASH_VERSION_MAX];
  char chip[SOLAR_OS_FLASH_CHIP_MAX];
  char archive[SOLAR_OS_FLASH_ARCHIVE_MAX];
  uint32_t archive_size;
  char archive_sha256[65];
  uint32_t factory_size;
  char factory_sha256[65];
  uint32_t flash_size;
  bool cached;
  char cache_root[SOLAR_OS_STORAGE_PATH_MAX];
} solar_os_flash_artifact_t;

typedef struct {
  char base_url[SOLAR_OS_FLASH_URL_MAX];
  size_t count;
  solar_os_flash_artifact_t *artifacts;
  char cache_root[SOLAR_OS_STORAGE_PATH_MAX];
} solar_os_flash_catalog_t;

typedef enum {
  SOLAR_OS_FLASH_PROGRESS_CATALOG,
  SOLAR_OS_FLASH_PROGRESS_SIGNATURE,
  SOLAR_OS_FLASH_PROGRESS_VERIFYING,
  SOLAR_OS_FLASH_PROGRESS_ARCHIVE,
  SOLAR_OS_FLASH_PROGRESS_EXTRACTING,
  SOLAR_OS_FLASH_PROGRESS_CONNECTING,
  SOLAR_OS_FLASH_PROGRESS_IDENTIFYING,
  SOLAR_OS_FLASH_PROGRESS_ERASING,
  SOLAR_OS_FLASH_PROGRESS_WRITING,
  SOLAR_OS_FLASH_PROGRESS_TARGET_VERIFY,
  SOLAR_OS_FLASH_PROGRESS_DONE,
} solar_os_flash_progress_stage_t;

typedef struct {
  solar_os_flash_progress_stage_t stage;
  uint32_t bytes_done;
  uint32_t bytes_total;
  bool total_known;
} solar_os_flash_progress_t;

typedef void (*solar_os_flash_progress_fn)(
    const solar_os_flash_progress_t *progress, void *user);

typedef struct {
  const char *port;
  int boot_pin;
  int reset_pin;
  uint32_t baud_rate;
} solar_os_flash_program_options_t;

/*
 * This service has no persistent state. All catalog, HTTP, ZIP, UART, GPIO,
 * and loader resources are allocated or acquired by these calls and released
 * before they return.
 */
/* Prefer a mounted SD volume, then USB storage. No internal-flash fallback. */
esp_err_t solar_os_flash_cache_root(char *root, size_t capacity);
bool solar_os_flash_cache_mounted(const char *root);
esp_err_t solar_os_flash_catalog_refresh(solar_os_flash_progress_fn progress,
                                         void *user);
esp_err_t solar_os_flash_catalog_refresh_at(const char *cache_root,
    solar_os_flash_progress_fn progress, void *user);
esp_err_t solar_os_flash_catalog_load(solar_os_flash_catalog_t **catalog);
esp_err_t solar_os_flash_catalog_load_at(const char *cache_root,
    solar_os_flash_catalog_t **catalog);
void solar_os_flash_catalog_free(solar_os_flash_catalog_t *catalog);

const solar_os_flash_artifact_t *
solar_os_flash_catalog_find(const solar_os_flash_catalog_t *catalog,
                            const char *board_id, const char *flavor,
                            const char *version);

esp_err_t
solar_os_flash_artifact_download(const solar_os_flash_catalog_t *catalog,
                                 const solar_os_flash_artifact_t *artifact,
                                 solar_os_flash_progress_fn progress,
                                 void *user);

esp_err_t
solar_os_flash_artifact_delete(const solar_os_flash_artifact_t *artifact);

esp_err_t
solar_os_flash_artifact_program(const solar_os_flash_artifact_t *artifact,
                                const solar_os_flash_program_options_t *options,
                                solar_os_flash_progress_fn progress,
                                void *user);

const char *
solar_os_flash_progress_stage_name(solar_os_flash_progress_stage_t stage);
