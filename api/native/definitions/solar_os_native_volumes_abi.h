#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_VOLUMES_SERVICE "storage.volumes"
#define SOLAR_OS_NATIVE_VOLUMES_ABI 1U
#define SOLAR_OS_NATIVE_VOLUME_PREFERRED 0U
#define SOLAR_OS_NATIVE_VOLUME_FLASH 1U
#define SOLAR_OS_NATIVE_VOLUME_SD 2U
#define SOLAR_OS_NATIVE_MOUNT_SD 0U
#define SOLAR_OS_NATIVE_MOUNT_FLASH 1U
#define SOLAR_OS_NATIVE_MOUNT_RAMFS 2U
#define SOLAR_OS_NATIVE_MOUNT_IMAGE 3U
#define SOLAR_OS_NATIVE_MOUNT_USB 4U
#define SOLAR_OS_NATIVE_MOUNT_HOST 5U
#define SOLAR_OS_NATIVE_MOUNT_UNKNOWN UINT32_MAX
#define SOLAR_OS_NATIVE_BLOCK_DISK 0U
#define SOLAR_OS_NATIVE_BLOCK_PARTITION 1U
#define SOLAR_OS_NATIVE_BLOCK_BOOTABLE 1U
#define SOLAR_OS_NATIVE_BLOCK_MOUNTABLE 2U
#define SOLAR_OS_NATIVE_BLOCK_MOUNTED 4U
#define SOLAR_OS_NATIVE_BLOCK_WHOLE_FS 8U
typedef struct {
    uint64_t total, used, free;
} solar_os_native_storage_usage_v1_t;
typedef struct {
    char path[32], name[12];
    uint32_t type;
} solar_os_native_mount_v1_t;
typedef struct {
    char name[12], fs[8], type_name[12], mount_point[32];
    uint64_t start_sector, sector_count, bytes;
    uint32_t type, partition, mbr_type, flags, logical_volume, sector_size;
} solar_os_native_block_v1_t;
/* OS-wide volume controls. No pointers are retained. Strings and metadata are
 * copied into caller storage. A null mount target selects the preferred
 * persistent volume; a null unmount target follows the OS preferred-volume
 * policy. Format is an explicit operation; there is no implicit formatting. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*state)(uint32_t role, bool *mounted, bool *unrecognized, char *path, size_t capacity);
    int (*status)(uint32_t role, char *text, size_t capacity);
    bool (*root_mounted)(void);
    int (*usage)(const char *path, solar_os_native_storage_usage_v1_t *usage);
    int (*block_usage)(const char *name, solar_os_native_storage_usage_v1_t *usage);
    uint32_t (*block_count)(void);
    int (*block)(uint32_t index, solar_os_native_block_v1_t *block);
    uint32_t (*mount_count)(void);
    int (*mount_info)(uint32_t index, solar_os_native_mount_v1_t *mount);
    int (*mount)(const char *target, const char *path);
    int (*unmount)(const char *target);
    int (*format)(const char *target);
    int (*rescan)(void);
} solar_os_native_volumes_api_v1_t;
