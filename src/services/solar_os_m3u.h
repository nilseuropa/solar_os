#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_storage.h"

#define SOLAR_OS_M3U_TITLE_MAX 96U
#define SOLAR_OS_M3U_PATH_MAX 320U
typedef struct {
    char path[SOLAR_OS_M3U_PATH_MAX];
    char title[SOLAR_OS_M3U_TITLE_MAX];
    int32_t duration; /* Seconds; -1 means unknown. Never a seek estimate. */
} solar_os_m3u_entry_t;

typedef esp_err_t (*solar_os_m3u_read_fn)(const solar_os_m3u_entry_t *, void *);
typedef bool (*solar_os_m3u_get_fn)(size_t, solar_os_m3u_entry_t *, void *);

bool solar_os_m3u_path(const char *path);
/* Local M3U/M3U8 files. Relative entries resolve against the playlist folder.
 * Callbacks may reject unsupported entries. HLS manifests are rejected. */
esp_err_t solar_os_m3u_read(const char *path, solar_os_m3u_read_fn accept, void *user);
esp_err_t solar_os_m3u_write(const char *path, size_t count,
                             solar_os_m3u_get_fn get, void *user);
