#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_PLAYGROUND_SERVICE "playground"
#define SOLAR_OS_NATIVE_PLAYGROUND_ABI 1U

#define SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX 320U
#define SOLAR_OS_PLAYGROUND_CATEGORY_MAX 24U
#define SOLAR_OS_PLAYGROUND_APP_MAX 64U
#define SOLAR_OS_PLAYGROUND_ID_MAX 32U
#define SOLAR_OS_PLAYGROUND_NAME_MAX 48U
#define SOLAR_OS_PLAYGROUND_VERSION_MAX 16U
#define SOLAR_OS_PLAYGROUND_DESCRIPTION_MAX 128U
#define SOLAR_OS_PLAYGROUND_AUTHOR_MAX 48U
#define SOLAR_OS_PLAYGROUND_ENTRY_MAX 64U

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

#define SOLAR_OS_PLAYGROUND_CATALOG_RELOAD 0U
#define SOLAR_OS_PLAYGROUND_CATALOG_REFRESH 1U
#define SOLAR_OS_PLAYGROUND_DATA_DELETE 2U

/* Package operations use catalog IDs and the Core installer. Progress visits
 * borrow a record on the calling task. Cancel applies to in-flight refresh or
 * installation up to publication. The installer validates catalog artifacts
 * and publishes the extracted app only after validation. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*source)(char *url, size_t capacity);
    /* NULL restores the default catalog source. */
    int (*set_source)(const char *url);
    solar_os_playground_target_t (*storage)(void);
    int (*set_storage)(solar_os_playground_target_t target);
    bool (*catalog_available)(void);
    /* DATA_DELETE removes the cached catalog and all installed Playground apps
     * from the configured storage target. */
    int (*catalog)(uint32_t operation, solar_os_playground_progress_fn progress, void *user);
    void (*cancel)(void);
    size_t (*category_count)(void);
    bool (*category)(size_t index, solar_os_playground_category_t *category);
    size_t (*app_count)(void);
    bool (*app)(size_t index, solar_os_playground_app_info_t *app);
    bool (*find)(const char *id, size_t *index, solar_os_playground_app_info_t *app);
    bool (*find_installed)(const char *id, solar_os_playground_app_info_t *app);
    bool (*installed_id)(size_t app_index, char *id, size_t capacity);
    bool (*installed)(const char *id, char *version, size_t capacity);
    int (*entry_path)(const char *id, char *path, size_t capacity);
    int (*install)(const char *id, solar_os_playground_target_t target,
        solar_os_playground_progress_fn progress, void *user);
    int (*remove)(const char *id, solar_os_playground_progress_fn progress, void *user);
    const char *(*runtime_name)(solar_os_playground_runtime_t runtime);
    const char *(*target_name)(solar_os_playground_target_t target);
} solar_os_native_playground_api_v1_t;
