#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_webradio_catalog.h"

#define CATALOG_MAGIC 0x57524144U
#define CATALOG_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
} catalog_blob_t;

static char storage_root[] = "/tmp/solaros-webradio-XXXXXX";
static catalog_blob_t legacy_catalog = {
    .magic = CATALOG_MAGIC,
    .version = CATALOG_VERSION,
    .count = 1U,
    .stations = {{"Migrated", "https://example.net/migrated.mp3"}},
};
static bool legacy_present = true;
static unsigned legacy_erase_count;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0U) {
        const size_t copy = length >= size ? size - 1U : length;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "host error";
}

esp_err_t solar_os_log_write(solar_os_log_level_t level,
                             const char *tag,
                             const char *format,
                             ...)
{
    (void)level;
    (void)tag;
    (void)format;
    return ESP_OK;
}

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    (void)memory_class;
    (void)tag;
    return calloc(count, size);
}

void solar_os_memory_free(void *ptr)
{
    free(ptr);
}

bool solar_os_storage_is_mounted(void)
{
    return true;
}

esp_err_t solar_os_storage_default_path(const char *relative_path,
                                        char *path,
                                        size_t path_len)
{
    return snprintf(path, path_len, "%s/%s", storage_root, relative_path) <
            (int)path_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_storage_mkdir(const char *path)
{
    return mkdir(path, 0777) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_remove(const char *path)
{
    return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_rename(const char *old_path, const char *new_path)
{
    return rename(old_path, new_path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_sync_file(FILE *file)
{
    return file != NULL && fflush(file) == 0 && fsync(fileno(file)) == 0 ?
        ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_sibling_path(const char *path,
                                        const char *suffix,
                                        char *out,
                                        size_t out_len)
{
    const int written = snprintf(out, out_len, "%s%s", path, suffix);
    return written >= 0 && (size_t)written < out_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_storage_replace_file(const char *staged_path,
                                        const char *active_path,
                                        const char *backup_path)
{
    struct stat info;
    const bool had_active = stat(active_path, &info) == 0;
    (void)unlink(backup_path);
    if (had_active && rename(active_path, backup_path) != 0) {
        return ESP_FAIL;
    }
    if (rename(staged_path, active_path) != 0) {
        if (had_active) {
            (void)rename(backup_path, active_path);
        }
        return ESP_FAIL;
    }
    (void)unlink(backup_path);
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "webradio") == 0);
    (void)mode;
    *handle = 1U;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle,
                       const char *key,
                       void *value,
                       size_t *length)
{
    assert(handle == 1U);
    assert(strcmp(key, "catalog") == 0);
    if (!legacy_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    assert(*length >= sizeof(legacy_catalog));
    memcpy(value, &legacy_catalog, sizeof(legacy_catalog));
    *length = sizeof(legacy_catalog);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1U);
    assert(strcmp(key, "catalog") == 0);
    if (!legacy_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    legacy_present = false;
    legacy_erase_count++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1U);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1U);
}

static void assert_station(size_t index, const char *name, const char *url)
{
    solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
    const size_t count = solar_os_webradio_catalog_snapshot(
        stations, SOLAR_OS_WEBRADIO_STATION_MAX, NULL);
    assert(index < count);
    assert(strcmp(stations[index].name, name) == 0);
    assert(strcmp(stations[index].url, url) == 0);
}

static void assert_catalog_file(size_t expected_count)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/.solar/webradio/catalog.bin",
                    storage_root) < (int)sizeof(path));
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    catalog_blob_t blob;
    assert(fread(&blob, sizeof(blob), 1U, file) == 1U);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    assert(blob.magic == CATALOG_MAGIC);
    assert(blob.version == CATALOG_VERSION);
    assert(blob.count == expected_count);
}

int main(void)
{
    assert(mkdtemp(storage_root) != NULL);

    assert(solar_os_webradio_catalog_init() == ESP_OK);
    assert(!legacy_present);
    assert(legacy_erase_count == 1U);
    assert_station(0U, "Migrated", "https://example.net/migrated.mp3");
    assert_catalog_file(1U);

    assert(solar_os_webradio_catalog_add(
               "Second", "http://example.net/second.mp3") == ESP_OK);
    assert_station(1U, "Second", "http://example.net/second.mp3");
    assert_catalog_file(2U);

    assert(solar_os_webradio_catalog_update(
               "Second", "Updated", "https://example.net/updated.mp3") == ESP_OK);
    assert_station(1U, "Updated", "https://example.net/updated.mp3");
    assert(solar_os_webradio_catalog_remove("Migrated") == ESP_OK);
    assert_station(0U, "Updated", "https://example.net/updated.mp3");
    assert_catalog_file(1U);

    assert(solar_os_webradio_catalog_reset() == ESP_OK);
    assert_station(0U, "Nightride", "https://stream.nightride.fm/nightride.mp3");
    assert_catalog_file(8U);

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/.solar/webradio/catalog.bin",
                    storage_root) < (int)sizeof(path));
    assert(unlink(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/.solar/webradio", storage_root) <
        (int)sizeof(path));
    assert(rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/.solar", storage_root) <
        (int)sizeof(path));
    assert(rmdir(path) == 0);
    assert(rmdir(storage_root) == 0);

    puts("webradio catalog tests: ok");
    return 0;
}
