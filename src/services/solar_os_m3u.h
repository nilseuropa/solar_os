#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_storage.h"
#include "solar_os_native_media_types_abi.h"

typedef esp_err_t (*solar_os_m3u_read_fn)(const solar_os_m3u_entry_t *, void *);

bool solar_os_m3u_path(const char *path);
/* Local M3U/M3U8 files. Relative entries resolve against the playlist folder.
 * Callbacks may reject unsupported entries. HLS manifests are rejected. */
esp_err_t solar_os_m3u_read(const char *path, solar_os_m3u_read_fn accept, void *user);
esp_err_t solar_os_m3u_write(const char *path, size_t count,
                             solar_os_m3u_get_fn get, void *user);
