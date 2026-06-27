#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_BOARD_STORAGE_DEFAULT_MOUNT_POINT "/sdcard"
#define SOLAR_OS_BOARD_STORAGE_BLOCK_NAME_MAX 12
#define SOLAR_OS_BOARD_STORAGE_FS_NAME_MAX 8
#define SOLAR_OS_BOARD_STORAGE_TYPE_NAME_MAX 12
#define SOLAR_OS_BOARD_STORAGE_MOUNT_POINT_MAX 32
#define SOLAR_OS_BOARD_STORAGE_LOGICAL_VOLUME_INVALID UINT8_MAX

typedef enum {
    SOLAR_OS_BOARD_STORAGE_BLOCK_DISK,
    SOLAR_OS_BOARD_STORAGE_BLOCK_PARTITION,
} solar_os_board_storage_block_type_t;

typedef struct {
    char name[SOLAR_OS_BOARD_STORAGE_BLOCK_NAME_MAX];
    solar_os_board_storage_block_type_t type;
    uint8_t partition_number;
    uint8_t mbr_type;
    bool bootable;
    bool mountable;
    bool mounted;
    bool whole_disk_filesystem;
    uint8_t logical_volume;
    uint64_t start_sector;
    uint64_t sector_count;
    uint32_t sector_size;
    uint64_t size_bytes;
    char fs[SOLAR_OS_BOARD_STORAGE_FS_NAME_MAX];
    char type_name[SOLAR_OS_BOARD_STORAGE_TYPE_NAME_MAX];
    char mount_point[SOLAR_OS_BOARD_STORAGE_MOUNT_POINT_MAX];
} solar_os_board_storage_block_t;

esp_err_t solar_os_board_storage_init(void);
esp_err_t solar_os_board_storage_mount(void);
esp_err_t solar_os_board_storage_mount_volume(const char *name, const char *mount_point);
esp_err_t solar_os_board_storage_unmount(void);
esp_err_t solar_os_board_storage_unmount_volume(const char *target);
bool solar_os_board_storage_is_mounted(void);
void solar_os_board_storage_get_status(char *buffer, size_t len);
const char *solar_os_board_storage_mount_point(void);
esp_err_t solar_os_board_storage_rescan(void);
size_t solar_os_board_storage_block_count(void);
bool solar_os_board_storage_get_block(size_t index, solar_os_board_storage_block_t *block);

