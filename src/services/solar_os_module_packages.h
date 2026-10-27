#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MODULE_PACKAGE_ID_MAX 32U
#define SOLAR_OS_MODULE_PACKAGE_NAME_MAX 64U
#define SOLAR_OS_MODULE_PACKAGE_VERSION_MAX 32U
#define SOLAR_OS_MODULE_PACKAGE_DESCRIPTION_MAX 128U
#define SOLAR_OS_MODULE_PACKAGE_URL_MAX 320U
#define SOLAR_OS_MODULE_PACKAGE_SHA256_MAX 65U
#define SOLAR_OS_MODULE_PACKAGE_COUNT_MAX 16U
#define SOLAR_OS_MODULE_APP_LIFECYCLE_ABI 1U
#define SOLAR_OS_MODULE_FOREGROUND_LIFECYCLE_ABI 2U
#define SOLAR_OS_MODULE_JOB_LIFECYCLE_ABI 1U
#define SOLAR_OS_MODULE_DRIVER_LIFECYCLE_ABI 1U
#define SOLAR_OS_MODULE_LIBRARY_LIFECYCLE_ABI 1U

typedef enum {
    SOLAR_OS_MODULE_TYPE_INVALID = 0,
    SOLAR_OS_MODULE_TYPE_APP,
    SOLAR_OS_MODULE_TYPE_JOB,
    SOLAR_OS_MODULE_TYPE_DRIVER,
    SOLAR_OS_MODULE_TYPE_LIBRARY,
} solar_os_module_type_t;

typedef struct {
    solar_os_module_type_t type;
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    char name[SOLAR_OS_MODULE_PACKAGE_NAME_MAX];
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char description[SOLAR_OS_MODULE_PACKAGE_DESCRIPTION_MAX];
    char artifact_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    char artifact_sha256[SOLAR_OS_MODULE_PACKAGE_SHA256_MAX];
    uint32_t artifact_size;
    uint32_t lifecycle_abi;
    uint32_t minimum_host_api_size;
    bool compatible;
    bool installed;
    char incompatibility[96];
} solar_os_module_package_t;

typedef struct {
    char host_version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char source_commit[65];
    char target[16];
    uint32_t native_abi;
    bool signature_verified;
    size_t count;
    solar_os_module_package_t packages[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
} solar_os_module_catalog_t;

typedef bool (*solar_os_module_package_cancel_fn)(void *user);
typedef void (*solar_os_module_package_progress_fn)(uint32_t bytes,
                                                    uint32_t total,
                                                    void *user);

typedef struct {
    solar_os_module_package_cancel_fn should_cancel;
    solar_os_module_package_progress_fn progress;
    void *user;
} solar_os_module_install_options_t;

typedef struct {
    solar_os_module_type_t type;
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    uint32_t bytes;
} solar_os_module_install_result_t;

typedef bool (*solar_os_module_package_visit_fn)(const char *id, void *user);

esp_err_t solar_os_module_packages_init(void);

esp_err_t solar_os_module_catalog_fetch(solar_os_module_catalog_t **out_catalog,
                                        char *detail,
                                        size_t detail_len);
esp_err_t solar_os_module_catalog_fetch_ex(
    solar_os_module_catalog_t **out_catalog,
    solar_os_module_package_cancel_fn should_cancel,
    void *cancel_user,
    char *detail,
    size_t detail_len);
void solar_os_module_catalog_free(solar_os_module_catalog_t *catalog);
size_t solar_os_module_catalog_cached_count(void);
bool solar_os_module_catalog_cached_get(size_t index,
                                        char *id,
                                        size_t id_len);

esp_err_t solar_os_module_package_install(const char *id,
                                          const solar_os_module_install_options_t *options,
                                          solar_os_module_install_result_t *result,
                                          char *detail,
                                          size_t detail_len);
esp_err_t solar_os_module_package_remove(const char *id,
                                         char *detail,
                                         size_t detail_len);
const char *solar_os_module_type_name(solar_os_module_type_t type);
esp_err_t solar_os_module_package_path(solar_os_module_type_t type,
                                       const char *id,
                                       char *path,
                                       size_t path_len);
esp_err_t solar_os_module_package_foreach_installed(
    solar_os_module_type_t type,
    solar_os_module_package_visit_fn visit,
    void *user);

#ifdef __cplusplus
}
#endif
