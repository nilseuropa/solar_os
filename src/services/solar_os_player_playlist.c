#include "solar_os_player_playlist.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_media_playlist.h"
#include "solar_os_memory.h"

typedef struct {
    uint32_t magic;
    uint16_t version, count;
    uint32_t generation;
} player_playlist_store_t;

static EXT_RAM_BSS_ATTR solar_os_media_playlist_t playlist;
static SemaphoreHandle_t playlist_lock;
static bool initialized;
static uint32_t generation;

static esp_err_t migrate_legacy(void)
{
    bool exists;
    esp_err_t err = solar_os_storage_exists(playlist.path, &exists);
    if (err != ESP_OK || exists) return err;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_storage_default_path(".player/playlist.bin", path, sizeof(path));
    if (err != ESP_OK) return err;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_OK;
    player_playlist_store_t store;
    bool valid = fread(&store, sizeof(store), 1, file) == 1 &&
        store.magic == 0x50534f53U && store.version == 1U &&
        store.count <= SOLAR_OS_PLAYER_PLAYLIST_CAPACITY;
    if (valid && store.count) {
        playlist.entries = solar_os_memory_calloc(store.count, sizeof(*playlist.entries),
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "player.migrate");
        if (!playlist.entries) { fclose(file); return ESP_ERR_NO_MEM; }
        for (size_t i = 0; valid && i < store.count; i++) {
            solar_os_m3u_entry_t *entry = &playlist.entries[i];
            valid = fread(entry->path, SOLAR_OS_STORAGE_PATH_MAX, 1, file) == 1 &&
                memchr(entry->path, 0, SOLAR_OS_STORAGE_PATH_MAX) != NULL;
            entry->duration = -1;
        }
    }
    valid = valid && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!valid) {
        solar_os_memory_free(playlist.entries);
        playlist.entries = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    playlist.count = store.count;
    /* Keep the old file as a migration backup. M3U takes precedence. */
    return solar_os_media_playlist_save(&playlist);
}

esp_err_t solar_os_player_playlist_init(void)
{
    if (initialized) return ESP_OK;
    if (!playlist_lock) playlist_lock = xSemaphoreCreateMutex();
    if (!playlist_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = solar_os_media_playlist_open(&playlist, SOLAR_OS_MEDIA_PLAYLIST_AUDIO, NULL, NULL);
    if (err == ESP_OK) err = migrate_legacy();
    if (err != ESP_OK) { solar_os_media_playlist_close(&playlist); return err; }
    initialized = true;
    generation = 1;
    return ESP_OK;
}

size_t solar_os_player_playlist_count(void) { return initialized ? playlist.count : 0; }
uint32_t solar_os_player_playlist_generation(void) { return generation; }

bool solar_os_player_playlist_get(size_t index, solar_os_player_track_t *track)
{
    if (!initialized || !track) return false;
    xSemaphoreTake(playlist_lock, portMAX_DELAY);
    bool found = index < playlist.count;
    if (found) *track = playlist.entries[index];
    xSemaphoreGive(playlist_lock);
    return found;
}

esp_err_t solar_os_player_playlist_add(const char *path, size_t *index)
{
    if (!initialized || !path) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(playlist_lock, portMAX_DELAY);
    esp_err_t err = solar_os_m3u_path(path) ? solar_os_media_playlist_import(&playlist, path) :
        solar_os_media_playlist_add(&playlist, path, index);
    if (err == ESP_OK) {
        generation = generation == UINT32_MAX ? 1 : generation + 1;
        if (solar_os_m3u_path(path) && index) *index = 0;
    }
    xSemaphoreGive(playlist_lock);
    return err;
}

esp_err_t solar_os_player_playlist_remove(size_t index)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(playlist_lock, portMAX_DELAY);
    esp_err_t err = solar_os_media_playlist_remove(&playlist, index);
    if (err == ESP_OK) generation = generation == UINT32_MAX ? 1 : generation + 1;
    xSemaphoreGive(playlist_lock);
    return err;
}
