#pragma once

#include "solar_os_native_components_abi.h"

#define SOLAR_OS_NATIVE_PACKAGES_SERVICE "packages"
#define SOLAR_OS_NATIVE_PACKAGES_ABI 1U
#define SOLAR_OS_NATIVE_PACKAGE_PATH_MAX 160U
#define SOLAR_OS_NATIVE_PACKAGE_UPGRADE_PREVIEW 0U
#define SOLAR_OS_NATIVE_PACKAGE_UPGRADE_CHECK 1U
#define SOLAR_OS_NATIVE_PACKAGE_UPGRADE_APPLY 2U

#define SOLAR_OS_MODULE_PACKAGE_ID_MAX 32U
#define SOLAR_OS_MODULE_PACKAGE_NAME_MAX 64U
#define SOLAR_OS_MODULE_PACKAGE_VERSION_MAX 32U
#define SOLAR_OS_MODULE_PACKAGE_DESCRIPTION_MAX 128U
#define SOLAR_OS_MODULE_PACKAGE_URL_MAX 320U
#define SOLAR_OS_MODULE_PACKAGE_SHA256_MAX 65U
#define SOLAR_OS_MODULE_PACKAGE_COUNT_MAX 256U
#define SOLAR_OS_MODULE_DEPENDENCIES_MAX 8U
#define SOLAR_OS_MODULE_SERVICES_MAX 64U

typedef struct {
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
} solar_os_native_package_dependency_v1_t;

typedef struct {
    char name[32];
    uint32_t abi;
    uint32_t minimum_struct_size;
} solar_os_native_package_service_requirement_v1_t;

typedef struct {
    uint32_t type;
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
    solar_os_native_package_dependency_v1_t dependencies[SOLAR_OS_MODULE_DEPENDENCIES_MAX];
    solar_os_native_package_service_requirement_v1_t services[SOLAR_OS_MODULE_SERVICES_MAX];
    bool compatible;
    bool installed;
    bool compiled;
    char incompatibility[96];
} solar_os_native_package_entry_v1_t;

typedef solar_os_native_cancel_fn solar_os_native_package_cancel_fn;
typedef void (*solar_os_native_package_progress_fn)(uint32_t bytes,
                                                    uint32_t total,
                                                    void *user);

typedef struct {
    solar_os_native_package_cancel_fn should_cancel;
    solar_os_native_package_progress_fn progress;
    void *user;
} solar_os_native_package_install_options_v1_t;

typedef struct {
    uint32_t type;
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char path[SOLAR_OS_NATIVE_PACKAGE_PATH_MAX];
    uint32_t bytes;
    bool compiled;
} solar_os_native_package_install_result_v1_t;

typedef bool (*solar_os_native_package_installed_visit_fn)(const char *id, void *user);

/* Receipts describe the bytes installed by pkg, including exact library pins.
 * A missing receipt is normal for files installed by older firmware. */
typedef struct {
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char sha256[SOLAR_OS_MODULE_PACKAGE_SHA256_MAX];
    uint32_t size;
    uint8_t dependency_count;
    solar_os_native_package_dependency_v1_t dependencies[SOLAR_OS_MODULE_DEPENDENCIES_MAX];
} solar_os_native_package_receipt_v1_t;

#define SOLAR_OS_MODULE_COMPATIBILITY_UNKNOWN 0U
#define SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED 1U
#define SOLAR_OS_MODULE_COMPATIBILITY_INCOMPATIBLE 2U

typedef struct {
    bool installed;
    bool compiled;
    char version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    uint32_t compatibility;
} solar_os_native_package_installed_info_v1_t;

typedef struct {
    char host_version[SOLAR_OS_MODULE_PACKAGE_VERSION_MAX];
    char source_commit[65];
    char target[16];
    uint32_t native_abi;
    bool signature_verified;
    size_t count;
} solar_os_native_package_catalog_info_v1_t;

typedef struct {
    uint32_t type;
    const char *id;
    const char *version;
    const char *previous_version;
    const char *previous_sha256;
    uint32_t artifact_size, previous_size;
    bool installed;
} solar_os_native_package_upgrade_entry_v1_t;

typedef struct {
    size_t count, held, verified, unverified, incompatible;
    uint32_t download_bytes;
    size_t completed;
} solar_os_native_package_upgrade_result_v1_t;

typedef struct {
    solar_os_native_package_install_options_v1_t transfer;
    bool (*visit)(const solar_os_native_package_upgrade_entry_v1_t *entry, void *user);
    bool (*confirm)(const solar_os_native_package_upgrade_result_v1_t *plan, void *user);
    void *user;
} solar_os_native_package_upgrade_options_v1_t;

/* A catalog handle owns an immutable verified snapshot. Refresh uses the
 * configured repository. Open and mutation callbacks are synchronous and
 * borrowed only until their operation returns. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*catalog_open)(bool refresh, solar_os_native_package_cancel_fn cancel, void *user,
        solar_os_native_handle_t *catalog, char *detail, size_t capacity);
    int (*catalog_info)(solar_os_native_handle_t catalog, solar_os_native_package_catalog_info_v1_t *info);
    int (*catalog_entry)(solar_os_native_handle_t catalog, size_t index, solar_os_native_package_entry_v1_t *entry);
    int (*catalog_close)(solar_os_native_handle_t catalog);
    size_t (*cached_count)(void);
    bool (*cached_id)(size_t index, char *id, size_t capacity);
    int (*install)(const char *id, const solar_os_native_package_install_options_v1_t *options,
        solar_os_native_package_install_result_v1_t *result, char *detail, size_t capacity);
    int (*remove)(const char *id, char *detail, size_t capacity);
    const char *(*type_name)(uint32_t type);
    int (*path)(uint32_t type, const char *id, char *path, size_t capacity);
    int (*visit_installed)(uint32_t type, solar_os_native_package_installed_visit_fn visit, void *user);
    int (*receipt)(uint32_t type, const char *id, solar_os_native_package_receipt_v1_t *receipt);
    /* Zero catalog inspects local state without catalog verification. */
    int (*inspect)(solar_os_native_handle_t catalog, uint32_t type, const char *id,
        const solar_os_native_package_install_options_v1_t *options, solar_os_native_package_installed_info_v1_t *info);
    /* NULL id selects all installed modules. Visit and confirm see the same
     * plan that can be applied while the package-operation lock is held.
     * Entry strings are borrowed only during the visit. Returning false
     * cancels the operation. Preview and check do no artifact downloads. */
    int (*upgrade)(const char *id, uint32_t action, const solar_os_native_package_upgrade_options_v1_t *options,
        solar_os_native_package_upgrade_result_v1_t *result, char *detail, size_t capacity);
} solar_os_native_packages_api_v1_t;
