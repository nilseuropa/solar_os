#pragma once

#include <dirent.h>
#include "solar_os_storage.h"

typedef struct {
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    DIR *volume;
} solar_os_flash_cache_t;

/* A directory lease keeps USB unmount/mode changes busy. Its VFS generation
 * also detects removal and replacement while an operation is in progress. */
esp_err_t solar_os_flash_cache_open(const char *root, solar_os_flash_cache_t *cache);
esp_err_t solar_os_flash_cache_check(const solar_os_flash_cache_t *cache);
void solar_os_flash_cache_close(solar_os_flash_cache_t *cache);
bool solar_os_flash_cache_mounted(const char *root);
