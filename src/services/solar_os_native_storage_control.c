#include "solar_os_native_storage_control.h"
#include "solar_os_native_storage_control_abi.h"
#include "solar_os_native_storage_values.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include <string.h>
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_disk_image.h"
#include "solar_os_ramfs.h"
#include "solar_os_nvs_backup.h"
typedef struct { uint32_t image; } native_image_claim_t;
static int image_destroy(void *value)
{
    native_image_claim_t *state = value;
    if (state->image) {
        uint32_t image = state->image;
        /* The raw close consumes its fd/claim even when sync or close reports
         * failure. Retain only this token's cleanup state after an error. */
        state->image = 0;
        esp_err_t error = solar_os_disk_image_close(image, state);
        if (error) return solar_os_native_result(error);
    }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int image_open(const char *path, solar_os_native_handle_t *out, uint64_t *size)
{
    if (!out || !size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; *size = 0;
    native_image_claim_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.image");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE, state, image_destroy, out);
    if (result) { image_destroy(state); return result; }
    uint32_t bytes = 0; result = solar_os_native_result(solar_os_disk_image_open(path, state, &state->image, &bytes));
    if (!result) *size = bytes;
    else if (!solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE)) *out = 0;
    return result;
}
static int image_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE); }
static int image_read(solar_os_native_handle_t id, uint64_t offset, void *bytes, size_t size)
{
    if ((!bytes && size) || offset > UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_image_claim_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = state->image ? solar_os_native_result(solar_os_disk_image_read(state->image, state, offset, bytes, size)) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release(id); return result;
}
static int image_write(solar_os_native_handle_t id, uint64_t offset, const void *bytes, size_t size)
{
    if ((!bytes && size) || offset > UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_image_claim_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = state->image ? solar_os_native_result(solar_os_disk_image_write(state->image, state, offset, bytes, size)) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release(id); return result;
}
static int image_sync(solar_os_native_handle_t id)
{
    native_image_claim_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = state->image ? solar_os_native_result(solar_os_disk_image_sync(state->image, state)) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release(id); return result;
}
static int image_mount(const char *path, const char *mount, uint32_t partition) { return solar_os_native_result(solar_os_disk_image_mount(path, mount, partition)); }
static int image_unmount(const char *target) { return solar_os_native_result(solar_os_disk_image_unmount(target)); }
static int image_create(const char *path, uint32_t size_mib) { return solar_os_native_result(solar_os_disk_image_create(path, size_mib)); }
static int image_mount_info(size_t index, solar_os_native_mount_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_storage_mount_info_t value = {0};
    return solar_os_disk_image_get_mount(index, &value) ? solar_os_native_storage_mount_value(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int image_block(size_t index, solar_os_native_block_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_storage_block_t value = {0};
    return solar_os_disk_image_get_block(index, &value) ? solar_os_native_storage_block_value(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static const solar_os_native_disk_image_api_v1_t image_api = {
    .abi_version = SOLAR_OS_NATIVE_DISK_IMAGE_ABI, .struct_size = sizeof(image_api), .open = image_open, .close = image_close,
    .read = image_read, .write = image_write, .sync = image_sync, .mount = image_mount, .unmount = image_unmount, .create = image_create,
    .mount_count = solar_os_disk_image_mount_count, .mount_info = image_mount_info, .block = image_block,
    .path_busy = solar_os_disk_image_path_busy, .backing_busy = solar_os_disk_image_backing_busy,
};
static int ramfs_mount(const char *path, uint64_t quota)
{ return quota > UINT32_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_SIZE : solar_os_native_result(solar_os_ramfs_mount(path, (size_t)quota)); }
static int ramfs_unmount(const char *path) { return solar_os_native_result(solar_os_ramfs_unmount(path)); }
static int ramfs_info(size_t index, solar_os_native_ramfs_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_ramfs_info_t value = {0}; if (!solar_os_ramfs_get_info(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    _Static_assert(sizeof(out->path) == sizeof(value.mount_point), "RAMFS path capacity"); memcpy(out->path, value.mount_point, sizeof(out->path));
    out->total = value.total_bytes; out->used = value.used_bytes; out->free = value.free_bytes;
    out->file_count = value.file_count; out->directory_count = value.dir_count; out->open_count = value.open_count; return SOLAR_OS_NATIVE_OK;
}
static int ramfs_mount_point(const char *path, char *out, size_t capacity) { return solar_os_native_result(solar_os_ramfs_path_mount_point(path, out, capacity)); }
static int ramfs_usage(const char *path, solar_os_native_storage_usage_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); return solar_os_native_result(solar_os_ramfs_get_usage_for_path(path, &out->total, &out->used, &out->free));
}
static const solar_os_native_ramfs_api_v1_t ramfs_api = {
    .abi_version = SOLAR_OS_NATIVE_RAMFS_ABI, .struct_size = sizeof(ramfs_api), .mount = ramfs_mount, .unmount = ramfs_unmount,
    .count = solar_os_ramfs_mount_count, .info = ramfs_info, .has_mount_prefix = solar_os_ramfs_path_has_mount_prefix, .mount_point = ramfs_mount_point, .usage = ramfs_usage,
};
static const char *backup_default_path(void) { return SOLAR_OS_NVS_BACKUP_DEFAULT_PATH; }
static int backup_run(const char *path, solar_os_native_settings_backup_v1_t *out, bool restore)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_nvs_backup_result_t value = {0};
    int result = solar_os_native_result(restore ? solar_os_nvs_backup_restore(path, &value) : solar_os_nvs_backup_create(path, &value));
    /* A restore may require reboot even when reporting a later cleanup error. */
    out->partition_size = value.partition_size; out->file_size = value.file_size; out->crc32 = value.crc32; out->reboot_required = value.reboot_required;
    return result;
}
static int backup_create(const char *path, solar_os_native_settings_backup_v1_t *out) { return backup_run(path, out, false); }
static int backup_restore(const char *path, solar_os_native_settings_backup_v1_t *out) { return backup_run(path, out, true); }
static const solar_os_native_settings_backup_api_v1_t backup_api = {
    .abi_version = SOLAR_OS_NATIVE_SETTINGS_BACKUP_ABI, .struct_size = sizeof(backup_api), .default_path = backup_default_path, .create = backup_create, .restore = backup_restore,
};
#endif
const void *solar_os_native_storage_control_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_storage_control_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
