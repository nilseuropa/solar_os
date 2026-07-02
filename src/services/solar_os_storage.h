#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_STORAGE_PATH_MAX 160
#define SOLAR_OS_STORAGE_BLOCK_NAME_MAX 12
#define SOLAR_OS_STORAGE_FS_NAME_MAX 8
#define SOLAR_OS_STORAGE_TYPE_NAME_MAX 12
#define SOLAR_OS_STORAGE_MOUNT_POINT_MAX 32
#define SOLAR_OS_STORAGE_LOGICAL_VOLUME_INVALID UINT8_MAX

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOLAR_OS_STORAGE_BLOCK_DISK,
    SOLAR_OS_STORAGE_BLOCK_PARTITION,
} solar_os_storage_block_type_t;

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
} solar_os_storage_usage_t;

typedef struct {
    char name[SOLAR_OS_STORAGE_BLOCK_NAME_MAX];
    solar_os_storage_block_type_t type;
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
    char fs[SOLAR_OS_STORAGE_FS_NAME_MAX];
    char type_name[SOLAR_OS_STORAGE_TYPE_NAME_MAX];
    char mount_point[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
} solar_os_storage_block_t;

typedef enum {
    SOLAR_OS_STORAGE_MOUNT_SD,
    SOLAR_OS_STORAGE_MOUNT_FLASH,
    SOLAR_OS_STORAGE_MOUNT_RAMFS,
} solar_os_storage_mount_type_t;

typedef struct {
    char mount_point[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    char name[SOLAR_OS_STORAGE_BLOCK_NAME_MAX];
    solar_os_storage_mount_type_t type;
} solar_os_storage_mount_info_t;

esp_err_t solar_os_storage_init(void);
esp_err_t solar_os_storage_mount(void);
esp_err_t solar_os_storage_mount_volume(const char *name, const char *mount_point);
esp_err_t solar_os_storage_unmount(void);
esp_err_t solar_os_storage_unmount_volume(const char *target);
bool solar_os_storage_is_mounted(void);
void solar_os_storage_get_status(char *buffer, size_t len);
const char *solar_os_storage_mount_point(void);

// Joins a relative path under a storage-owned base path. Dot-dot segments are
// clamped to base_path, so this is for app/service state paths, not shell cwd
// resolution where users expect `..` to walk upward.
esp_err_t solar_os_storage_join_path(const char *base_path,
                                     const char *relative_path,
                                     char *path,
                                     size_t path_len);
esp_err_t solar_os_storage_default_path(const char *relative_path,
                                        char *path,
                                        size_t path_len);
esp_err_t solar_os_storage_get_usage(solar_os_storage_usage_t *usage);
esp_err_t solar_os_storage_get_usage_for_path(const char *path, solar_os_storage_usage_t *usage);
esp_err_t solar_os_storage_get_usage_for_block(const solar_os_storage_block_t *block,
                                               solar_os_storage_usage_t *usage);
esp_err_t solar_os_storage_rescan(void);
size_t solar_os_storage_block_count(void);
bool solar_os_storage_get_block(size_t index, solar_os_storage_block_t *block);
size_t solar_os_storage_mount_count(void);
bool solar_os_storage_get_mount(size_t index, solar_os_storage_mount_info_t *mount);
bool solar_os_storage_root_is_mounted(void);
bool solar_os_storage_path_has_mount_prefix(const char *path);
esp_err_t solar_os_storage_path_mount_point(const char *path,
                                            char *mount_point,
                                            size_t mount_point_len);
esp_err_t solar_os_storage_normalize_path(const char *path, char *out, size_t out_len);
esp_err_t solar_os_storage_resolve_path_at(const char *cwd,
                                           const char *arg,
                                           char *path,
                                           size_t path_len);
esp_err_t solar_os_storage_resolve_path(const char *arg, char *path, size_t path_len);
esp_err_t solar_os_storage_mkdir(const char *path);
esp_err_t solar_os_storage_rmdir(const char *path);
esp_err_t solar_os_storage_remove(const char *path);
esp_err_t solar_os_storage_rename(const char *old_path, const char *new_path);
esp_err_t solar_os_storage_copy_file(const char *source_path, const char *dest_path);

#ifdef __cplusplus
}
#endif
