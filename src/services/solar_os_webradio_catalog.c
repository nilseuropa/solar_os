#include "solar_os_webradio_catalog.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_m3u.h"
#include "solar_os_storage.h"

#define WEBRADIO_NVS_NAMESPACE "webradio"
#define WEBRADIO_NVS_CATALOG_KEY "catalog"
#define WEBRADIO_CATALOG_MAGIC 0x57524144U
#define WEBRADIO_CATALOG_VERSION 1U
#define WEBRADIO_CATALOG_ROOT_DIR ".solar"
#define WEBRADIO_CATALOG_DIR ".solar/webradio"
#define WEBRADIO_CATALOG_FILE "playlist.m3u"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
} webradio_catalog_blob_t;

typedef struct {
    bool initialized;
    uint32_t generation;
    size_t count;
    solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    int32_t durations[SOLAR_OS_WEBRADIO_STATION_MAX];
} webradio_catalog_state_t;

typedef struct {
    webradio_catalog_blob_t blob;
    int32_t durations[SOLAR_OS_WEBRADIO_STATION_MAX];
} webradio_m3u_t;

static const char *TAG = "solar_os_webradio_catalog";
static EXT_RAM_BSS_ATTR webradio_catalog_state_t catalog;
static portMUX_TYPE catalog_lock = portMUX_INITIALIZER_UNLOCKED;

static const solar_os_webradio_station_t default_stations[] = {
    {"Nightride", "https://stream.nightride.fm/nightride.mp3"},
    {"Chillsynth", "https://stream.nightride.fm/chillsynth.mp3"},
    {"Datawave", "https://stream.nightride.fm/datawave.mp3"},
    {"Spacesynth", "https://stream.nightride.fm/spacesynth.mp3"},
    {"Darksynth", "https://stream.nightride.fm/darksynth.mp3"},
    {"Horrorsynth", "https://stream.nightride.fm/horrorsynth.mp3"},
    {"EBSM", "https://stream.nightride.fm/ebsm.mp3"},
    {"Rekt", "https://stream.nightride.fm/rekt.mp3"},
};

bool solar_os_webradio_url_valid(const char *url)
{
    if (url == NULL ||
        strnlen(url, SOLAR_OS_WEBRADIO_URL_MAX) >= SOLAR_OS_WEBRADIO_URL_MAX) {
        return false;
    }
    return strncmp(url, "https://", 8U) == 0 ||
        strncmp(url, "http://", 7U) == 0;
}

static bool webradio_station_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_WEBRADIO_STATION_NAME_MAX) >=
            SOLAR_OS_WEBRADIO_STATION_NAME_MAX) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor != '\0'; cursor++) {
        if (iscntrl(*cursor)) {
            return false;
        }
    }
    return true;
}

static void webradio_catalog_set_defaults(void)
{
    catalog.count = sizeof(default_stations) / sizeof(default_stations[0]);
    memcpy(catalog.stations, default_stations, sizeof(default_stations));
    for (size_t i = 0; i < SOLAR_OS_WEBRADIO_STATION_MAX; i++) catalog.durations[i] = -1;
    if (catalog.count < SOLAR_OS_WEBRADIO_STATION_MAX) {
        memset(&catalog.stations[catalog.count],
               0,
               (SOLAR_OS_WEBRADIO_STATION_MAX - catalog.count) *
                   sizeof(catalog.stations[0]));
    }
}

static bool webradio_catalog_blob_valid(const webradio_catalog_blob_t *blob)
{
    if (blob == NULL || blob->magic != WEBRADIO_CATALOG_MAGIC ||
        blob->version != WEBRADIO_CATALOG_VERSION ||
        blob->count > SOLAR_OS_WEBRADIO_STATION_MAX) {
        return false;
    }
    for (size_t i = 0U; i < blob->count; i++) {
        if (!webradio_station_name_valid(blob->stations[i].name) ||
            !solar_os_webradio_url_valid(blob->stations[i].url)) {
            return false;
        }
    }
    return true;
}

static esp_err_t webradio_catalog_prepare_path(void)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    static const char *const directories[] = {
        WEBRADIO_CATALOG_ROOT_DIR,
        WEBRADIO_CATALOG_DIR,
    };
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    for (size_t i = 0U; i < sizeof(directories) / sizeof(directories[0]); i++) {
        esp_err_t err = solar_os_storage_default_path(
            directories[i], path, sizeof(path));
        if (err != ESP_OK) {
            return err;
        }
        if (solar_os_storage_mkdir(path) != ESP_OK && errno != EEXIST) {
            return ESP_FAIL;
        }
    }

    return solar_os_storage_default_path(
        WEBRADIO_CATALOG_DIR "/" WEBRADIO_CATALOG_FILE,
        catalog.path,
        sizeof(catalog.path));
}

static bool webradio_catalog_load_file(webradio_catalog_blob_t *blob)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(WEBRADIO_CATALOG_DIR "/catalog.bin", path, sizeof(path)) != ESP_OK)
        return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    const bool loaded = fread(blob, sizeof(*blob), 1U, file) == 1U &&
        fgetc(file) == EOF && webradio_catalog_blob_valid(blob);
    fclose(file);
    return loaded;
}

static bool webradio_catalog_load_legacy_nvs(webradio_catalog_blob_t *blob)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WEBRADIO_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }
    size_t length = sizeof(*blob);
    err = nvs_get_blob(nvs, WEBRADIO_NVS_CATALOG_KEY, blob, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(*blob) &&
        webradio_catalog_blob_valid(blob);
}

static void webradio_catalog_erase_legacy_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WEBRADIO_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return;
    }
    err = nvs_erase_key(nvs, WEBRADIO_NVS_CATALOG_KEY);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        SOLAR_OS_LOGW(TAG, "legacy NVS catalog cleanup failed: %s",
                      esp_err_to_name(err));
    }
}

static esp_err_t webradio_m3u_accept(const solar_os_m3u_entry_t *entry, void *user)
{
    webradio_m3u_t *data = user;
    if (!solar_os_webradio_url_valid(entry->path)) return ESP_ERR_NOT_SUPPORTED;
    if (data->blob.count == SOLAR_OS_WEBRADIO_STATION_MAX) return ESP_ERR_INVALID_SIZE;
    size_t i = data->blob.count++;
    solar_os_webradio_station_t *station = &data->blob.stations[i];
    const char *name = entry->title[0] ? entry->title : entry->path;
    size_t name_len = strnlen(name, sizeof(station->name) - 1U);
    while (name_len && ((unsigned char)name[name_len] & 0xc0U) == 0x80U) name_len--;
    memcpy(station->name, name, name_len);
    station->name[name_len] = 0;
    strlcpy(station->url, entry->path, sizeof(station->url));
    data->durations[i] = entry->duration;
    return ESP_OK;
}

static bool webradio_m3u_get(size_t index, solar_os_m3u_entry_t *entry, void *user)
{
    webradio_m3u_t *data = user;
    if (index >= data->blob.count) return false;
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->path, data->blob.stations[index].url, sizeof(entry->path));
    strlcpy(entry->title, data->blob.stations[index].name, sizeof(entry->title));
    entry->duration = data->durations[index];
    return true;
}

static webradio_m3u_t *webradio_catalog_copy(void)
{
    webradio_m3u_t *data = solar_os_memory_calloc(1, sizeof(*data),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "webradio.catalog.edit");
    if (data) {
        portENTER_CRITICAL(&catalog_lock);
        data->blob.count = catalog.count;
        memcpy(data->blob.stations, catalog.stations, sizeof(catalog.stations));
        memcpy(data->durations, catalog.durations, sizeof(catalog.durations));
        portEXIT_CRITICAL(&catalog_lock);
    }
    return data;
}

static esp_err_t webradio_catalog_publish(webradio_m3u_t *data)
{
    esp_err_t err = solar_os_m3u_write(catalog.path, data->blob.count, webradio_m3u_get, data);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&catalog_lock);
        catalog.count = data->blob.count;
        memcpy(catalog.stations, data->blob.stations, sizeof(catalog.stations));
        memcpy(catalog.durations, data->durations, sizeof(catalog.durations));
        catalog.generation++;
        portEXIT_CRITICAL(&catalog_lock);
    }
    return err;
}

static esp_err_t webradio_catalog_persist(void)
{
    webradio_m3u_t *data = solar_os_memory_calloc(1, sizeof(*data),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "webradio.catalog.save");
    if (!data) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&catalog_lock);
    data->blob.count = catalog.count;
    memcpy(data->blob.stations, catalog.stations, sizeof(catalog.stations));
    memcpy(data->durations, catalog.durations, sizeof(catalog.durations));
    portEXIT_CRITICAL(&catalog_lock);
    esp_err_t err = solar_os_m3u_write(catalog.path, data->blob.count, webradio_m3u_get, data);
    solar_os_memory_free(data);
    return err;
}

int32_t solar_os_webradio_catalog_duration(size_t index)
{
    portENTER_CRITICAL(&catalog_lock);
    int32_t duration = index < catalog.count ? catalog.durations[index] : -1;
    portEXIT_CRITICAL(&catalog_lock);
    return duration;
}

esp_err_t solar_os_webradio_catalog_import(const char *path)
{
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG, "catalog init failed");
    webradio_m3u_t *data = solar_os_memory_calloc(1, sizeof(*data),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "webradio.catalog.import");
    if (!data) return ESP_ERR_NO_MEM;
    esp_err_t err = solar_os_m3u_read(path, webradio_m3u_accept, data);
    if (err == ESP_OK) err = webradio_catalog_publish(data);
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_webradio_catalog_init(void)
{
    portENTER_CRITICAL(&catalog_lock);
    const bool initialized = catalog.initialized;
    portEXIT_CRITICAL(&catalog_lock);
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(webradio_catalog_prepare_path(), TAG,
                        "catalog path setup failed");

    webradio_m3u_t *data =
        solar_os_memory_calloc(1,
                               sizeof(*data),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "webradio.catalog.load");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    webradio_catalog_blob_t *blob = &data->blob;
    for (size_t i = 0; i < SOLAR_OS_WEBRADIO_STATION_MAX; i++) data->durations[i] = -1;
    esp_err_t m3u_err = solar_os_m3u_read(catalog.path, webradio_m3u_accept, data);
    if (m3u_err != ESP_OK && m3u_err != ESP_ERR_NOT_FOUND) {
        solar_os_memory_free(data);
        return m3u_err;
    }
    const bool loaded_from_m3u = m3u_err == ESP_OK;
    const bool loaded_from_file = loaded_from_m3u || webradio_catalog_load_file(blob);
    const bool loaded_from_nvs = !loaded_from_file &&
        webradio_catalog_load_legacy_nvs(blob);
    const bool loaded = loaded_from_file || loaded_from_nvs;

    portENTER_CRITICAL(&catalog_lock);
    if (loaded) {
        catalog.count = blob->count;
        memcpy(catalog.durations, data->durations, sizeof(catalog.durations));
        memcpy(catalog.stations,
               blob->stations,
               catalog.count * sizeof(catalog.stations[0]));
        if (catalog.count < SOLAR_OS_WEBRADIO_STATION_MAX) {
            memset(&catalog.stations[catalog.count],
                   0,
                   (SOLAR_OS_WEBRADIO_STATION_MAX - catalog.count) *
                       sizeof(catalog.stations[0]));
        }
    } else {
        webradio_catalog_set_defaults();
    }
    catalog.generation = 1U;
    catalog.initialized = true;
    portEXIT_CRITICAL(&catalog_lock);
    solar_os_memory_free(data);

    if (!loaded_from_m3u) {
        const esp_err_t err = webradio_catalog_persist();
        if (err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "catalog file persistence failed: %s",
                          esp_err_to_name(err));
        } else {
            webradio_catalog_erase_legacy_nvs();
        }
    } else {
        webradio_catalog_erase_legacy_nvs();
    }
    return ESP_OK;
}

size_t solar_os_webradio_catalog_snapshot(
    solar_os_webradio_station_t *stations,
    size_t capacity,
    uint32_t *generation)
{
    portENTER_CRITICAL(&catalog_lock);
    const size_t count = catalog.count < capacity ? catalog.count : capacity;
    if (stations != NULL && count > 0U) {
        memcpy(stations, catalog.stations, count * sizeof(stations[0]));
    }
    if (generation != NULL) {
        *generation = catalog.generation;
    }
    portEXIT_CRITICAL(&catalog_lock);
    return count;
}

esp_err_t solar_os_webradio_catalog_add(const char *name, const char *url)
{
    if (!webradio_station_name_valid(name) || !solar_os_webradio_url_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG,
                        "catalog init failed");

    webradio_m3u_t *data = webradio_catalog_copy();
    if (!data) return ESP_ERR_NO_MEM;
    size_t index = data->blob.count;
    for (size_t i = 0; i < data->blob.count; i++) {
        if (strcasecmp(data->blob.stations[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index == data->blob.count && data->blob.count >= SOLAR_OS_WEBRADIO_STATION_MAX) {
        solar_os_memory_free(data);
        return ESP_ERR_NO_MEM;
    }
    if (index == data->blob.count) {
        data->blob.count++;
    }
    strlcpy(data->blob.stations[index].name,
            name,
            sizeof(data->blob.stations[index].name));
    strlcpy(data->blob.stations[index].url,
            url,
            sizeof(data->blob.stations[index].url));
    data->durations[index] = -1;
    esp_err_t err = webradio_catalog_publish(data);
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_webradio_catalog_update(const char *old_name,
                                           const char *name,
                                           const char *url)
{
    if (!webradio_station_name_valid(old_name) ||
        !webradio_station_name_valid(name) ||
        !solar_os_webradio_url_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG,
                        "catalog init failed");

    webradio_m3u_t *data = webradio_catalog_copy();
    if (!data) return ESP_ERR_NO_MEM;
    size_t index = data->blob.count;
    for (size_t i = 0U; i < data->blob.count; i++) {
        if (strcasecmp(data->blob.stations[i].name, old_name) == 0) {
            index = i;
            break;
        }
    }
    if (index == data->blob.count) {
        solar_os_memory_free(data);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0U; i < data->blob.count; i++) {
        if (i != index && strcasecmp(data->blob.stations[i].name, name) == 0) {
            solar_os_memory_free(data);
            return ESP_ERR_INVALID_STATE;
        }
    }
    strlcpy(data->blob.stations[index].name,
            name,
            sizeof(data->blob.stations[index].name));
    if (strcmp(data->blob.stations[index].url, url)) data->durations[index] = -1;
    strlcpy(data->blob.stations[index].url,
            url,
            sizeof(data->blob.stations[index].url));
    esp_err_t err = webradio_catalog_publish(data);
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_webradio_catalog_remove(const char *name)
{
    if (!webradio_station_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG,
                        "catalog init failed");

    webradio_m3u_t *data = webradio_catalog_copy();
    if (!data) return ESP_ERR_NO_MEM;
    size_t index = data->blob.count;
    for (size_t i = 0; i < data->blob.count; i++) {
        if (strcasecmp(data->blob.stations[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index == data->blob.count) {
        solar_os_memory_free(data);
        return ESP_ERR_NOT_FOUND;
    }
    if (index + 1U < data->blob.count) {
        memmove(&data->blob.stations[index], &data->blob.stations[index + 1U],
                (data->blob.count - index - 1U) * sizeof(data->blob.stations[0]));
        memmove(&data->durations[index], &data->durations[index + 1U],
                (data->blob.count - index - 1U) * sizeof(data->durations[0]));
    }
    data->blob.count--;
    esp_err_t err = webradio_catalog_publish(data);
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_webradio_catalog_reset(void)
{
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG,
                        "catalog init failed");
    webradio_m3u_t *data = solar_os_memory_calloc(1, sizeof(*data),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "webradio.catalog.reset");
    if (!data) return ESP_ERR_NO_MEM;
    data->blob.count = sizeof(default_stations) / sizeof(default_stations[0]);
    memcpy(data->blob.stations, default_stations, sizeof(default_stations));
    for (size_t i = 0; i < SOLAR_OS_WEBRADIO_STATION_MAX; i++) data->durations[i] = -1;
    esp_err_t err = webradio_catalog_publish(data);
    solar_os_memory_free(data);
    return err;
}
