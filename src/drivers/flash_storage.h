#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FLASH_STORAGE_PARTITION_LABEL "flash"
#define FLASH_STORAGE_MOUNT_POINT "/flash"
#define FLASH_STORAGE_ROOT_MOUNT_POINT "/"
#define FLASH_STORAGE_MOUNT_POINT_MAX 32
#define FLASH_STORAGE_LOGICAL_VOLUME_INVALID UINT8_MAX

esp_err_t flash_storage_mount(const char *mount_point);
esp_err_t flash_storage_unmount(void);
bool flash_storage_is_mounted(void);
const char *flash_storage_mount_point(void);
uint8_t flash_storage_logical_volume(void);
void flash_storage_get_status(char *buffer, size_t len);
esp_err_t flash_storage_get_usage(uint64_t *total_bytes,
                                  uint64_t *used_bytes,
                                  uint64_t *free_bytes);
