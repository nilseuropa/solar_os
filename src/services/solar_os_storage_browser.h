#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"
#include "solar_os_m3u.h"
#include "solar_os_native_storage_browser_abi.h"

typedef struct solar_os_storage_browser solar_os_storage_browser_t;

esp_err_t solar_os_storage_browser_create(
    solar_os_storage_browser_filter_t filter,
    void *filter_user,
    solar_os_storage_browser_t **browser);
void solar_os_storage_browser_destroy(solar_os_storage_browser_t *browser);
esp_err_t solar_os_storage_browser_open(solar_os_storage_browser_t *browser,
                                        const char *path);
esp_err_t solar_os_storage_browser_open_filtered(solar_os_storage_browser_t *browser,
    const char *path, solar_os_storage_browser_filter_t filter, void *user);
esp_err_t solar_os_storage_browser_refresh(solar_os_storage_browser_t *browser);
const char *solar_os_storage_browser_path(
    const solar_os_storage_browser_t *browser);
size_t solar_os_storage_browser_count(const solar_os_storage_browser_t *browser);
size_t solar_os_storage_browser_cursor(const solar_os_storage_browser_t *browser);
bool solar_os_storage_browser_entry(const solar_os_storage_browser_t *browser,
                                    size_t index,
                                    solar_os_storage_browser_entry_t *entry);
/* Adapter for the shared media list renderer. */
bool solar_os_storage_browser_media_entry(size_t index, solar_os_m3u_entry_t *entry, void *browser);
void solar_os_storage_browser_move(solar_os_storage_browser_t *browser,
                                   int offset);
/* Opens a directory, or returns a normalized selected file path. */
esp_err_t solar_os_storage_browser_activate(solar_os_storage_browser_t *browser,
                                            char *selected,
                                            size_t selected_len,
                                            bool *file_selected);
esp_err_t solar_os_storage_browser_activate_filtered(solar_os_storage_browser_t *browser,
    char *selected, size_t selected_len, bool *file_selected,
    solar_os_storage_browser_filter_t filter, void *user);
/* Recorder and other clients can use this as their destination directory. */
esp_err_t solar_os_storage_browser_select_directory(
    const solar_os_storage_browser_t *browser,
    char *selected,
    size_t selected_len);
