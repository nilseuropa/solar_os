#include "solar_os_module_packages.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "solar_os_config.h"
#include "solar_os_board_caps.h"
#include "solar_os_crypto.h"
#include "solar_os_http_client.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_elf.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_driver_abi.h"
#include "solar_os_native_job_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_foreground.h"
#include "solar_os_ota_key.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#ifndef SOLAR_OS_MODULE_REPOSITORY_URL
#define SOLAR_OS_MODULE_REPOSITORY_URL "https://solar-os.eu/ota/modules"
#endif

#define MODULE_CATALOG_MAX_BYTES (32U * 1024U)
#define MODULE_CATALOG_SCHEMA_VERSION 2U
#define MODULE_SIGNATURE_MAX_BYTES 512U
#define MODULE_HTTP_TIMEOUT_MS 15000U
#define MODULE_HTTP_DEADLINE_MS 60000U

static atomic_bool module_operation_running;

typedef struct {
    size_t count;
    char ids[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX][SOLAR_OS_MODULE_PACKAGE_ID_MAX];
} module_catalog_id_cache_t;

static EXT_RAM_BSS_ATTR module_catalog_id_cache_t module_catalog_id_cache;
static portMUX_TYPE module_catalog_cache_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    FILE *file;
    solar_os_crypto_sha256_t sha256;
    const solar_os_module_install_options_t *options;
    uint32_t expected_size;
    uint32_t bytes;
    int response_status;
    bool response_size_mismatch;
} module_download_t;

static void module_set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "module operation failed");
    }
}

static bool module_name_valid(const char *text)
{
    if (text == NULL || text[0] == '\0' || strlen(text) >= SOLAR_OS_MODULE_PACKAGE_ID_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') {
            return false;
        }
    }
    return strcmp(text, ".") != 0 && strcmp(text, "..") != 0;
}

const char *solar_os_module_type_name(solar_os_module_type_t type)
{
    switch (type) {
    case SOLAR_OS_MODULE_TYPE_APP:
        return "app";
    case SOLAR_OS_MODULE_TYPE_JOB:
        return "job";
    case SOLAR_OS_MODULE_TYPE_DRIVER:
        return "driver";
    case SOLAR_OS_MODULE_TYPE_LIBRARY:
        return "library";
    default:
        return "invalid";
    }
}

static const char *module_type_directory(solar_os_module_type_t type)
{
    switch (type) {
    case SOLAR_OS_MODULE_TYPE_APP:
        return "apps";
    case SOLAR_OS_MODULE_TYPE_JOB:
        return "jobs";
    case SOLAR_OS_MODULE_TYPE_DRIVER:
        return "drivers";
    case SOLAR_OS_MODULE_TYPE_LIBRARY:
        return "libraries";
    default:
        return NULL;
    }
}

static esp_err_t module_legacy_app_path(const char *id,
                                        char *path,
                                        size_t path_len)
{
    if (!module_name_valid(id) || path == NULL || path_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char relative[64];
    const int written = snprintf(relative,
                                 sizeof(relative),
                                 "modules/%s.app.elf",
                                 id);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_storage_default_path(relative, path, path_len);
}

static solar_os_module_type_t module_type_parse(const char *name)
{
    if (name != NULL && strcmp(name, "app") == 0) {
        return SOLAR_OS_MODULE_TYPE_APP;
    }
    if (name != NULL && strcmp(name, "job") == 0) {
        return SOLAR_OS_MODULE_TYPE_JOB;
    }
    if (name != NULL && strcmp(name, "driver") == 0) {
        return SOLAR_OS_MODULE_TYPE_DRIVER;
    }
    if (name != NULL && strcmp(name, "library") == 0) {
        return SOLAR_OS_MODULE_TYPE_LIBRARY;
    }
    return SOLAR_OS_MODULE_TYPE_INVALID;
}

static bool module_relative_path_valid(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/' || strstr(path, "..") != NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.' && *p != '/') {
            return false;
        }
    }
    return true;
}

static esp_err_t module_join_url(const char *base,
                                 const char *path,
                                 char *out,
                                 size_t out_len)
{
    if (base == NULL || path == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t base_len = strlen(base);
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 base,
                                 base_len > 0U && base[base_len - 1U] == '/' ? "" : "/",
                                 path[0] == '/' ? path + 1 : path);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t module_release_url(char *out, size_t out_len)
{
    return module_join_url(SOLAR_OS_MODULE_REPOSITORY_URL,
                           SOLAR_OS_VERSION,
                           out,
                           out_len);
}

static esp_err_t module_fetch_body(const char *url,
                                   size_t max_bytes,
                                   solar_os_http_buffered_response_t *response,
                                   solar_os_module_package_cancel_fn should_cancel,
                                   void *cancel_user)
{
    const solar_os_http_request_options_t request = {
        .url = url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .user_agent = "SolarOS-pkg/" SOLAR_OS_VERSION,
        .follow_redirects = true,
        .timeout_ms = MODULE_HTTP_TIMEOUT_MS,
        .deadline_ms = MODULE_HTTP_DEADLINE_MS,
        .should_cancel = should_cancel,
        .cancel_user_data = cancel_user,
        .receive_buffer_size = 2048U,
        .transmit_buffer_size = 1024U,
    };
    esp_err_t err = solar_os_http_perform_buffered(&request, max_bytes + 1U, response);
    if (err != ESP_OK) {
        return err;
    }
    if (response->response.status_code != 200) {
        return ESP_ERR_NOT_FOUND;
    }
    if (response->body_truncated || response->body_len > max_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    response->body[response->body_len] = '\0';
    return ESP_OK;
}

static esp_err_t module_verify_catalog_signature(const uint8_t *catalog,
                                                 size_t catalog_len,
                                                 const char *signature)
{
    uint8_t der[SOLAR_OS_CRYPTO_ECDSA_P256_DER_SIGNATURE_MAX];
    size_t der_len = 0U;
    esp_err_t err = solar_os_crypto_base64_decode(signature,
                                                  der,
                                                  sizeof(der),
                                                  &der_len);
    if (err == ESP_OK) {
        err = solar_os_crypto_ecdsa_p256_sha256_verify_pem(
            SOLAR_OS_OTA_PUBLIC_KEY_PEM,
            catalog,
            catalog_len,
            der,
            der_len);
    }
    return err;
}

static bool module_capability_available(const char *required)
{
    char available[SOLAR_OS_BOARD_CAPABILITIES_TEXT_MAX];
    if (required == NULL ||
        !solar_os_board_capabilities_format(available, sizeof(available))) {
        return false;
    }

    const size_t required_len = strlen(required);
    const char *cursor = available;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        const char *end = strchr(cursor, ' ');
        const size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (len == required_len && memcmp(cursor, required, len) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

static esp_err_t module_parse_package(const solar_os_json_value_t *value,
                                      const char *release_url,
                                      solar_os_module_package_t *package)
{
    if (!solar_os_json_is_object(value) || package == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(package, 0, sizeof(*package));

    char artifact_path[192];
    char type_name[16];
    esp_err_t err = solar_os_json_get_path_string(value, "type",
                                                   type_name, sizeof(type_name));
    if (err == ESP_OK) {
        package->type = module_type_parse(type_name);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "id",
                                            package->id, sizeof(package->id));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "name",
                                            package->name, sizeof(package->name));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "version",
                                            package->version, sizeof(package->version));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "description",
                                            package->description,
                                            sizeof(package->description));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value,
                                            "lifecycle_abi",
                                            &package->lifecycle_abi);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value,
                                            "minimum_host_api_size",
                                            &package->minimum_host_api_size);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "artifact.path",
                                            artifact_path, sizeof(artifact_path));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value, "artifact.size",
                                            &package->artifact_size);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "artifact.sha256",
                                            package->artifact_sha256,
                                            sizeof(package->artifact_sha256));
    }
    char expected_artifact_path[192];
    const int expected_artifact_path_len = snprintf(
        expected_artifact_path,
        sizeof(expected_artifact_path),
        "%s/%s/%s/%s.elf",
        solar_os_module_type_name(package->type),
        package->id,
        package->version,
        package->artifact_sha256);
    if (err != ESP_OK || package->type == SOLAR_OS_MODULE_TYPE_INVALID ||
        !module_name_valid(package->id) ||
        package->name[0] == '\0' || !module_name_valid(package->version) ||
        package->lifecycle_abi == 0U ||
        package->minimum_host_api_size == 0U ||
        !module_relative_path_valid(artifact_path) ||
        expected_artifact_path_len < 0 ||
        (size_t)expected_artifact_path_len >= sizeof(expected_artifact_path) ||
        strcmp(artifact_path, expected_artifact_path) != 0 ||
        package->artifact_size < 52U ||
        package->artifact_size > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        !solar_os_crypto_sha256_hex_is_valid(package->artifact_sha256) ||
        module_join_url(release_url,
                        artifact_path,
                        package->artifact_url,
                        sizeof(package->artifact_url)) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    package->compatible = true;
    uint32_t supported_lifecycle_abi = 0U;
    size_t host_api_size = 0U;
    switch (package->type) {
    case SOLAR_OS_MODULE_TYPE_APP:
        supported_lifecycle_abi = package->lifecycle_abi == SOLAR_OS_MODULE_FOREGROUND_LIFECYCLE_ABI ?
            SOLAR_OS_MODULE_FOREGROUND_LIFECYCLE_ABI : SOLAR_OS_MODULE_APP_LIFECYCLE_ABI;
        host_api_size = sizeof(solar_os_native_host_api_v1_t);
        break;
    case SOLAR_OS_MODULE_TYPE_JOB:
        supported_lifecycle_abi = SOLAR_OS_MODULE_JOB_LIFECYCLE_ABI;
        host_api_size = sizeof(solar_os_native_job_host_api_v1_t);
        break;
    case SOLAR_OS_MODULE_TYPE_LIBRARY:
        supported_lifecycle_abi = SOLAR_OS_MODULE_LIBRARY_LIFECYCLE_ABI;
        host_api_size = sizeof(solar_os_native_library_host_api_v1_t);
        break;
    case SOLAR_OS_MODULE_TYPE_DRIVER:
        supported_lifecycle_abi = SOLAR_OS_MODULE_DRIVER_LIFECYCLE_ABI;
        host_api_size = sizeof(solar_os_native_driver_host_api_v1_t);
#if !SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        package->compatible = false;
        snprintf(package->incompatibility,
                 sizeof(package->incompatibility),
                 "needs expansion service");
#endif
        break;
    default:
        break;
    }
    if (package->compatible &&
        package->lifecycle_abi != supported_lifecycle_abi) {
        package->compatible = false;
        snprintf(package->incompatibility,
                 sizeof(package->incompatibility),
                 "needs %s lifecycle ABI %u",
                 solar_os_module_type_name(package->type),
                 (unsigned)package->lifecycle_abi);
    } else if (package->compatible &&
               package->minimum_host_api_size > host_api_size) {
        package->compatible = false;
        snprintf(package->incompatibility,
                 sizeof(package->incompatibility),
                 "needs host API size %u",
                 (unsigned)package->minimum_host_api_size);
    }

    const solar_os_json_value_t *capabilities =
        solar_os_json_path_get(value, "required_capabilities");
    if (!solar_os_json_is_array(capabilities)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t capability_count = solar_os_json_array_size(capabilities);
    for (size_t i = 0U; i < capability_count; i++) {
        char capability[40];
        if (solar_os_json_get_string(solar_os_json_array_get(capabilities, i),
                                     capability,
                                     sizeof(capability)) != ESP_OK ||
            capability[0] == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (package->compatible && !module_capability_available(capability)) {
            package->compatible = false;
            snprintf(package->incompatibility,
                     sizeof(package->incompatibility),
                     "needs %s capability",
                     capability);
        }
    }

    char installed_path[SOLAR_OS_STORAGE_PATH_MAX];
    bool installed = false;
    if (solar_os_module_package_path(package->type,
                                     package->id,
                                     installed_path,
                                     sizeof(installed_path)) == ESP_OK) {
        (void)solar_os_storage_exists(installed_path, &installed);
    }
    package->installed = installed;
    return ESP_OK;
}

static esp_err_t module_parse_catalog(const uint8_t *body,
                                      size_t body_len,
                                      const char *release_url,
                                      solar_os_module_catalog_t *catalog)
{
    solar_os_json_doc_t *document = NULL;
    esp_err_t err = solar_os_json_parse((const char *)body, body_len, &document);
    if (err != ESP_OK) {
        return err;
    }

    const solar_os_json_value_t *root = solar_os_json_root(document);
    char schema[40];
    char project[16];
    uint32_t schema_version = 0U;
    err = solar_os_json_get_path_string(root, "schema", schema, sizeof(schema));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(root, "schema_version", &schema_version);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "project", project, sizeof(project));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "host.version",
                                            catalog->host_version,
                                            sizeof(catalog->host_version));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "host.source_commit",
                                            catalog->source_commit,
                                            sizeof(catalog->source_commit));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "host.target",
                                            catalog->target,
                                            sizeof(catalog->target));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(root, "host.native_abi",
                                            &catalog->native_abi);
    }
    if (err != ESP_OK || strcmp(schema, "solaros.module_catalog") != 0 ||
        schema_version != MODULE_CATALOG_SCHEMA_VERSION ||
        strcmp(project, "SolarOS") != 0 ||
        catalog->host_version[0] == '\0' || catalog->source_commit[0] == '\0' ||
        catalog->target[0] == '\0' ||
        strcmp(catalog->host_version, SOLAR_OS_VERSION) != 0 ||
        strcmp(catalog->target, CONFIG_IDF_TARGET) != 0 ||
        catalog->native_abi != SOLAR_OS_NATIVE_ABI_VERSION) {
        solar_os_json_free(document);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const solar_os_json_value_t *modules = solar_os_json_path_get(root, "modules");
    const size_t count = solar_os_json_array_size(modules);
    if (!solar_os_json_is_array(modules) || count > SOLAR_OS_MODULE_PACKAGE_COUNT_MAX) {
        solar_os_json_free(document);
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0U; i < count; i++) {
        err = module_parse_package(solar_os_json_array_get(modules, i),
                                   release_url,
                                   &catalog->packages[i]);
        if (err != ESP_OK) {
            solar_os_json_free(document);
            return err;
        }
        for (size_t previous = 0U; previous < i; previous++) {
            if (strcmp(catalog->packages[previous].id,
                       catalog->packages[i].id) == 0) {
                solar_os_json_free(document);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    catalog->count = count;
    solar_os_json_free(document);
    return ESP_OK;
}

static esp_err_t module_catalog_fetch(
    solar_os_module_catalog_t **out_catalog,
    solar_os_module_package_cancel_fn should_cancel,
    void *cancel_user,
    char *detail,
    size_t detail_len)
{
    if (out_catalog == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_catalog = NULL;
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }

    char release_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    char catalog_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    char signature_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    if (module_release_url(release_url, sizeof(release_url)) != ESP_OK ||
        module_join_url(release_url, "catalog.json",
                        catalog_url, sizeof(catalog_url)) != ESP_OK ||
        module_join_url(release_url, "catalog.sig",
                        signature_url, sizeof(signature_url)) != ESP_OK) {
        module_set_detail(detail, detail_len, "module repository URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    solar_os_http_buffered_response_t catalog_response;
    solar_os_http_buffered_response_t signature_response;
    memset(&catalog_response, 0, sizeof(catalog_response));
    memset(&signature_response, 0, sizeof(signature_response));
    esp_err_t err = module_fetch_body(catalog_url,
                                      MODULE_CATALOG_MAX_BYTES,
                                      &catalog_response,
                                      should_cancel,
                                      cancel_user);
    if (err == ESP_OK) {
        err = module_fetch_body(signature_url,
                                MODULE_SIGNATURE_MAX_BYTES,
                                &signature_response,
                                should_cancel,
                                cancel_user);
    }
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not download the module catalog");
        goto done;
    }

    err = module_verify_catalog_signature(catalog_response.body,
                                          catalog_response.body_len,
                                          (const char *)signature_response.body);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "module catalog signature is invalid");
        goto done;
    }

    solar_os_module_catalog_t *catalog = solar_os_memory_calloc(
        1U,
        sizeof(*catalog),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "modules.catalog");
    if (catalog == NULL) {
        err = ESP_ERR_NO_MEM;
        module_set_detail(detail, detail_len, "not enough external memory for the catalog");
        goto done;
    }
    err = module_parse_catalog(catalog_response.body,
                               catalog_response.body_len,
                               release_url,
                               catalog);
    if (err != ESP_OK) {
        solar_os_memory_free(catalog);
        module_set_detail(detail, detail_len,
                          err == ESP_ERR_NOT_SUPPORTED ?
                              "catalog does not match this firmware, target, ABI, or schema" :
                              "module catalog is invalid");
        goto done;
    }
    catalog->signature_verified = true;
    portENTER_CRITICAL(&module_catalog_cache_lock);
    module_catalog_id_cache.count = catalog->count;
    for (size_t i = 0U; i < catalog->count; i++) {
        strlcpy(module_catalog_id_cache.ids[i],
                catalog->packages[i].id,
                sizeof(module_catalog_id_cache.ids[i]));
    }
    portEXIT_CRITICAL(&module_catalog_cache_lock);
    *out_catalog = catalog;

done:
    solar_os_http_buffered_response_clear(&signature_response);
    solar_os_http_buffered_response_clear(&catalog_response);
    return err;
}

esp_err_t solar_os_module_catalog_fetch(solar_os_module_catalog_t **out_catalog,
                                        char *detail,
                                        size_t detail_len)
{
    return solar_os_module_catalog_fetch_ex(out_catalog,
                                            NULL,
                                            NULL,
                                            detail,
                                            detail_len);
}

esp_err_t solar_os_module_catalog_fetch_ex(
    solar_os_module_catalog_t **out_catalog,
    solar_os_module_package_cancel_fn should_cancel,
    void *cancel_user,
    char *detail,
    size_t detail_len)
{
    return module_catalog_fetch(out_catalog,
                                should_cancel,
                                cancel_user,
                                detail,
                                detail_len);
}

void solar_os_module_catalog_free(solar_os_module_catalog_t *catalog)
{
    solar_os_memory_free(catalog);
}

size_t solar_os_module_catalog_cached_count(void)
{
    portENTER_CRITICAL(&module_catalog_cache_lock);
    const size_t count = module_catalog_id_cache.count;
    portEXIT_CRITICAL(&module_catalog_cache_lock);
    return count;
}

bool solar_os_module_catalog_cached_get(size_t index,
                                        char *id,
                                        size_t id_len)
{
    if (id == NULL || id_len == 0U) {
        return false;
    }
    portENTER_CRITICAL(&module_catalog_cache_lock);
    const bool found = index < module_catalog_id_cache.count;
    if (found) {
        strlcpy(id, module_catalog_id_cache.ids[index], id_len);
    }
    portEXIT_CRITICAL(&module_catalog_cache_lock);
    return found;
}

esp_err_t solar_os_module_package_path(solar_os_module_type_t type,
                                       const char *id,
                                       char *path,
                                       size_t path_len)
{
    const char *directory = module_type_directory(type);
    if (directory == NULL || !module_name_valid(id) ||
        path == NULL || path_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char relative[64];
    const int written = snprintf(relative,
                                 sizeof(relative),
                                 "modules/%s/%s.elf",
                                 directory,
                                 id);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_storage_default_path(relative, path, path_len);
}

esp_err_t solar_os_module_package_foreach_installed(
    solar_os_module_type_t type,
    solar_os_module_package_visit_fn visit,
    void *user)
{
    static const char suffix[] = ".elf";
    char modules_path[SOLAR_OS_STORAGE_PATH_MAX];
    const char *directory = module_type_directory(type);

    if (directory == NULL || visit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char relative[32];
    const int written = snprintf(relative,
                                 sizeof(relative),
                                 "modules/%s",
                                 directory);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = solar_os_storage_default_path(relative,
                                                  modules_path,
                                                  sizeof(modules_path));
    if (err != ESP_OK) {
        return err;
    }

    errno = 0;
    DIR *dir = opendir(modules_path);
    if (dir == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const size_t name_len = strlen(entry->d_name);
        const size_t suffix_len = sizeof(suffix) - 1U;
        if (name_len <= suffix_len ||
            strcmp(&entry->d_name[name_len - suffix_len], suffix) != 0) {
            continue;
        }

        const size_t id_len = name_len - suffix_len;
        if (id_len >= SOLAR_OS_MODULE_PACKAGE_ID_MAX) {
            continue;
        }
        char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
        memcpy(id, entry->d_name, id_len);
        id[id_len] = '\0';
        if (!module_name_valid(id)) {
            continue;
        }

        char path[SOLAR_OS_STORAGE_PATH_MAX];
        solar_os_storage_metadata_t metadata;
        if (solar_os_module_package_path(type, id, path, sizeof(path)) != ESP_OK ||
            solar_os_storage_stat(path, &metadata) != ESP_OK ||
            metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
            continue;
        }
        if (!visit(id, user)) {
            break;
        }
    }

    closedir(dir);
    return ESP_OK;
}

typedef struct {
    solar_os_module_type_t type;
    esp_err_t first_error;
} module_init_visit_t;

static bool module_activate_installed(const char *id, void *user)
{
    module_init_visit_t *visit = (module_init_visit_t *)user;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char detail[128] = {0};
    esp_err_t err = solar_os_module_package_path(visit->type,
                                                 id,
                                                 path,
                                                 sizeof(path));
    if (err == ESP_OK) {
        err = solar_os_native_module_activate(visit->type,
                                              id,
                                              path,
                                              detail,
                                              sizeof(detail));
    }
    if (err != ESP_OK) {
        SOLAR_OS_LOGW("module_packages",
                      "could not activate %s %s: %s%s%s",
                      solar_os_module_type_name(visit->type),
                      id,
                      esp_err_to_name(err),
                      detail[0] != '\0' ? " - " : "",
                      detail);
        if (visit->first_error == ESP_OK) {
            visit->first_error = err;
        }
    }
    return true;
}

esp_err_t solar_os_module_packages_init(void)
{
    module_init_visit_t visit = {
        .first_error = ESP_OK,
    };
    static const solar_os_module_type_t resident_types[] = {
        /* Libraries stay on storage until a consumer acquires them. */
        SOLAR_OS_MODULE_TYPE_JOB,
        SOLAR_OS_MODULE_TYPE_DRIVER,
    };
    for (size_t i = 0U; i < sizeof(resident_types) / sizeof(resident_types[0]); i++) {
        visit.type = resident_types[i];
        const esp_err_t err = solar_os_module_package_foreach_installed(
            visit.type,
            module_activate_installed,
            &visit);
        if (err != ESP_OK && visit.first_error == ESP_OK) {
            visit.first_error = err;
        }
    }
    return visit.first_error;
}

static bool module_download_cancelled(void *user)
{
    module_download_t *download = (module_download_t *)user;
    return download != NULL && download->options != NULL &&
        download->options->should_cancel != NULL &&
        download->options->should_cancel(download->options->user);
}

static esp_err_t module_download_event(const solar_os_http_event_t *event, void *user)
{
    module_download_t *download = (module_download_t *)user;
    if (event == NULL || download == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_RESPONSE) {
        download->response_status = event->status_code;
        download->response_size_mismatch =
            event->content_length >= 0 &&
            (uint64_t)event->content_length != download->expected_size;
        if (event->status_code != 200 || download->response_size_mismatch) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA || event->data_len == 0U) {
        return ESP_OK;
    }
    if (download->bytes > download->expected_size ||
        event->data_len > download->expected_size - download->bytes ||
        fwrite(event->data, 1U, event->data_len, download->file) != event->data_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = solar_os_crypto_sha256_update(&download->sha256,
                                                  event->data,
                                                  event->data_len);
    if (err != ESP_OK) {
        return err;
    }
    download->bytes += (uint32_t)event->data_len;
    if (download->options != NULL && download->options->progress != NULL) {
        download->options->progress(download->bytes,
                                    download->expected_size,
                                    download->options->user);
    }
    return ESP_OK;
}

static esp_err_t module_validate_download(const char *path,
                                          uint32_t size,
                                          const solar_os_module_package_t *package,
                                          char *detail,
                                          size_t detail_len)
{
    uint8_t *data = solar_os_memory_alloc(size,
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "modules.validate");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = 0U;
    esp_err_t err = solar_os_storage_read_file(path, data, size, &read_len);
    if (err == ESP_OK && read_len != size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        solar_os_native_elf_info_t info;
        err = solar_os_native_elf_validate(data,
                                           size,
                                           SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                           &info,
                                           detail,
                                           detail_len);
        if (err == ESP_OK && package->type == SOLAR_OS_MODULE_TYPE_APP &&
            ((package->lifecycle_abi == SOLAR_OS_MODULE_FOREGROUND_LIFECYCLE_ABI) != info.foreground ||
             (info.foreground && strcmp(info.app.id, package->id) != 0))) {
            module_set_detail(detail, detail_len, "ELF app identity or lifecycle differs from the signed catalog");
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_module_package_install(const char *id,
                                          const solar_os_module_install_options_t *options,
                                          solar_os_module_install_result_t *result,
                                          char *detail,
                                          size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (!module_name_valid(id)) {
        module_set_detail(detail, detail_len, "invalid module name");
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&module_operation_running, true)) {
        module_set_detail(detail, detail_len, "another module operation is running");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    solar_os_module_catalog_t *catalog = NULL;
    err = module_catalog_fetch(&catalog,
                               options != NULL ? options->should_cancel : NULL,
                               options != NULL ? options->user : NULL,
                               detail,
                               detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    const solar_os_module_package_t *package = NULL;
    for (size_t i = 0U; i < catalog->count; i++) {
        if (strcmp(catalog->packages[i].id, id) == 0) {
            package = &catalog->packages[i];
            break;
        }
    }
    if (package == NULL) {
        module_set_detail(detail, detail_len, "module is not present in the catalog");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (!package->compatible) {
        module_set_detail(detail, detail_len, package->incompatibility);
        err = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    if (package->type == SOLAR_OS_MODULE_TYPE_APP && solar_os_native_foreground_active(id)) {
        module_set_detail(detail, detail_len, "close the app session before replacing its module");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (package->type != SOLAR_OS_MODULE_TYPE_APP &&
        solar_os_native_module_active(package->type, id)) {
        module_set_detail(detail,
                          detail_len,
                          "remove the active resident module before reinstalling it");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    char module_dir[SOLAR_OS_STORAGE_PATH_MAX];
    char active_path[SOLAR_OS_STORAGE_PATH_MAX];
    char staged_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_module_package_path(package->type,
                                     id,
                                     active_path,
                                     sizeof(active_path)) != ESP_OK ||
        solar_os_storage_sibling_path(active_path, ".tmp",
                                      staged_path, sizeof(staged_path)) != ESP_OK ||
        solar_os_storage_sibling_path(active_path, ".bak",
                                      backup_path, sizeof(backup_path)) != ESP_OK) {
        module_set_detail(detail, detail_len, "module storage path is too long");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    const char *type_directory = module_type_directory(package->type);
    char module_relative[32];
    const int module_relative_len = snprintf(module_relative,
                                             sizeof(module_relative),
                                             "modules/%s",
                                             type_directory != NULL ?
                                                 type_directory : "invalid");
    if (module_relative_len < 0 ||
        (size_t)module_relative_len >= sizeof(module_relative) ||
        solar_os_storage_default_path(module_relative,
                                      module_dir,
                                      sizeof(module_dir)) != ESP_OK) {
        module_set_detail(detail, detail_len, "module storage path is too long");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    err = solar_os_storage_makedirs(module_dir, true);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not create the module directory");
        goto done;
    }
    (void)solar_os_storage_remove(staged_path);

    FILE *file = fopen(staged_path, "wb");
    if (file == NULL) {
        module_set_detail(detail, detail_len, "could not create the staged module file");
        err = ESP_FAIL;
        goto done;
    }
    module_download_t download = {
        .file = file,
        .options = options,
        .expected_size = package->artifact_size,
    };
    solar_os_crypto_sha256_init(&download.sha256);
    err = solar_os_crypto_sha256_start(&download.sha256);

    const solar_os_http_request_options_t request = {
        .url = package->artifact_url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .user_agent = "SolarOS-pkg/" SOLAR_OS_VERSION,
        .follow_redirects = true,
        .timeout_ms = MODULE_HTTP_TIMEOUT_MS,
        .read_poll_ms = 250U,
        .deadline_ms = MODULE_HTTP_DEADLINE_MS,
        .should_cancel = module_download_cancelled,
        .cancel_user_data = &download,
        .receive_buffer_size = 4096U,
        .transmit_buffer_size = 1024U,
        .event_handler = module_download_event,
        .user_data = &download,
    };
    solar_os_http_request_t *http = NULL;
    solar_os_http_response_t response;
    if (err == ESP_OK) {
        err = solar_os_http_request_create(&request, &http);
    }
    if (err == ESP_OK) {
        err = solar_os_http_request_perform(http, &response);
    }
    if (http != NULL) {
        const esp_err_t destroy_err = solar_os_http_request_destroy(http);
        if (err == ESP_OK && destroy_err != ESP_OK) {
            err = destroy_err;
        }
    }
    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    if (err == ESP_OK && (response.status_code != 200 ||
                          download.bytes != package->artifact_size)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = solar_os_crypto_sha256_finish(&download.sha256, digest);
    }
    if (err == ESP_OK &&
        !solar_os_crypto_sha256_matches_hex(digest, package->artifact_sha256)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    solar_os_crypto_sha256_free(&download.sha256);
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(staged_path);
        if (err == ESP_ERR_INVALID_CRC) {
            module_set_detail(detail,
                              detail_len,
                              "downloaded module hash does not match the signed catalog");
        } else if (download.response_status != 0 &&
                   download.response_status != 200) {
            if (detail != NULL && detail_len > 0U) {
                snprintf(detail,
                         detail_len,
                         "module artifact server returned HTTP %d",
                         download.response_status);
            }
        } else if (download.response_size_mismatch) {
            module_set_detail(detail,
                              detail_len,
                              "module artifact size differs from the signed catalog");
        } else {
            module_set_detail(detail,
                              detail_len,
                              "module download failed or was cancelled");
        }
        goto done;
    }

    bool resident_activated = false;
    err = module_validate_download(staged_path,
                                   package->artifact_size,
                                   package,
                                   detail,
                                   detail_len);
    if (err == ESP_OK && package->type != SOLAR_OS_MODULE_TYPE_APP) {
        err = solar_os_native_module_activate(package->type,
                                              package->id,
                                              staged_path,
                                              detail,
                                              detail_len);
        resident_activated = err == ESP_OK;
    }
    /* Validate a library's registration during installation, then leave it
     * cold. Libraries are activated by consumers, never by the boot scan. */
    if (err == ESP_OK && package->type == SOLAR_OS_MODULE_TYPE_LIBRARY) {
        err = solar_os_native_module_deactivate(package->type, package->id,
                                                detail, detail_len);
        resident_activated = err != ESP_OK;
    }
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(staged_path,
                                            active_path,
                                            backup_path);
    }
    if (err != ESP_OK) {
        if (resident_activated) {
            char deactivate_detail[128];
            (void)solar_os_native_module_deactivate(package->type,
                                                    package->id,
                                                    deactivate_detail,
                                                    sizeof(deactivate_detail));
        }
        (void)solar_os_storage_remove(staged_path);
        if (detail == NULL || detail[0] == '\0') {
            module_set_detail(detail, detail_len, "module validation or activation failed");
        }
        goto done;
    }
    if (package->type == SOLAR_OS_MODULE_TYPE_APP) {
        char legacy_path[SOLAR_OS_STORAGE_PATH_MAX];
        if (module_legacy_app_path(id,
                                   legacy_path,
                                   sizeof(legacy_path)) == ESP_OK) {
            (void)solar_os_storage_remove(legacy_path);
        }
    }

    if (result != NULL) {
        result->type = package->type;
        snprintf(result->id, sizeof(result->id), "%s", package->id);
        snprintf(result->version, sizeof(result->version), "%s", package->version);
        snprintf(result->path, sizeof(result->path), "%s", active_path);
        result->bytes = package->artifact_size;
    }

done:
    solar_os_module_catalog_free(catalog);
    atomic_store(&module_operation_running, false);
    return err;
}

esp_err_t solar_os_module_package_remove(const char *id,
                                         char *detail,
                                         size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (atomic_exchange(&module_operation_running, true)) {
        module_set_detail(detail, detail_len, "another module operation is running");
        return ESP_ERR_INVALID_STATE;
    }

    if (!module_name_valid(id)) {
        module_set_detail(detail, detail_len, "invalid module name or storage path");
        atomic_store(&module_operation_running, false);
        return ESP_ERR_INVALID_ARG;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    size_t matches = 0U;
    solar_os_module_type_t matched_type = SOLAR_OS_MODULE_TYPE_INVALID;
    static const solar_os_module_type_t types[] = {
        SOLAR_OS_MODULE_TYPE_APP,
        SOLAR_OS_MODULE_TYPE_JOB,
        SOLAR_OS_MODULE_TYPE_DRIVER,
        SOLAR_OS_MODULE_TYPE_LIBRARY,
    };
    for (size_t i = 0U; i < sizeof(types) / sizeof(types[0]); i++) {
        char candidate[SOLAR_OS_STORAGE_PATH_MAX];
        bool exists = false;
        if (solar_os_module_package_path(types[i],
                                         id,
                                         candidate,
                                         sizeof(candidate)) == ESP_OK &&
            solar_os_storage_exists(candidate, &exists) == ESP_OK && exists) {
            snprintf(path, sizeof(path), "%s", candidate);
            matched_type = types[i];
            matches++;
        }
    }
    char legacy_path[SOLAR_OS_STORAGE_PATH_MAX];
    bool legacy_exists = false;
    if (module_legacy_app_path(id, legacy_path, sizeof(legacy_path)) == ESP_OK) {
        (void)solar_os_storage_exists(legacy_path, &legacy_exists);
    }
    if (legacy_exists && matches == 0U) {
        snprintf(path, sizeof(path), "%s", legacy_path);
        matched_type = SOLAR_OS_MODULE_TYPE_APP;
        matches = 1U;
    } else if (legacy_exists && matched_type != SOLAR_OS_MODULE_TYPE_APP) {
        matches++;
    }
    if (matches > 1U) {
        module_set_detail(detail, detail_len, "module name is ambiguous across types");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (matches == 0U) {
        module_set_detail(detail, detail_len, "module is not installed");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (matched_type == SOLAR_OS_MODULE_TYPE_APP && solar_os_native_foreground_active(id)) {
        module_set_detail(detail, detail_len, "close the app session before removing its module");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (matched_type != SOLAR_OS_MODULE_TYPE_APP &&
        solar_os_native_module_active(matched_type, id)) {
        err = solar_os_native_module_deactivate(matched_type,
                                                id,
                                                detail,
                                                detail_len);
        if (err != ESP_OK) {
            goto done;
        }
    }
    err = solar_os_storage_remove(path);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not remove the installed module");
    } else if (matched_type == SOLAR_OS_MODULE_TYPE_APP && legacy_exists &&
               strcmp(path, legacy_path) != 0) {
        (void)solar_os_storage_remove(legacy_path);
    }

done:
    atomic_store(&module_operation_running, false);
    return err;
}
