#include "solar_os_media_preferences.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_log.h"
#include "solar_os_saved_config.h"

typedef struct {
    uint8_t version, volume, visualizer, reserved;
} media_preferences_store_t;

struct solar_os_media_preferences {
    SemaphoreHandle_t lock;
    solar_os_saved_config_t file;
    solar_os_media_player_id_t player;
    uint8_t volume, visualizer;
    bool dirty;
    char device[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
};

static const char *const names[] = {"media-player", "media-webradio", "media-vplay", "media-rtsp"};
static const uint8_t visualizer_counts[] = {3, 2, 1, 1};

static esp_err_t media_preferences_save_locked(solar_os_media_preferences_t *preferences)
{
    esp_err_t err = solar_os_saved_config_path(&preferences->file, names[preferences->player]);
    if (err != ESP_OK) return err;
    char temporary[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_sibling_path(preferences->file.path, ".tmp", temporary, sizeof(temporary)) != ESP_OK ||
        solar_os_storage_sibling_path(preferences->file.path, ".bak", backup, sizeof(backup)) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    media_preferences_store_t store = {1, preferences->volume, preferences->visualizer, 0};
    solar_os_saved_config_header_t header = {
        0x31474643U, sizeof(store), solar_os_saved_config_checksum(&store, sizeof(store)),
    };
    FILE *file = fopen(temporary, "wb");
    if (!file) return ESP_FAIL;
    err = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(&store, sizeof(store), 1, file) == 1 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) err = solar_os_storage_sync_file(file);
    if (fclose(file) != 0) err = ESP_FAIL;
    /* This tiny record can be verified on the stack, including on boards
     * without PSRAM. Use the same checked format as other .settings files. */
    media_preferences_store_t verified;
    size_t length = 0;
    if (err == ESP_OK) err = solar_os_saved_config_read(temporary, &verified, sizeof(verified), &length);
    if (err == ESP_OK && (length != sizeof(store) || memcmp(&verified, &store, sizeof(store)))) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) err = solar_os_storage_replace_file(temporary, preferences->file.path, backup);
    if (err != ESP_OK) (void)solar_os_storage_remove(temporary);
    else preferences->dirty = false;
    return err;
}

esp_err_t solar_os_media_preferences_create(solar_os_media_player_id_t player,
                                            solar_os_media_preferences_t **out)
{
    if (!out || player < 0 || player >= SOLAR_OS_MEDIA_PLAYER_COUNT) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    solar_os_media_preferences_t *preferences = solar_os_memory_calloc(1, sizeof(*preferences),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "media.preferences");
    if (!preferences) return ESP_ERR_NO_MEM;
    preferences->lock = xSemaphoreCreateMutex();
    if (!preferences->lock) { solar_os_memory_free(preferences); return ESP_ERR_NO_MEM; }
    preferences->player = player;
    preferences->volume = 50;
    char relative[64], path[SOLAR_OS_STORAGE_PATH_MAX];
    snprintf(relative, sizeof(relative), ".settings/%s.bin", names[player]);
    /* Loading must not create .settings; defer all writes until app exit. */
    if (solar_os_storage_default_path(relative, path, sizeof(path)) == ESP_OK) {
        media_preferences_store_t store;
        size_t length = 0;
        if (solar_os_saved_config_read(path, &store, sizeof(store), &length) == ESP_OK &&
            length == sizeof(store) && store.version == 1 && !store.reserved && store.volume <= 100 &&
            store.visualizer < visualizer_counts[player]) {
            strlcpy(preferences->file.path, path, sizeof(preferences->file.path));
            preferences->volume = store.volume;
            preferences->visualizer = store.visualizer;
        }
    }
    *out = preferences;
    return ESP_OK;
}

void solar_os_media_preferences_destroy(solar_os_media_preferences_t *preferences)
{
    if (!preferences) return;
    xSemaphoreTake(preferences->lock, portMAX_DELAY);
    if (preferences->dirty) {
        esp_err_t err = media_preferences_save_locked(preferences);
        if (err != ESP_OK)
            SOLAR_OS_LOGW("media.preferences", "cannot save %s settings (%d)", names[preferences->player], (int)err);
    }
    xSemaphoreGive(preferences->lock);
    vSemaphoreDelete(preferences->lock);
    solar_os_memory_free(preferences);
}

uint8_t solar_os_media_preferences_volume(solar_os_media_preferences_t *preferences)
{
    if (!preferences) return 50;
    xSemaphoreTake(preferences->lock, portMAX_DELAY);
    uint8_t volume = preferences->volume;
    xSemaphoreGive(preferences->lock);
    return volume;
}

uint8_t solar_os_media_preferences_visualizer(solar_os_media_preferences_t *preferences)
{
    if (!preferences) return 0;
    xSemaphoreTake(preferences->lock, portMAX_DELAY);
    uint8_t visualizer = preferences->visualizer;
    xSemaphoreGive(preferences->lock);
    return visualizer;
}

esp_err_t solar_os_media_preferences_set_volume(solar_os_media_preferences_t *preferences, uint8_t volume)
{
    if (!preferences || volume > 100) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(preferences->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (volume != preferences->volume) {
        if (preferences->device[0]) err = solar_os_audio_set_device_volume(preferences->device, volume);
        if (err == ESP_OK) {
            preferences->volume = volume;
            preferences->dirty = true;
        }
    }
    xSemaphoreGive(preferences->lock);
    return err;
}

esp_err_t solar_os_media_preferences_set_visualizer(solar_os_media_preferences_t *preferences, uint8_t visualizer)
{
    if (!preferences || visualizer >= visualizer_counts[preferences->player]) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(preferences->lock, portMAX_DELAY);
    if (visualizer != preferences->visualizer) {
        preferences->visualizer = visualizer;
        preferences->dirty = true;
    }
    xSemaphoreGive(preferences->lock);
    return ESP_OK;
}

void solar_os_media_preferences_device(const solar_os_audio_device_info_t *device, void *user)
{
    solar_os_media_preferences_t *preferences = user;
    if (!preferences) return;
    xSemaphoreTake(preferences->lock, portMAX_DELAY);
    strlcpy(preferences->device, device ? device->id : "", sizeof(preferences->device));
    if (device && solar_os_audio_set_device_volume(device->id, preferences->volume) != ESP_OK)
        SOLAR_OS_LOGW("media.preferences", "cannot apply player volume to %s", device->id);
    xSemaphoreGive(preferences->lock);
}
