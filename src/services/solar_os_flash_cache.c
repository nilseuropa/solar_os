#include "solar_os_flash_cache.h"
#include "solar_os_flash.h"

#include <errno.h>
#include <string.h>

static bool cache_media(const solar_os_storage_mount_info_t *mount)
{
    return mount->type == SOLAR_OS_STORAGE_MOUNT_SD ||
        mount->type == SOLAR_OS_STORAGE_MOUNT_USB;
}

esp_err_t solar_os_flash_cache_root(char *root, size_t capacity)
{
    if (!root || !capacity) return ESP_ERR_INVALID_ARG;
    root[0] = '\0';
    solar_os_storage_mount_info_t selected = {0};
    const size_t count = solar_os_storage_mount_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_mount_info_t mount;
        if (!solar_os_storage_get_mount(i, &mount) || !cache_media(&mount)) continue;
        /* Prefer SD, then the first mount path alphabetically within a type. */
        if (!selected.mount_point[0] || mount.type < selected.type ||
            (mount.type == selected.type && strcmp(mount.mount_point, selected.mount_point) < 0))
            selected = mount;
    }
    if (!selected.mount_point[0]) return ESP_ERR_INVALID_STATE;
    return solar_os_storage_join_path(selected.mount_point, ".flash", root, capacity);
}

static bool cache_volume(const char *root, solar_os_storage_mount_info_t *volume)
{
    if (!root || !root[0]) return false;
    const size_t count = solar_os_storage_mount_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_mount_info_t mount;
        char candidate[SOLAR_OS_STORAGE_PATH_MAX];
        if (solar_os_storage_get_mount(i, &mount) && cache_media(&mount) &&
            solar_os_storage_join_path(mount.mount_point, ".flash", candidate, sizeof(candidate)) == ESP_OK &&
            !strcmp(candidate, root)) {
            if (volume) *volume = mount;
            return true;
        }
    }
    return false;
}

bool solar_os_flash_cache_mounted(const char *root) { return cache_volume(root, NULL); }

esp_err_t solar_os_flash_cache_open(const char *root, solar_os_flash_cache_t *cache)
{
    if (!cache) return ESP_ERR_INVALID_ARG;
    memset(cache, 0, sizeof(*cache));
    esp_err_t err = ESP_OK;
    if (root) {
        if (strlcpy(cache->root, root, sizeof(cache->root)) >= sizeof(cache->root))
            return ESP_ERR_INVALID_SIZE;
    } else err = solar_os_flash_cache_root(cache->root, sizeof(cache->root));
    solar_os_storage_mount_info_t volume;
    if (err == ESP_OK && !cache_volume(cache->root, &volume)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        cache->volume = opendir(volume.mount_point);
        if (!cache->volume) err = ESP_FAIL;
    }
    return err;
}

esp_err_t solar_os_flash_cache_check(const solar_os_flash_cache_t *cache)
{
    if (!cache || !cache->volume || !cache_volume(cache->root, NULL)) return ESP_ERR_INVALID_STATE;
    errno = 0;
    (void)readdir(cache->volume);
    return errno ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void solar_os_flash_cache_close(solar_os_flash_cache_t *cache)
{
    if (cache && cache->volume) { closedir(cache->volume); cache->volume = NULL; }
}
