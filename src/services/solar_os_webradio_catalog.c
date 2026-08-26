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
#include "solar_os_storage.h"

#define WEBRADIO_NVS_NAMESPACE "webradio"
#define WEBRADIO_NVS_CATALOG_KEY "catalog"
#define WEBRADIO_CATALOG_MAGIC 0x57524144U
#define WEBRADIO_CATALOG_VERSION 1U
#define WEBRADIO_CATALOG_ROOT_DIR ".solar"
#define WEBRADIO_CATALOG_DIR ".solar/webradio"
#define WEBRADIO_CATALOG_FILE "catalog.bin"

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
} webradio_catalog_state_t;

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
    FILE *file = fopen(catalog.path, "rb");
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

static esp_err_t webradio_catalog_persist(void)
{
    webradio_catalog_blob_t *blob =
        solar_os_memory_calloc(1,
                               sizeof(*blob),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "webradio.catalog.save");
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&catalog_lock);
    blob->magic = WEBRADIO_CATALOG_MAGIC;
    blob->version = WEBRADIO_CATALOG_VERSION;
    blob->count = (uint16_t)catalog.count;
    memcpy(blob->stations,
           catalog.stations,
           catalog.count * sizeof(catalog.stations[0]));
    portEXIT_CRITICAL(&catalog_lock);

    char temporary[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_sibling_path(
            catalog.path, ".tmp", temporary, sizeof(temporary)) != ESP_OK ||
        solar_os_storage_sibling_path(
            catalog.path, ".bak", backup, sizeof(backup)) != ESP_OK) {
        solar_os_memory_free(blob);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        solar_os_memory_free(blob);
        return ESP_FAIL;
    }
    esp_err_t err = fwrite(blob, sizeof(*blob), 1U, file) == 1U ?
        ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(temporary, catalog.path, backup);
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(temporary);
    }
    solar_os_memory_free(blob);
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

    webradio_catalog_blob_t *blob =
        solar_os_memory_calloc(1,
                               sizeof(*blob),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "webradio.catalog.load");
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const bool loaded_from_file = webradio_catalog_load_file(blob);
    const bool loaded_from_nvs = !loaded_from_file &&
        webradio_catalog_load_legacy_nvs(blob);
    const bool loaded = loaded_from_file || loaded_from_nvs;

    portENTER_CRITICAL(&catalog_lock);
    if (loaded) {
        catalog.count = blob->count;
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
    solar_os_memory_free(blob);

    if (!loaded_from_file) {
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

    portENTER_CRITICAL(&catalog_lock);
    size_t index = catalog.count;
    for (size_t i = 0; i < catalog.count; i++) {
        if (strcasecmp(catalog.stations[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index == catalog.count && catalog.count >= SOLAR_OS_WEBRADIO_STATION_MAX) {
        portEXIT_CRITICAL(&catalog_lock);
        return ESP_ERR_NO_MEM;
    }
    if (index == catalog.count) {
        catalog.count++;
    }
    strlcpy(catalog.stations[index].name,
            name,
            sizeof(catalog.stations[index].name));
    strlcpy(catalog.stations[index].url,
            url,
            sizeof(catalog.stations[index].url));
    catalog.generation++;
    portEXIT_CRITICAL(&catalog_lock);
    return webradio_catalog_persist();
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

    portENTER_CRITICAL(&catalog_lock);
    size_t index = catalog.count;
    for (size_t i = 0U; i < catalog.count; i++) {
        if (strcasecmp(catalog.stations[i].name, old_name) == 0) {
            index = i;
            break;
        }
    }
    if (index == catalog.count) {
        portEXIT_CRITICAL(&catalog_lock);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0U; i < catalog.count; i++) {
        if (i != index && strcasecmp(catalog.stations[i].name, name) == 0) {
            portEXIT_CRITICAL(&catalog_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    strlcpy(catalog.stations[index].name,
            name,
            sizeof(catalog.stations[index].name));
    strlcpy(catalog.stations[index].url,
            url,
            sizeof(catalog.stations[index].url));
    catalog.generation++;
    portEXIT_CRITICAL(&catalog_lock);
    return webradio_catalog_persist();
}

esp_err_t solar_os_webradio_catalog_remove(const char *name)
{
    if (!webradio_station_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG,
                        "catalog init failed");

    portENTER_CRITICAL(&catalog_lock);
    size_t index = catalog.count;
    for (size_t i = 0; i < catalog.count; i++) {
        if (strcasecmp(catalog.stations[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index == catalog.count) {
        portEXIT_CRITICAL(&catalog_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (index + 1U < catalog.count) {
        memmove(&catalog.stations[index],
                &catalog.stations[index + 1U],
                (catalog.count - index - 1U) * sizeof(catalog.stations[0]));
    }
    catalog.count--;
    memset(&catalog.stations[catalog.count], 0, sizeof(catalog.stations[0]));
    catalog.generation++;
    portEXIT_CRITICAL(&catalog_lock);
    return webradio_catalog_persist();
}

esp_err_t solar_os_webradio_catalog_reset(void)
{
    ESP_RETURN_ON_ERROR(solar_os_webradio_catalog_init(), TAG,
                        "catalog init failed");
    portENTER_CRITICAL(&catalog_lock);
    webradio_catalog_set_defaults();
    catalog.generation++;
    portEXIT_CRITICAL(&catalog_lock);
    return webradio_catalog_persist();
}
