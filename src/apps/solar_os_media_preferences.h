#pragma once

#include "solar_os_audio.h"

typedef enum {
    SOLAR_OS_MEDIA_PLAYER,
    SOLAR_OS_MEDIA_WEBRADIO,
    SOLAR_OS_MEDIA_VPLAY,
    SOLAR_OS_MEDIA_RTSP,
    SOLAR_OS_MEDIA_PLAYER_COUNT,
} solar_os_media_player_id_t;

typedef struct solar_os_media_preferences solar_os_media_preferences_t;

esp_err_t solar_os_media_preferences_create(solar_os_media_player_id_t player,
                                            solar_os_media_preferences_t **preferences);
/* Save changed settings once when the app exits, then release them.
 * Workers and device callbacks must finish first. Save failures are logged. */
void solar_os_media_preferences_destroy(solar_os_media_preferences_t *preferences);
uint8_t solar_os_media_preferences_volume(solar_os_media_preferences_t *preferences);
uint8_t solar_os_media_preferences_visualizer(solar_os_media_preferences_t *preferences);
/* Apply session changes without storage writes. Volume errors concern only
 * the active audio device; both choices are persisted by destroy() on exit. */
esp_err_t solar_os_media_preferences_set_volume(solar_os_media_preferences_t *preferences, uint8_t volume);
esp_err_t solar_os_media_preferences_set_visualizer(solar_os_media_preferences_t *preferences, uint8_t visualizer);
/* Playback device callback: attach after opening, detach with NULL before
 * releasing the output. Idle controls then affect only this player's setting. */
void solar_os_media_preferences_device(const solar_os_audio_device_info_t *device, void *preferences);
