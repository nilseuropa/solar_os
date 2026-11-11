#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"
#include "solar_os_m3u.h"

#define SOLAR_OS_PLAYER_PLAYLIST_CAPACITY 32U

typedef solar_os_m3u_entry_t solar_os_player_track_t;

esp_err_t solar_os_player_playlist_init(void);
size_t solar_os_player_playlist_count(void);
uint32_t solar_os_player_playlist_generation(void);
bool solar_os_player_playlist_get(size_t index, solar_os_player_track_t *track);
esp_err_t solar_os_player_playlist_add(const char *path, size_t *index);
esp_err_t solar_os_player_playlist_remove(size_t index);
