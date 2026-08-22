#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"

#define SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX 320U
#define SOLAR_OS_PLAYGROUND_CATEGORY_MAX 24U
#define SOLAR_OS_PLAYGROUND_APP_MAX 64U
#define SOLAR_OS_PLAYGROUND_ID_MAX 32U
#define SOLAR_OS_PLAYGROUND_NAME_MAX 48U
#define SOLAR_OS_PLAYGROUND_VERSION_MAX 16U
#define SOLAR_OS_PLAYGROUND_DESCRIPTION_MAX 128U
#define SOLAR_OS_PLAYGROUND_AUTHOR_MAX 48U
#define SOLAR_OS_PLAYGROUND_ENTRY_MAX 64U

#define SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE \
    "https://raw.githubusercontent.com/nilseuropa/solar_os_playground/main/dist/catalog.json"

typedef enum {
    SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON,
    SOLAR_OS_PLAYGROUND_RUNTIME_LUA,
} solar_os_playground_runtime_t;

typedef enum {
    SOLAR_OS_PLAYGROUND_TARGET_AUTO,
    SOLAR_OS_PLAYGROUND_TARGET_FLASH,
    SOLAR_OS_PLAYGROUND_TARGET_SD,
} solar_os_playground_target_t;

typedef enum {
    SOLAR_OS_PLAYGROUND_PROGRESS_CATALOG,
    SOLAR_OS_PLAYGROUND_PROGRESS_PACKAGE,
    SOLAR_OS_PLAYGROUND_PROGRESS_VERIFY,
    SOLAR_OS_PLAYGROUND_PROGRESS_INSTALL,
    SOLAR_OS_PLAYGROUND_PROGRESS_UNINSTALL,
    SOLAR_OS_PLAYGROUND_PROGRESS_DONE,
} solar_os_playground_progress_stage_t;

typedef struct {
    char id[SOLAR_OS_PLAYGROUND_ID_MAX];
    char title[SOLAR_OS_PLAYGROUND_NAME_MAX];
    uint32_t order;
} solar_os_playground_category_t;

typedef struct {
    char id[SOLAR_OS_PLAYGROUND_ID_MAX];
    char name[SOLAR_OS_PLAYGROUND_NAME_MAX];
    char version[SOLAR_OS_PLAYGROUND_VERSION_MAX];
    solar_os_playground_runtime_t runtime;
    char entry[SOLAR_OS_PLAYGROUND_ENTRY_MAX];
    char category[SOLAR_OS_PLAYGROUND_ID_MAX];
    char description[SOLAR_OS_PLAYGROUND_DESCRIPTION_MAX];
    char author[SOLAR_OS_PLAYGROUND_AUTHOR_MAX];
    char tags[96];
    char min_solaros[SOLAR_OS_PLAYGROUND_VERSION_MAX];
    char archive[128];
    char sha256[65];
    uint32_t size;
    bool compatible;
    char incompatibility[48];
} solar_os_playground_app_info_t;

typedef struct {
    solar_os_playground_progress_stage_t stage;
    uint32_t bytes_read;
    uint32_t bytes_total;
    bool total_known;
} solar_os_playground_progress_t;

typedef void (*solar_os_playground_progress_fn)(
    const solar_os_playground_progress_t *progress,
    void *user);

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
