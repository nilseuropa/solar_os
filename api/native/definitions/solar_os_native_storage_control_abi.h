#pragma once
#include "solar_os_native_volumes_abi.h"
#define SOLAR_OS_NATIVE_DISK_IMAGE_SERVICE "storage.images"
#define SOLAR_OS_NATIVE_DISK_IMAGE_ABI 1U
#define SOLAR_OS_NATIVE_RAMFS_SERVICE "storage.ramfs"
#define SOLAR_OS_NATIVE_RAMFS_ABI 1U
#define SOLAR_OS_NATIVE_SETTINGS_BACKUP_SERVICE "settings.backup"
#define SOLAR_OS_NATIVE_SETTINGS_BACKUP_ABI 1U
typedef struct {
    char path[16];
    uint64_t total, used, free, file_count, directory_count, open_count;
} solar_os_native_ramfs_info_v1_t;
typedef struct { uint64_t partition_size, file_size; uint32_t crc32, reboot_required; } solar_os_native_settings_backup_v1_t;
/* An image lease owns one OS raw-image claim. Its host-owned owner identity
 * survives between calls and through cleanup retries. Serialize operations.
 * The OS rejects simultaneous raw/mounted use and enforces sector/file bounds.
 * OS close consumes the raw claim even on a sync error. A close error leaves
 * the native token only for cleanup retry; further I/O is invalid.
 * No VFS, FAT or backend handles escape. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *path, solar_os_native_handle_t *image, uint64_t *size);
    int (*close)(solar_os_native_handle_t image);
    int (*read)(solar_os_native_handle_t image, uint64_t offset, void *data, size_t size);
    int (*write)(solar_os_native_handle_t image, uint64_t offset, const void *data, size_t size);
    int (*sync)(solar_os_native_handle_t image);
    int (*mount)(const char *path, const char *mount_point, uint32_t partition);
    int (*unmount)(const char *target);
    int (*create)(const char *path, uint32_t size_mib);
    size_t (*mount_count)(void);
    int (*mount_info)(size_t index, solar_os_native_mount_v1_t *mount);
    int (*block)(size_t index, solar_os_native_block_v1_t *block);
    bool (*path_busy)(const char *path);
    bool (*backing_busy)(const char *mount_point);
} solar_os_native_disk_image_api_v1_t;
/* RAM volumes use the normal filesystem interface for contents. Quotas are
 * byte counts limited to the ESP32 address space. Status records are copied. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*mount)(const char *path, uint64_t quota);
    int (*unmount)(const char *path);
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_ramfs_info_v1_t *info);
    bool (*has_mount_prefix)(const char *path);
    int (*mount_point)(const char *path, char *mount_point, size_t capacity);
    int (*usage)(const char *path, solar_os_native_storage_usage_v1_t *usage);
} solar_os_native_ramfs_api_v1_t;
/* Restore reports whether a reboot is required; it does not restart the OS.
 * Operations are explicit. Paths and result storage are borrowed during calls.
 * default_path is a host-owned constant valid until reboot. */
typedef struct {
    uint32_t abi_version, struct_size;
    const char *(*default_path)(void);
    int (*create)(const char *path, solar_os_native_settings_backup_v1_t *result);
    int (*restore)(const char *path, solar_os_native_settings_backup_v1_t *result);
} solar_os_native_settings_backup_api_v1_t;
