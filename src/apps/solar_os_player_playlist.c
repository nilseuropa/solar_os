#include "solar_os_player_playlist.h"

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_media_playlist.h"

static EXT_RAM_BSS_ATTR solar_os_media_playlist_t playlist;
static SemaphoreHandle_t playlist_lock;
static bool initialized;

esp_err_t solar_os_player_playlist_init(void)
{
    if (!playlist_lock) playlist_lock = xSemaphoreCreateMutex();
    if (!playlist_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(playlist_lock, portMAX_DELAY);
    const esp_err_t err = initialized ? solar_os_media_playlist_refresh(&playlist) :
        solar_os_media_playlist_open(&playlist, SOLAR_OS_MEDIA_PLAYLIST_AUDIO, NULL, NULL);
    initialized = initialized || err == ESP_OK;
    xSemaphoreGive(playlist_lock);
    return err;
}

size_t solar_os_player_playlist_count(void)
{
    if (!initialized) return 0;
    xSemaphoreTake(playlist_lock, portMAX_DELAY);
    size_t count = playlist.count;
    xSemaphoreGive(playlist_lock);
    return count;
}

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
    if (err == ESP_OK && solar_os_m3u_path(path) && index) *index = 0;
    xSemaphoreGive(playlist_lock);
    return err;
}

esp_err_t solar_os_player_playlist_remove(size_t index)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(playlist_lock, portMAX_DELAY);
    esp_err_t err = solar_os_media_playlist_remove(&playlist, index);
    xSemaphoreGive(playlist_lock);
    return err;
}
