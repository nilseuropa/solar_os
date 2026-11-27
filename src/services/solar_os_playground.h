#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"

#include "solar_os_native_playground_abi.h"

#define SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE \
    "https://raw.githubusercontent.com/nilseuropa/solar_os_playground/main/dist/catalog.json"

esp_err_t solar_os_playground_init(void);
void solar_os_playground_get_source(char *url, size_t url_len);
esp_err_t solar_os_playground_set_source(const char *url);
esp_err_t solar_os_playground_reset_source(void);
solar_os_playground_target_t solar_os_playground_get_storage(void);
esp_err_t solar_os_playground_set_storage(solar_os_playground_target_t target);

bool solar_os_playground_catalog_available(void);
esp_err_t solar_os_playground_delete(void);
esp_err_t solar_os_playground_reload(void);
esp_err_t solar_os_playground_refresh(volatile bool *cancel,
                                      solar_os_playground_progress_fn progress_fn,
                                      void *progress_user);
void solar_os_playground_cancel(void);
size_t solar_os_playground_category_count(void);
bool solar_os_playground_get_category(size_t index,
                                      solar_os_playground_category_t *category);
size_t solar_os_playground_app_count(void);
bool solar_os_playground_get_app(size_t index,
                                 solar_os_playground_app_info_t *app);
bool solar_os_playground_get_installed_app_id(size_t index,
                                              char *id,
                                              size_t id_len);
bool solar_os_playground_find_app(const char *id,
                                  size_t *index,
                                  solar_os_playground_app_info_t *app);
bool solar_os_playground_find_installed_app(
    const char *id,
    solar_os_playground_app_info_t *app);

bool solar_os_playground_is_installed(const solar_os_playground_app_info_t *app,
                                      char *version,
                                      size_t version_len);
esp_err_t solar_os_playground_entry_path(const solar_os_playground_app_info_t *app,
                                         char *path,
                                         size_t path_len);
esp_err_t solar_os_playground_install(const solar_os_playground_app_info_t *app,
                                      solar_os_playground_target_t target,
                                      volatile bool *cancel,
                                      solar_os_playground_progress_fn progress_fn,
                                      void *progress_user);
esp_err_t solar_os_playground_uninstall(
    const solar_os_playground_app_info_t *app,
    solar_os_playground_progress_fn progress_fn,
    void *progress_user);

const char *solar_os_playground_runtime_name(solar_os_playground_runtime_t runtime);
const char *solar_os_playground_target_name(solar_os_playground_target_t target);
