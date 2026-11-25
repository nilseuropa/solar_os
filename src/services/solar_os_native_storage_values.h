#pragma once
#include "solar_os_native_volumes_abi.h"
#include "solar_os_storage.h"
#include <string.h>
static inline int solar_os_native_storage_block_value(const solar_os_storage_block_t *in, solar_os_native_block_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (in->type) {
    case SOLAR_OS_STORAGE_BLOCK_DISK: out->type = SOLAR_OS_NATIVE_BLOCK_DISK; break;
    case SOLAR_OS_STORAGE_BLOCK_PARTITION: out->type = SOLAR_OS_NATIVE_BLOCK_PARTITION; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
#define COPY(field) do { _Static_assert(sizeof(out->field) == sizeof(in->field), "storage text capacity"); memcpy(out->field, in->field, sizeof(out->field)); } while (0)
    COPY(name); COPY(fs); COPY(type_name); COPY(mount_point);
#undef COPY
    out->start_sector = in->start_sector; out->sector_count = in->sector_count; out->bytes = in->size_bytes;
    out->partition = in->partition_number; out->mbr_type = in->mbr_type; out->logical_volume = in->logical_volume; out->sector_size = in->sector_size;
    out->flags = (in->bootable ? SOLAR_OS_NATIVE_BLOCK_BOOTABLE : 0) | (in->mountable ? SOLAR_OS_NATIVE_BLOCK_MOUNTABLE : 0) |
        (in->mounted ? SOLAR_OS_NATIVE_BLOCK_MOUNTED : 0) | (in->whole_disk_filesystem ? SOLAR_OS_NATIVE_BLOCK_WHOLE_FS : 0);
    return SOLAR_OS_NATIVE_OK;
}
static inline int solar_os_native_storage_mount_value(const solar_os_storage_mount_info_t *in, solar_os_native_mount_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (in->type) {
    case SOLAR_OS_STORAGE_MOUNT_SD: out->type = SOLAR_OS_NATIVE_MOUNT_SD; break;
    case SOLAR_OS_STORAGE_MOUNT_FLASH: out->type = SOLAR_OS_NATIVE_MOUNT_FLASH; break;
    case SOLAR_OS_STORAGE_MOUNT_RAMFS: out->type = SOLAR_OS_NATIVE_MOUNT_RAMFS; break;
    case SOLAR_OS_STORAGE_MOUNT_IMAGE: out->type = SOLAR_OS_NATIVE_MOUNT_IMAGE; break;
    case SOLAR_OS_STORAGE_MOUNT_USB: out->type = SOLAR_OS_NATIVE_MOUNT_USB; break;
    case SOLAR_OS_STORAGE_MOUNT_HOST: out->type = SOLAR_OS_NATIVE_MOUNT_HOST; break;
    default: out->type = SOLAR_OS_NATIVE_MOUNT_UNKNOWN; break;
    }
    _Static_assert(sizeof(out->path) == sizeof(in->mount_point), "mount path capacity");
    _Static_assert(sizeof(out->name) == sizeof(in->name), "mount name capacity");
    memcpy(out->path, in->mount_point, sizeof(out->path)); memcpy(out->name, in->name, sizeof(out->name));
    return SOLAR_OS_NATIVE_OK;
}
