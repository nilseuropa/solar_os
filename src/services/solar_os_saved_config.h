#pragma once

/* Checked mounted-storage settings with read-only legacy NVS migration.
 * Callers serialize this object and keep it for a boot to pin the volume. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvs.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

typedef struct { char path[SOLAR_OS_STORAGE_PATH_MAX]; } solar_os_saved_config_t;
typedef struct { uint32_t magic, size, checksum; } solar_os_saved_config_header_t;
typedef bool (*solar_os_saved_config_validator_t)(const void *, size_t);

static inline uint32_t solar_os_saved_config_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data; uint32_t hash = 2166136261U;
    while (size--) hash = (hash ^ *bytes++) * 16777619U;
    return hash;
}

static inline esp_err_t solar_os_saved_config_path(solar_os_saved_config_t *store, const char *name)
{
    if (store->path[0]) return ESP_OK;
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = solar_os_storage_default_path(".settings", directory, sizeof(directory));
    if (error != ESP_OK) return error;
    struct stat info;
    if (stat(directory, &info) != 0 && solar_os_storage_mkdir(directory) != ESP_OK) return ESP_FAIL;
    if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) return ESP_ERR_INVALID_STATE;
    if (snprintf(store->path, sizeof(store->path), "%s/%s.bin", directory, name) >= (int)sizeof(store->path)) {
        store->path[0] = 0; return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static inline esp_err_t solar_os_saved_config_read(const char *path, void *data, size_t capacity, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    solar_os_saved_config_header_t header;
    esp_err_t error = ESP_ERR_INVALID_CRC;
    if (fread(&header, sizeof(header), 1, file) == 1 && header.magic == 0x31474643 &&
        header.size && header.size <= capacity && fread(data, 1, header.size, file) == header.size &&
        fgetc(file) == EOF && !ferror(file) &&
        header.checksum == solar_os_saved_config_checksum(data, header.size)) {
        *length = header.size; error = ESP_OK;
    }
    if (fclose(file) != 0) error = ESP_FAIL;
    return error;
}

static inline esp_err_t solar_os_saved_config_save(solar_os_saved_config_t *store, const char *name,
    const void *data, size_t size)
{
    if (!data || !size || size > 16384) return ESP_ERR_INVALID_SIZE;
    esp_err_t error = solar_os_saved_config_path(store, name); if (error != ESP_OK) return error;
    char temp[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp", store->path) >= (int)sizeof(temp) ||
        snprintf(backup, sizeof(backup), "%s.bak", store->path) >= (int)sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    void *verify = solar_os_memory_calloc(1, size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "settings.verify");
    if (!verify) return ESP_ERR_NO_MEM;
    FILE *file = fopen(temp, "wb");
    error = ESP_FAIL;
    if (file) {
        solar_os_saved_config_header_t header = {0x31474643, size, solar_os_saved_config_checksum(data, size)};
        bool ok = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(data, 1, size, file) == size;
        if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
        if (fclose(file) != 0) ok = false;
        size_t length = 0;
        if (ok) error = solar_os_saved_config_read(temp, verify, size, &length);
        if (error == ESP_OK && (length != size || memcmp(data, verify, size))) error = ESP_ERR_INVALID_CRC;
        if (error == ESP_OK) error = solar_os_storage_replace_file(temp, store->path, backup);
        if (error == ESP_OK && unlink(backup) != 0 && errno != ENOENT) error = ESP_FAIL;
    }
    volatile uint8_t *bytes = verify; for (size_t i = 0; i < size; i++) bytes[i] = 0;
    solar_os_memory_free(verify);
    if (error != ESP_OK) unlink(temp);
    return error;
}

static inline esp_err_t solar_os_saved_config_cleanup(const solar_os_saved_config_t *store)
{
    if (!store->path[0]) return ESP_ERR_INVALID_STATE;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const char *suffixes[] = {".bak", ".tmp"};
    for (size_t i = 0; i < 2; i++) {
        if (snprintf(path, sizeof(path), "%s%s", store->path, suffixes[i]) >= (int)sizeof(path)) return ESP_ERR_INVALID_SIZE;
        if (unlink(path) != 0 && errno != ENOENT) return ESP_FAIL;
    }
    return ESP_OK;
}

static inline void solar_os_saved_config_erase_legacy(const char *space, const char *key)
{
    nvs_handle_t handle;
    /* Do not allocate an empty namespace on every boot. */
    if (nvs_open(space, NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = 0;
    esp_err_t error = nvs_get_blob(handle, key, NULL, &size); nvs_close(handle);
    if (error != ESP_OK || nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_erase_key(handle, key) == ESP_OK) (void)nvs_commit(handle);
    nvs_close(handle);
}

static inline esp_err_t solar_os_saved_config_load(solar_os_saved_config_t *store, const char *name,
    const char *space, const char *key, void *data, size_t capacity, size_t *length,
    solar_os_saved_config_validator_t valid)
{
    esp_err_t path_error = solar_os_saved_config_path(store, name);
    esp_err_t error = path_error;
    if (path_error == ESP_OK) {
        error = solar_os_saved_config_read(store->path, data, capacity, length);
        if (error != ESP_OK) {
            char backup[SOLAR_OS_STORAGE_PATH_MAX];
            if (snprintf(backup, sizeof(backup), "%s.bak", store->path) >= (int)sizeof(backup)) return ESP_ERR_INVALID_SIZE;
            esp_err_t recovered = solar_os_saved_config_read(backup, data, capacity, length);
            if (recovered == ESP_OK) {
                if (!valid(data, *length)) return ESP_ERR_INVALID_RESPONSE;
                if (unlink(store->path) != 0 && errno != ENOENT) return ESP_FAIL;
                if (rename(backup, store->path) != 0) return ESP_FAIL;
                error = ESP_OK;
            }
            else if (error != ESP_ERR_NOT_FOUND || recovered != ESP_ERR_NOT_FOUND) return error == ESP_ERR_NOT_FOUND ? recovered : error;
        }
        if (error == ESP_OK) {
            if (!valid(data, *length)) return ESP_ERR_INVALID_RESPONSE;
            solar_os_saved_config_erase_legacy(space, key); return ESP_OK;
        }
    }
    nvs_handle_t handle;
    error = nvs_open(space, NVS_READONLY, &handle); if (error != ESP_OK) return error;
    *length = capacity; error = nvs_get_blob(handle, key, data, length); nvs_close(handle);
    if (error != ESP_OK) return error;
    if (!valid(data, *length)) return ESP_ERR_INVALID_RESPONSE;
    /* Legacy settings can still be used if mounted storage is unavailable.
     * They are erased only after a checked file has been committed. */
    if (path_error == ESP_OK && solar_os_saved_config_save(store, name, data, *length) == ESP_OK)
        solar_os_saved_config_erase_legacy(space, key);
    return ESP_OK;
}
