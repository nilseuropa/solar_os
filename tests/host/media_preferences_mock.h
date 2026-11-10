#pragma once
#include "solar_os_media_preferences.h"

struct solar_os_media_preferences { uint8_t volume, visualizer; };
static solar_os_media_preferences_t mock_media_preferences[4] = {{50, 0}, {50, 0}, {50, 0}, {50, 0}};
esp_err_t solar_os_media_preferences_create(solar_os_media_player_id_t id, solar_os_media_preferences_t **out)
{ assert(id < SOLAR_OS_MEDIA_PLAYER_COUNT); *out = &mock_media_preferences[id]; return ESP_OK; }
void solar_os_media_preferences_destroy(solar_os_media_preferences_t *p) { (void)p; }
uint8_t solar_os_media_preferences_volume(solar_os_media_preferences_t *p) { return p ? p->volume : 50; }
uint8_t solar_os_media_preferences_visualizer(solar_os_media_preferences_t *p) { return p ? p->visualizer : 0; }
esp_err_t solar_os_media_preferences_set_volume(solar_os_media_preferences_t *p, uint8_t volume)
{ assert(p && volume <= 100); p->volume = volume; return ESP_OK; }
esp_err_t solar_os_media_preferences_set_visualizer(solar_os_media_preferences_t *p, uint8_t visualizer)
{ assert(p); p->visualizer = visualizer; return ESP_OK; }
void solar_os_media_preferences_device(const solar_os_audio_device_info_t *device, void *p)
{ (void)device; (void)p; }
