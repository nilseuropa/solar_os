#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"
#include "solar_os_components.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MODULE_PACKAGE_ID_MAX 32U
#define SOLAR_OS_MODULE_PACKAGE_NAME_MAX 64U
#define SOLAR_OS_MODULE_PACKAGE_VERSION_MAX 32U
#define SOLAR_OS_MODULE_PACKAGE_DESCRIPTION_MAX 128U
#define SOLAR_OS_MODULE_PACKAGE_URL_MAX 320U
#define SOLAR_OS_MODULE_PACKAGE_SHA256_MAX 65U
#define SOLAR_OS_MODULE_PACKAGE_COUNT_MAX 256U
#define SOLAR_OS_MODULE_DEPENDENCIES_MAX 8U
#define SOLAR_OS_MODULE_SERVICES_MAX 64U
#define SOLAR_OS_MODULE_APP_LIFECYCLE_ABI 1U
#define SOLAR_OS_MODULE_FOREGROUND_LIFECYCLE_ABI 2U
#define SOLAR_OS_MODULE_JOB_LIFECYCLE_ABI 1U
#define SOLAR_OS_MODULE_DRIVER_LIFECYCLE_ABI 1U
#define SOLAR_OS_MODULE_LIBRARY_LIFECYCLE_ABI 1U

typedef struct {
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
} solar_os_module_dependency_t;

typedef struct {
    char name[32];
    uint32_t abi;
    uint32_t minimum_struct_size;
} solar_os_module_service_requirement_t;

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
    uint8_t dependency_count;
    uint8_t service_count;
    solar_os_module_dependency_t dependencies[SOLAR_OS_MODULE_DEPENDENCIES_MAX];
    solar_os_module_service_requirement_t services[SOLAR_OS_MODULE_SERVICES_MAX];
    bool compatible;
    bool installed;
    bool compiled;
    char incompatibility[96];
} solar_os_module_package_t;

typedef struct {
    char host_version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char source_commit[65];
    char target[16];
    uint32_t native_abi;
    bool signature_verified;
    size_t count;
    /* Owned count-sized allocation; release with catalog_free. */
    solar_os_module_package_t *packages;
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
    bool compiled;
} solar_os_module_install_result_t;

typedef bool (*solar_os_module_package_visit_fn)(const char *id, void *user);

/* Receipts describe the bytes installed by pkg, including exact library pins.
 * A missing receipt is normal for files installed by older firmware. */
typedef struct {
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char sha256[SOLAR_OS_MODULE_PACKAGE_SHA256_MAX];
    uint32_t size;
    uint8_t dependency_count;
    solar_os_module_dependency_t dependencies[SOLAR_OS_MODULE_DEPENDENCIES_MAX];
} solar_os_module_receipt_t;

typedef struct {
    uint8_t package_index;
    bool installed;
    char previous_version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char previous_sha256[SOLAR_OS_MODULE_PACKAGE_SHA256_MAX];
    uint32_t previous_size;
} solar_os_module_upgrade_entry_t;

typedef enum {
    SOLAR_OS_MODULE_COMPATIBILITY_UNKNOWN,
    SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED,
    SOLAR_OS_MODULE_COMPATIBILITY_INCOMPATIBLE,
} solar_os_module_compatibility_t;

typedef struct {
    bool installed;
    bool compiled;
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    solar_os_module_compatibility_t compatibility;
} solar_os_module_installed_info_t;

typedef struct {
    solar_os_module_catalog_t *catalog;
    size_t count;
    size_t held_count;
    size_t verified_count;
    size_t unverified_count;
    size_t incompatible_count;
    uint32_t download_bytes;
    solar_os_module_upgrade_entry_t *entries;
    bool applied;
} solar_os_module_upgrade_plan_t;

esp_err_t solar_os_module_packages_init(void);

/* Read the saved signed catalog without network access. Verification and host
 * compatibility checks run again, and installed flags reflect local files. */
esp_err_t solar_os_module_catalog_load(solar_os_module_catalog_t **out_catalog,
                                       char *detail, size_t detail_len);
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

esp_err_t solar_os_module_package_receipt(solar_os_module_type_t type,
                                         const char *id,
                                         solar_os_module_receipt_t *receipt);
/* Verify installed bytes against a catalog already verified for this OS.
 * A differing or absent catalog entry leaves compatibility unknown. */
esp_err_t solar_os_module_package_inspect(const solar_os_module_catalog_t *catalog,
    solar_os_module_type_t type, const char *id,
    const solar_os_module_install_options_t *options, solar_os_module_installed_info_t *info);
/* Plan from the saved, verified catalog; NULL id selects all installed modules.
 * The plan owns the module-operation lock until freed. No artifacts are fetched
 * and no files are changed while planning or checking. */
esp_err_t solar_os_module_upgrade_plan(const char *id,
    const solar_os_module_install_options_t *options,
    solar_os_module_upgrade_plan_t **out_plan, char *detail, size_t detail_len);
esp_err_t solar_os_module_upgrade_check(const solar_os_module_upgrade_plan_t *plan,
    const solar_os_module_install_options_t *options, char *detail, size_t detail_len);
esp_err_t solar_os_module_upgrade_apply(solar_os_module_upgrade_plan_t *plan,
    const solar_os_module_install_options_t *options, size_t *completed,
    char *detail, size_t detail_len);
void solar_os_module_upgrade_plan_free(solar_os_module_upgrade_plan_t *plan);

#ifdef __cplusplus
}
#endif
