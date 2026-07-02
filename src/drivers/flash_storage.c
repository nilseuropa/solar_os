#include "flash_storage.h"

#include <stdio.h>
#include <string.h>

#include "diskio_wl.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#define FLASH_STORAGE_MAX_FILES 4
#define FLASH_STORAGE_ALLOC_UNIT_SIZE 4096

static const char *TAG = "flash_storage";

static wl_handle_t flash_wl_handle = WL_INVALID_HANDLE;
static uint8_t flash_logical_volume = FLASH_STORAGE_LOGICAL_VOLUME_INVALID;
static char flash_mount_point[FLASH_STORAGE_MOUNT_POINT_MAX];
static char flash_status[64] = "not mounted";

static const char *flash_storage_vfs_base_path(const char *mount_point)
{
    return strcmp(mount_point, FLASH_STORAGE_ROOT_MOUNT_POINT) == 0 ? "" : mount_point;
}

static void flash_storage_set_error_status(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        strlcpy(flash_status, "partition not found", sizeof(flash_status));
        break;
    case ESP_ERR_INVALID_STATE:
        strlcpy(flash_status, "already mounted", sizeof(flash_status));
        break;
    case ESP_ERR_NO_MEM:
        strlcpy(flash_status, "out of memory", sizeof(flash_status));
        break;
    default:
        snprintf(flash_status, sizeof(flash_status), "error %s", esp_err_to_name(err));
        break;
    }
}

esp_err_t flash_storage_mount(const char *mount_point)
{
    if (mount_point == NULL || mount_point[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(mount_point) >= sizeof(flash_mount_point)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (flash_storage_is_mounted()) {
        return strcmp(flash_mount_point, mount_point) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = FLASH_STORAGE_MAX_FILES,
        .allocation_unit_size = FLASH_STORAGE_ALLOC_UNIT_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = true,
    };

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    const char *vfs_base_path = flash_storage_vfs_base_path(mount_point);
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(vfs_base_path,
                                                     FLASH_STORAGE_PARTITION_LABEL,
                                                     &mount_config,
                                                     &wl_handle);
    if (ret != ESP_OK) {
        flash_storage_set_error_status(ret);
        ESP_LOGW(TAG, "flash mount at %s failed: %s", mount_point, esp_err_to_name(ret));
        return ret;
    }

    const uint8_t logical_volume = ff_diskio_get_pdrv_wl(wl_handle);
    if (logical_volume == 0xff) {
        (void)esp_vfs_fat_spiflash_unmount_rw_wl(vfs_base_path, wl_handle);
        flash_storage_set_error_status(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    flash_wl_handle = wl_handle;
    flash_logical_volume = logical_volume;
    strlcpy(flash_mount_point, mount_point, sizeof(flash_mount_point));
    snprintf(flash_status, sizeof(flash_status), "mounted at %s", mount_point);
    ESP_LOGI(TAG, "mounted %s at %s", FLASH_STORAGE_PARTITION_LABEL, mount_point);
    return ESP_OK;
}

esp_err_t flash_storage_unmount(void)
{
    if (!flash_storage_is_mounted()) {
        strlcpy(flash_status, "not mounted", sizeof(flash_status));
        return ESP_ERR_INVALID_STATE;
    }

    const char *vfs_base_path = flash_storage_vfs_base_path(flash_mount_point);
    esp_err_t ret = esp_vfs_fat_spiflash_unmount_rw_wl(vfs_base_path, flash_wl_handle);
    if (ret != ESP_OK) {
        flash_storage_set_error_status(ret);
        return ret;
    }

    flash_wl_handle = WL_INVALID_HANDLE;
    flash_logical_volume = FLASH_STORAGE_LOGICAL_VOLUME_INVALID;
    flash_mount_point[0] = '\0';
    strlcpy(flash_status, "not mounted", sizeof(flash_status));
    return ESP_OK;
}

bool flash_storage_is_mounted(void)
{
    return flash_wl_handle != WL_INVALID_HANDLE;
}

const char *flash_storage_mount_point(void)
{
    return flash_storage_is_mounted() ? flash_mount_point : "";
}

uint8_t flash_storage_logical_volume(void)
{
    return flash_logical_volume;
}

void flash_storage_get_status(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }
    strlcpy(buffer, flash_status, len);
}

esp_err_t flash_storage_get_usage(uint64_t *total_bytes,
                                  uint64_t *used_bytes,
                                  uint64_t *free_bytes)
{
    if (total_bytes == NULL || used_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!flash_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free_space = 0;
    esp_err_t ret = esp_vfs_fat_info(flash_storage_vfs_base_path(flash_mount_point),
                                     &total,
                                     &free_space);
    if (ret != ESP_OK) {
        return ret;
    }

    *total_bytes = total;
    *free_bytes = free_space;
    *used_bytes = total >= free_space ? total - free_space : 0;
    return ESP_OK;
}
