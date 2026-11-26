#pragma once

#include "solar_os_m3u.h"
#include "solar_os_native_m3u_abi.h"

typedef struct {
    solar_os_m3u_entry_t *entries;
    size_t count;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_media_playlist_kind_t kind;
    uint32_t revision;
} solar_os_media_playlist_t;

/* Argument imports an M3U or adds one source. NULL opens the saved list.
 * Imported files stay unchanged; edits persist in the per-player default. */
esp_err_t solar_os_media_playlist_open(solar_os_media_playlist_t *list,
                                       solar_os_media_playlist_kind_t kind,
                                       const char *argument, size_t *selected);
void solar_os_media_playlist_close(solar_os_media_playlist_t *list);
/* Reload a saved list after another session edits it. */
esp_err_t solar_os_media_playlist_refresh(solar_os_media_playlist_t *list);
esp_err_t solar_os_media_playlist_import(solar_os_media_playlist_t *list, const char *path);
esp_err_t solar_os_media_playlist_add(solar_os_media_playlist_t *list, const char *path, size_t *index);
/* Replaces one entry, or appends when index == count. Saves before returning. */
esp_err_t solar_os_media_playlist_set(solar_os_media_playlist_t *list, size_t index,
                                      const solar_os_m3u_entry_t *entry);
esp_err_t solar_os_media_playlist_remove(solar_os_media_playlist_t *list, size_t index);
