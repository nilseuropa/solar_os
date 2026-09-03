#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#define SOLAR_OS_STORAGE_PATH_MAX 160

bool solar_os_storage_flash_is_mounted(void);
const char *solar_os_storage_flash_mount_point(void);
esp_err_t solar_os_storage_join_path(const char *base_path,
                                     const char *relative_path,
                                     char *path,
                                     size_t path_len);
esp_err_t solar_os_storage_mkdir(const char *path);
esp_err_t solar_os_storage_remove(const char *path);
esp_err_t solar_os_storage_sync_file(FILE *file);
esp_err_t solar_os_storage_sibling_path(const char *path,
                                        const char *suffix,
                                        char *out,
                                        size_t out_len);
esp_err_t solar_os_storage_replace_file(const char *staged_path,
                                        const char *active_path,
                                        const char *backup_path);
