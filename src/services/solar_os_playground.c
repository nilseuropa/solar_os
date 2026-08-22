#include "solar_os_playground.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_crypto.h"
#include "solar_os_http_client.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_zip.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define PLAYGROUND_NVS_NAMESPACE "playground"
#define PLAYGROUND_NVS_SOURCE_KEY "source"
#define PLAYGROUND_NVS_STORAGE_KEY "storage"
#define PLAYGROUND_SCHEMA "solaros.playground.catalog"
#define PLAYGROUND_SCHEMA_VERSION 1U
#define PLAYGROUND_CATALOG_MAX (256U * 1024U)
#define PLAYGROUND_PACKAGE_MAX (2U * 1024U * 1024U)
#define PLAYGROUND_EXTRACTED_MAX (8U * 1024U * 1024U)
#define PLAYGROUND_HTTP_TIMEOUT_MS 15000U
#define PLAYGROUND_HTTP_DEADLINE_MS 60000U
#define PLAYGROUND_MANIFEST_FILE "manifest.json"
#define PLAYGROUND_CATALOG_FILE "catalog.json"
#define PLAYGROUND_CATALOG_SOURCE_FILE "catalog.source"
#define PLAYGROUND_CATALOG_TEMP_FILE "catalog.json.new"
#define PLAYGROUND_CATALOG_SOURCE_TEMP_FILE "catalog.source.new"
#define PLAYGROUND_CATALOG_BACKUP_FILE "catalog.json.old"
#define PLAYGROUND_CATALOG_SOURCE_BACKUP_FILE "catalog.source.old"
#define PLAYGROUND_ALIAS_DIR ".shell"
#define PLAYGROUND_ALIAS_FILE "playground"
#define PLAYGROUND_ALIAS_TEMP_FILE "playground.new"
#define PLAYGROUND_ALIAS_BACKUP_FILE "playground.old"
#define PLAYGROUND_ALIAS_HEADER "# managed by Playground; do not edit\n"
#define PLAYGROUND_ALIAS_CONTENT_MAX \
    (sizeof(PLAYGROUND_ALIAS_HEADER) + \
     SOLAR_OS_PLAYGROUND_APP_MAX * (SOLAR_OS_PLAYGROUND_ID_MAX * 2U + 18U))

typedef struct {
    size_t category_count;
    size_t app_count;
    solar_os_playground_category_t categories[SOLAR_OS_PLAYGROUND_CATEGORY_MAX];
    solar_os_playground_app_info_t apps[SOLAR_OS_PLAYGROUND_APP_MAX];
} playground_catalog_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    volatile bool *cancel;
    solar_os_playground_progress_fn progress_fn;
    void *progress_user;
    solar_os_playground_progress_t progress;
} playground_http_buffer_t;

typedef struct {
    uint64_t uncompressed_size;
    const char *entry;
    size_t manifest_count;
    size_t entry_count;
} playground_zip_summary_t;

typedef struct {
    char catalog[SOLAR_OS_STORAGE_PATH_MAX];
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char catalog_temp[SOLAR_OS_STORAGE_PATH_MAX];
    char source_temp[SOLAR_OS_STORAGE_PATH_MAX];
    char catalog_backup[SOLAR_OS_STORAGE_PATH_MAX];
    char source_backup[SOLAR_OS_STORAGE_PATH_MAX];
} playground_cache_paths_t;

typedef struct {
    char id[SOLAR_OS_PLAYGROUND_ID_MAX];
} playground_alias_id_t;

static EXT_RAM_BSS_ATTR playground_catalog_t playground_catalog_banks[2];
static portMUX_TYPE playground_lock = portMUX_INITIALIZER_UNLOCKED;
static playground_catalog_t *playground_catalog =
    &playground_catalog_banks[0];
static char playground_source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX] =
    SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE;
static solar_os_playground_target_t playground_storage =
    SOLAR_OS_PLAYGROUND_TARGET_FLASH;
static bool playground_initialized;
static bool playground_catalog_ready;
static bool playground_refresh_active;
static uint32_t playground_source_generation;
static solar_os_http_request_t *playground_active_request;
static uint32_t playground_request_users;

static esp_err_t playground_sync_aliases(void);

static void playground_report(solar_os_playground_progress_fn callback,
                              void *user,
                              solar_os_playground_progress_t *progress)
{
    if (callback != NULL && progress != NULL) {
        callback(progress, user);
    }
}

static bool playground_url_valid(const char *url)
{
    if (url == NULL || url[0] == '\0' ||
        strlen(url) >= SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX ||
        (strncmp(url, "https://", 8U) != 0 &&
         strncmp(url, "http://", 7U) != 0)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p != '\0'; p++) {
        if (!isprint(*p) || isspace(*p)) {
            return false;
        }
    }
    return true;
}

static esp_err_t playground_normalize_source(const char *url,
                                             char *normalized,
                                             size_t normalized_len)
{
    static const char github_prefix[] = "https://github.com/";
    if (normalized == NULL || normalized_len == 0U ||
        !playground_url_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(url, github_prefix, strlen(github_prefix)) == 0) {
        const char *repository = url + strlen(github_prefix);
        const char *separator = strchr(repository, '/');
        if (separator != NULL && separator != repository &&
            memchr(repository, '?', (size_t)(separator - repository)) == NULL &&
            memchr(repository, '#', (size_t)(separator - repository)) == NULL) {
            const char *name = separator + 1U;
            size_t name_len = strlen(name);
            if (name_len > 0U && name[name_len - 1U] == '/') {
                name_len--;
            }
            if (name_len > 4U &&
                strncmp(name + name_len - 4U, ".git", 4U) == 0) {
                name_len -= 4U;
            }
            if (name_len > 0U &&
                memchr(name, '/', name_len) == NULL &&
                memchr(name, '?', name_len) == NULL &&
                memchr(name, '#', name_len) == NULL) {
                const int written = snprintf(
                    normalized,
                    normalized_len,
                    "https://raw.githubusercontent.com/%.*s/%.*s/"
                    "main/dist/catalog.json",
                    (int)(separator - repository),
                    repository,
                    (int)name_len,
                    name);
                return written >= 0 && (size_t)written < normalized_len ?
                    ESP_OK : ESP_ERR_INVALID_SIZE;
            }
        }
    }

    return strlcpy(normalized, url, normalized_len) < normalized_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t playground_catalog_fetch_url(const char *source,
                                              char *fetch_url,
                                              size_t fetch_url_len,
                                              bool *uses_github_api)
{
    static const char raw_prefix[] =
        "https://raw.githubusercontent.com/";
    static const char catalog_suffix[] = "/dist/catalog.json";
    if (source == NULL || fetch_url == NULL || fetch_url_len == 0U ||
        uses_github_api == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *uses_github_api = false;
    if (strncmp(source, raw_prefix, strlen(raw_prefix)) == 0) {
        const char *owner = source + strlen(raw_prefix);
        const char *owner_end = strchr(owner, '/');
        const char *repository = owner_end != NULL ? owner_end + 1U : NULL;
        const char *repository_end =
            repository != NULL ? strchr(repository, '/') : NULL;
        const char *ref =
            repository_end != NULL ? repository_end + 1U : NULL;
        const size_t ref_and_suffix_len = ref != NULL ? strlen(ref) : 0U;
        const size_t suffix_len = strlen(catalog_suffix);
        if (owner_end != NULL && owner_end != owner && repository_end != NULL &&
            repository_end != repository && ref_and_suffix_len > suffix_len &&
            strcmp(ref + ref_and_suffix_len - suffix_len,
                   catalog_suffix) == 0) {
            const size_t ref_len = ref_and_suffix_len - suffix_len;
            if (memchr(ref, '/', ref_len) == NULL &&
                memchr(ref, '?', ref_len) == NULL &&
                memchr(ref, '#', ref_len) == NULL) {
                const int written = snprintf(
                    fetch_url,
                    fetch_url_len,
                    "https://api.github.com/repos/%.*s/%.*s/contents/"
                    "dist/catalog.json?ref=%.*s",
                    (int)(owner_end - owner),
                    owner,
                    (int)(repository_end - repository),
                    repository,
                    (int)ref_len,
                    ref);
                if (written < 0 || (size_t)written >= fetch_url_len) {
                    return ESP_ERR_INVALID_SIZE;
                }
                *uses_github_api = true;
                return ESP_OK;
            }
        }
    }

    return strlcpy(fetch_url, source, fetch_url_len) < fetch_url_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool playground_id_valid(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    bool previous_dash = true;
    for (const unsigned char *p = (const unsigned char *)id; *p != '\0'; p++) {
        if (*p == '-') {
            if (previous_dash) {
                return false;
            }
            previous_dash = true;
        } else if (islower(*p) || isdigit(*p)) {
            previous_dash = false;
        } else {
            return false;
        }
    }
    return !previous_dash;
}

static bool playground_relative_path_valid(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/' ||
        strstr(path, "\\") != NULL) {
        return false;
    }
    const char *segment = path;
    while (segment != NULL && *segment != '\0') {
        const char *end = strchr(segment, '/');
        const size_t len = end != NULL ?
            (size_t)(end - segment) : strlen(segment);
        if (len == 0U ||
            (len == 1U && segment[0] == '.') ||
            (len == 2U && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        segment = end != NULL ? end + 1U : NULL;
    }
    return true;
}

static bool playground_runtime_available(solar_os_playground_runtime_t runtime)
{
    switch (runtime) {
    case SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON:
#if SOLAR_OS_PACKAGE_APP_PYTHON
        return true;
#else
        return false;
#endif
    case SOLAR_OS_PLAYGROUND_RUNTIME_LUA:
#if SOLAR_OS_PACKAGE_APP_LUA
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

static bool playground_parse_semver(const char *text,
                                    unsigned *major,
                                    unsigned *minor,
                                    unsigned *patch)
{
    char trailing = '\0';
    return text != NULL &&
        sscanf(text, "%u.%u.%u%c", major, minor, patch, &trailing) == 3;
}

static bool playground_version_at_least(const char *current, const char *minimum)
{
    unsigned current_major = 0U;
    unsigned current_minor = 0U;
    unsigned current_patch = 0U;
    unsigned minimum_major = 0U;
    unsigned minimum_minor = 0U;
    unsigned minimum_patch = 0U;
    if (!playground_parse_semver(current,
                                 &current_major,
                                 &current_minor,
                                 &current_patch) ||
        !playground_parse_semver(minimum,
                                 &minimum_major,
                                 &minimum_minor,
                                 &minimum_patch)) {
        return false;
    }
    if (current_major != minimum_major) {
        return current_major > minimum_major;
    }
    if (current_minor != minimum_minor) {
        return current_minor > minimum_minor;
    }
    return current_patch >= minimum_patch;
}

static bool playground_capability_available(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (unsigned bit = 0U; bit < 32U; bit++) {
        const solar_os_board_capability_t capability = UINT64_C(1) << bit;
        if (strcmp(solar_os_board_capability_name(capability), name) == 0) {
            return solar_os_board_has(capability);
        }
    }
    return false;
}

static bool playground_requirements_available(
    const solar_os_json_value_t *requirements,
    char *reason,
    size_t reason_len)
{
    if (!solar_os_json_is_array(requirements)) {
        return false;
    }
    const size_t count = solar_os_json_array_size(requirements);
    for (size_t i = 0U; i < count; i++) {
        char requirement[32] = {0};
        if (solar_os_json_get_string(
                solar_os_json_array_get(requirements, i),
                requirement,
                sizeof(requirement)) != ESP_OK ||
            !playground_capability_available(requirement)) {
            snprintf(reason,
                     reason_len,
                     "requires %s",
                     requirement[0] != '\0' ? requirement : "unknown");
            return false;
        }
    }
    return true;
}

static esp_err_t playground_http_event(const solar_os_http_event_t *event,
                                       void *user_data)
{
    playground_http_buffer_t *buffer =
        (playground_http_buffer_t *)user_data;
    if (event == NULL || buffer == NULL) {
        return ESP_OK;
    }
    if (buffer->cancel != NULL && *buffer->cancel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
        if (event->header_name != NULL && event->header_value != NULL &&
            strcasecmp(event->header_name, "Content-Length") == 0) {
            char *end = NULL;
            const unsigned long long parsed =
                strtoull(event->header_value, &end, 10);
            if (end != event->header_value && *end == '\0' &&
                parsed <= buffer->capacity - 1U) {
                buffer->progress.bytes_total = (uint32_t)parsed;
                buffer->progress.total_known = true;
                playground_report(buffer->progress_fn,
                                  buffer->progress_user,
                                  &buffer->progress);
            }
        }
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA) {
        return ESP_OK;
    }
    if (event->data_len > buffer->capacity - buffer->length - 1U) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    buffer->progress.bytes_read = (uint32_t)buffer->length;
    playground_report(buffer->progress_fn,
                      buffer->progress_user,
                      &buffer->progress);
    return ESP_OK;
}

static esp_err_t playground_download(
    const char *url,
    size_t max_len,
    const solar_os_http_header_t *headers,
    size_t header_count,
    volatile bool *cancel,
    solar_os_playground_progress_stage_t stage,
    solar_os_playground_progress_fn progress_fn,
    void *progress_user,
    char **body,
    size_t *body_len)
{
    if (url == NULL || body == NULL || body_len == NULL || max_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *body = NULL;
    *body_len = 0U;
    char *data = solar_os_memory_alloc(max_len + 1U,
                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "playground.http");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    data[0] = '\0';
    playground_http_buffer_t buffer = {
        .data = data,
        .capacity = max_len + 1U,
        .cancel = cancel,
        .progress_fn = progress_fn,
        .progress_user = progress_user,
        .progress = {
            .stage = stage,
        },
    };
    playground_report(progress_fn, progress_user, &buffer.progress);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .headers = headers,
        .header_count = header_count,
        .user_agent = "SolarOS-playground/" SOLAR_OS_VERSION,
        .follow_redirects = true,
        .timeout_ms = PLAYGROUND_HTTP_TIMEOUT_MS,
        .deadline_ms = PLAYGROUND_HTTP_DEADLINE_MS,
        .receive_buffer_size = 2048U,
        .transmit_buffer_size = 1024U,
        .event_handler = playground_http_event,
        .user_data = &buffer,
    };
    solar_os_http_request_t *request = NULL;
    esp_err_t err = solar_os_http_request_create(&options, &request);
    solar_os_http_response_t response = {0};
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        playground_active_request = request;
        portEXIT_CRITICAL(&playground_lock);
        err = solar_os_http_request_perform(request, &response);
        portENTER_CRITICAL(&playground_lock);
        if (playground_active_request == request) {
            playground_active_request = NULL;
        }
        portEXIT_CRITICAL(&playground_lock);
        for (;;) {
            portENTER_CRITICAL(&playground_lock);
            const bool in_use = playground_request_users > 0U;
            portEXIT_CRITICAL(&playground_lock);
            if (!in_use) {
                break;
            }
            vTaskDelay(1);
        }
    }
    if (request != NULL) {
        const esp_err_t destroy_err =
            solar_os_http_request_destroy(request);
        if (err == ESP_OK && destroy_err != ESP_OK) {
            err = destroy_err;
        }
    }
    if (err == ESP_OK && response.status_code != 200) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (cancel != NULL && *cancel) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        solar_os_memory_free(data);
        return err;
    }
    *body = data;
    *body_len = buffer.length;
    return ESP_OK;
}

static esp_err_t playground_parse_runtime(
    const solar_os_json_value_t *value,
    solar_os_playground_runtime_t *runtime)
{
    char name[16];
    esp_err_t err = solar_os_json_get_string(value, name, sizeof(name));
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(name, "python") == 0) {
        *runtime = SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON;
        return ESP_OK;
    }
    if (strcmp(name, "lua") == 0) {
        *runtime = SOLAR_OS_PLAYGROUND_RUNTIME_LUA;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t playground_parse_category(
    const solar_os_json_value_t *value,
    solar_os_playground_category_t *category)
{
    esp_err_t err = solar_os_json_get_path_string(
        value, "id", category->id, sizeof(category->id));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "title", category->title, sizeof(category->title));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value, "order", &category->order);
    }
    if (err != ESP_OK || !playground_id_valid(category->id) ||
        category->title[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static bool playground_category_exists(const playground_catalog_t *catalog,
                                       const char *id)
{
    for (size_t i = 0U; i < catalog->category_count; i++) {
        if (strcmp(catalog->categories[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t playground_parse_app(
    const solar_os_json_value_t *value,
    const playground_catalog_t *catalog,
    solar_os_playground_app_info_t *app)
{
    memset(app, 0, sizeof(*app));
    esp_err_t err = solar_os_json_get_path_string(
        value, "id", app->id, sizeof(app->id));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "name", app->name, sizeof(app->name));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "version", app->version, sizeof(app->version));
    }
    if (err == ESP_OK) {
        err = playground_parse_runtime(
            solar_os_json_object_get(value, "runtime"), &app->runtime);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "entry", app->entry, sizeof(app->entry));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "category", app->category, sizeof(app->category));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "description", app->description, sizeof(app->description));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "author", app->author, sizeof(app->author));
    }
    const solar_os_json_value_t *tags =
        err == ESP_OK ? solar_os_json_object_get(value, "tags") : NULL;
    if (err == ESP_OK && !solar_os_json_is_array(tags)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0U;
         err == ESP_OK && i < solar_os_json_array_size(tags);
         i++) {
        char tag[24];
        err = solar_os_json_get_string(
            solar_os_json_array_get(tags, i), tag, sizeof(tag));
        if (err == ESP_OK) {
            const size_t used = strlen(app->tags);
            const int written = snprintf(app->tags + used,
                                         sizeof(app->tags) - used,
                                         "%s%s",
                                         used > 0U ? " " : "",
                                         tag);
            if (written < 0 ||
                (size_t)written >= sizeof(app->tags) - used) {
                err = ESP_ERR_INVALID_SIZE;
            }
        }
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "min_solaros", app->min_solaros, sizeof(app->min_solaros));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "archive", app->archive, sizeof(app->archive));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            value, "sha256", app->sha256, sizeof(app->sha256));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value, "size", &app->size);
    }
    if (err != ESP_OK || !playground_id_valid(app->id) ||
        app->name[0] == '\0' ||
        !playground_relative_path_valid(app->entry) ||
        (app->runtime == SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON &&
         (strlen(app->entry) < 3U ||
          strcmp(app->entry + strlen(app->entry) - 3U, ".py") != 0)) ||
        (app->runtime == SOLAR_OS_PLAYGROUND_RUNTIME_LUA &&
         (strlen(app->entry) < 4U ||
          strcmp(app->entry + strlen(app->entry) - 4U, ".lua") != 0)) ||
        !playground_category_exists(catalog, app->category) ||
        !playground_relative_path_valid(app->archive) ||
        !solar_os_crypto_sha256_hex_is_valid(app->sha256) ||
        app->size == 0U || app->size > PLAYGROUND_PACKAGE_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    app->compatible = true;
    if (!playground_runtime_available(app->runtime)) {
        app->compatible = false;
        snprintf(app->incompatibility,
                 sizeof(app->incompatibility),
                 "%s unavailable",
                 solar_os_playground_runtime_name(app->runtime));
    } else if (!playground_version_at_least(SOLAR_OS_VERSION,
                                            app->min_solaros)) {
        app->compatible = false;
        snprintf(app->incompatibility,
                 sizeof(app->incompatibility),
                 "needs SolarOS %s",
                 app->min_solaros);
    } else {
        const solar_os_json_value_t *requirements =
            solar_os_json_object_get(value, "requires");
        app->compatible = playground_requirements_available(
            requirements,
            app->incompatibility,
            sizeof(app->incompatibility));
    }
    return ESP_OK;
}

static int playground_category_compare(const void *left, const void *right)
{
    const solar_os_playground_category_t *a = left;
    const solar_os_playground_category_t *b = right;
    if (a->order < b->order) {
        return -1;
    }
    if (a->order > b->order) {
        return 1;
    }
    return strcasecmp(a->title, b->title);
}

static int playground_app_compare(const void *left, const void *right)
{
    const solar_os_playground_app_info_t *a = left;
    const solar_os_playground_app_info_t *b = right;
    const int category = strcmp(a->category, b->category);
    return category != 0 ? category : strcasecmp(a->name, b->name);
}

static esp_err_t playground_parse_catalog(const char *body,
                                          size_t body_len,
                                          playground_catalog_t *catalog)
{
    solar_os_json_doc_t *document = NULL;
    memset(catalog, 0, sizeof(*catalog));
    esp_err_t err = solar_os_json_parse(body, body_len, &document);
    const solar_os_json_value_t *root =
        err == ESP_OK ? solar_os_json_root(document) : NULL;
    char schema[40];
    uint32_t schema_version = 0U;
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            root, "schema", schema, sizeof(schema));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(
            root, "schema_version", &schema_version);
    }
    const solar_os_json_value_t *categories =
        err == ESP_OK ? solar_os_json_object_get(root, "categories") : NULL;
    const solar_os_json_value_t *apps =
        err == ESP_OK ? solar_os_json_object_get(root, "apps") : NULL;
    if (err != ESP_OK || strcmp(schema, PLAYGROUND_SCHEMA) != 0 ||
        schema_version != PLAYGROUND_SCHEMA_VERSION ||
        !solar_os_json_is_array(categories) ||
        !solar_os_json_is_array(apps)) {
        solar_os_json_free(document);
        return ESP_ERR_INVALID_RESPONSE;
    }

    catalog->category_count = solar_os_json_array_size(categories);
    catalog->app_count = solar_os_json_array_size(apps);
    if (catalog->category_count == 0U ||
        catalog->category_count > SOLAR_OS_PLAYGROUND_CATEGORY_MAX ||
        catalog->app_count > SOLAR_OS_PLAYGROUND_APP_MAX) {
        solar_os_json_free(document);
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0U; i < catalog->category_count && err == ESP_OK; i++) {
        err = playground_parse_category(
            solar_os_json_array_get(categories, i),
            &catalog->categories[i]);
        for (size_t previous = 0U; previous < i && err == ESP_OK; previous++) {
            if (strcmp(catalog->categories[previous].id,
                       catalog->categories[i].id) == 0) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    for (size_t i = 0U; i < catalog->app_count && err == ESP_OK; i++) {
        err = playground_parse_app(
            solar_os_json_array_get(apps, i), catalog, &catalog->apps[i]);
        for (size_t previous = 0U; previous < i && err == ESP_OK; previous++) {
            if (strcmp(catalog->apps[previous].id,
                       catalog->apps[i].id) == 0) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    solar_os_json_free(document);
    if (err == ESP_OK) {
        qsort(catalog->categories,
              catalog->category_count,
              sizeof(catalog->categories[0]),
              playground_category_compare);
        qsort(catalog->apps,
              catalog->app_count,
              sizeof(catalog->apps[0]),
              playground_app_compare);
    }
    return err;
}

static esp_err_t playground_join_url(const char *catalog_url,
                                     const char *relative,
                                     char *url,
                                     size_t url_len)
{
    if (!playground_relative_path_valid(relative)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *slash = strrchr(catalog_url, '/');
    if (slash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t base_len = (size_t)(slash - catalog_url + 1U);
    const int written = snprintf(url,
                                 url_len,
                                 "%.*s%s",
                                 (int)base_len,
                                 catalog_url,
                                 relative);
    return written >= 0 && (size_t)written < url_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t playground_mkdir_one(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t playground_write_file(const char *path,
                                       const void *data,
                                       size_t len)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const size_t written = len > 0U ? fwrite(data, 1U, len, file) : 0U;
    const bool failed =
        written != len || fflush(file) != 0 || fsync(fileno(file)) != 0;
    fclose(file);
    return failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t playground_read_file(const char *path,
                                      size_t max_len,
                                      char **data,
                                      size_t *data_len)
{
    if (data == NULL || data_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = NULL;
    *data_len = 0U;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || (uint64_t)st.st_size > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t len = (size_t)st.st_size;
    char *buffer = solar_os_memory_alloc(len + 1U,
                                         SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "playground.catalog.file");
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        solar_os_memory_free(buffer);
        return ESP_FAIL;
    }
    const size_t read_len = fread(buffer, 1U, len, file);
    const bool failed = read_len != len || ferror(file);
    fclose(file);
    if (failed) {
        solar_os_memory_free(buffer);
        return ESP_FAIL;
    }
    buffer[len] = '\0';
    *data = buffer;
    *data_len = len;
    return ESP_OK;
}

static esp_err_t playground_remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return remove(path) == 0 ? ESP_OK : ESP_FAIL;
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    struct dirent *entry = NULL;
    while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        if (solar_os_storage_join_path(
                path, entry->d_name, child, sizeof(child)) != ESP_OK) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = playground_remove_tree(child);
    }
    closedir(directory);
    if (err == ESP_OK && rmdir(path) != 0 && errno != ENOENT) {
        err = ESP_FAIL;
    }
    return err;
}

static esp_err_t playground_mount_for_target(
    solar_os_playground_target_t target,
    char *mount,
    size_t mount_len)
{
    if (target == SOLAR_OS_PLAYGROUND_TARGET_AUTO) {
        (void)solar_os_playground_init();
        portENTER_CRITICAL(&playground_lock);
        target = playground_storage;
        portEXIT_CRITICAL(&playground_lock);
    }
    if (target == SOLAR_OS_PLAYGROUND_TARGET_SD) {
        if (!solar_os_storage_sd_is_mounted()) {
            return ESP_ERR_INVALID_STATE;
        }
        return strlcpy(mount,
                       solar_os_storage_sd_mount_point(),
                       mount_len) < mount_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (target != SOLAR_OS_PLAYGROUND_TARGET_FLASH) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t count = solar_os_storage_mount_count();
    for (size_t i = 0U; i < count; i++) {
        solar_os_storage_mount_info_t info;
        if (solar_os_storage_get_mount(i, &info) &&
            info.type == SOLAR_OS_STORAGE_MOUNT_FLASH) {
            return strlcpy(mount, info.mount_point, mount_len) < mount_len ?
                ESP_OK : ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t playground_data_root(const char *mount,
                                      char *root,
                                      size_t root_len,
                                      bool create)
{
    esp_err_t err =
        solar_os_storage_join_path(mount, "playground", root, root_len);
    if (err == ESP_OK && create) {
        err = playground_mkdir_one(root);
    }
    return err;
}

static esp_err_t playground_legacy_data_root(const char *mount,
                                             char *root,
                                             size_t root_len)
{
    char solar[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_join_path(mount,
                                               ".solar",
                                               solar,
                                               sizeof(solar));
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            solar, "playground", root, root_len);
    }
    return err;
}

static esp_err_t playground_cache_paths(
    bool create,
    solar_os_playground_target_t target,
    playground_cache_paths_t *paths)
{
    if (paths == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = playground_mount_for_target(target, mount, sizeof(mount));
    if (err == ESP_OK) {
        err = playground_data_root(mount, root, sizeof(root), create);
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            root,
            PLAYGROUND_CATALOG_FILE,
            paths->catalog,
            sizeof(paths->catalog));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            root,
            PLAYGROUND_CATALOG_SOURCE_FILE,
            paths->source,
            sizeof(paths->source));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(root,
                                         PLAYGROUND_CATALOG_TEMP_FILE,
                                         paths->catalog_temp,
                                         sizeof(paths->catalog_temp));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            root,
            PLAYGROUND_CATALOG_SOURCE_TEMP_FILE,
            paths->source_temp,
            sizeof(paths->source_temp));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(root,
                                         PLAYGROUND_CATALOG_BACKUP_FILE,
                                         paths->catalog_backup,
                                         sizeof(paths->catalog_backup));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            root,
            PLAYGROUND_CATALOG_SOURCE_BACKUP_FILE,
            paths->source_backup,
            sizeof(paths->source_backup));
    }
    return err;
}

static esp_err_t playground_backup_file(const char *path,
                                        const char *backup,
                                        bool *backed_up)
{
    if (backed_up == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *backed_up = false;
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (remove(backup) != 0 && errno != ENOENT) {
        return ESP_FAIL;
    }
    if (rename(path, backup) != 0) {
        return ESP_FAIL;
    }
    *backed_up = true;
    return ESP_OK;
}

static void playground_restore_file(const char *path,
                                    const char *backup,
                                    bool backed_up,
                                    bool committed)
{
    if (committed) {
        (void)remove(path);
    }
    if (backed_up) {
        (void)remove(path);
        (void)rename(backup, path);
    }
}

static esp_err_t playground_save_catalog(const char *source_value,
                                         const char *body,
                                         size_t body_len,
                                         solar_os_playground_target_t target)
{
    playground_cache_paths_t paths = {0};
    esp_err_t err = playground_cache_paths(true, target, &paths);
    if (err == ESP_OK) {
        err = playground_write_file(paths.catalog_temp, body, body_len);
    }
    if (err == ESP_OK) {
        err = playground_write_file(
            paths.source_temp, source_value, strlen(source_value));
    }
    bool catalog_backed_up = false;
    bool source_backed_up = false;
    bool catalog_committed = false;
    bool source_committed = false;
    if (err == ESP_OK) {
        err = playground_backup_file(paths.catalog,
                                     paths.catalog_backup,
                                     &catalog_backed_up);
    }
    if (err == ESP_OK) {
        err = playground_backup_file(paths.source,
                                     paths.source_backup,
                                     &source_backed_up);
    }
    if (err == ESP_OK && rename(paths.catalog_temp, paths.catalog) != 0) {
        err = ESP_FAIL;
    } else if (err == ESP_OK) {
        catalog_committed = true;
    }
    if (err == ESP_OK && rename(paths.source_temp, paths.source) != 0) {
        err = ESP_FAIL;
    } else if (err == ESP_OK) {
        source_committed = true;
    }
    if (err == ESP_OK) {
        (void)remove(paths.catalog_backup);
        (void)remove(paths.source_backup);
    } else {
        playground_restore_file(
            paths.catalog,
            paths.catalog_backup,
            catalog_backed_up,
            catalog_committed);
        playground_restore_file(
            paths.source,
            paths.source_backup,
            source_backed_up,
            source_committed);
        if (paths.catalog_temp[0] != '\0') {
            (void)remove(paths.catalog_temp);
        }
        if (paths.source_temp[0] != '\0') {
            (void)remove(paths.source_temp);
        }
    }
    return err;
}

static esp_err_t playground_app_root_fields(
    const char *mount,
    solar_os_playground_runtime_t runtime_kind,
    const char *app_id,
    char *root,
    size_t root_len,
    bool create)
{
    char playground[SOLAR_OS_STORAGE_PATH_MAX];
    char runtime[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err =
        playground_data_root(mount, playground, sizeof(playground), create);
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            playground,
            solar_os_playground_runtime_name(runtime_kind),
            runtime,
            sizeof(runtime));
    }
    if (err == ESP_OK && create) {
        err = playground_mkdir_one(runtime);
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(runtime,
                                         app_id,
                                         root,
                                         root_len);
    }
    return err;
}

static esp_err_t playground_app_root(
    const char *mount,
    const solar_os_playground_app_info_t *app,
    char *root,
    size_t root_len,
    bool create)
{
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return playground_app_root_fields(
        mount, app->runtime, app->id, root, root_len, create);
}

static bool playground_find_installed_root_fields(
    solar_os_playground_runtime_t runtime,
    const char *id,
    const char *entry_name,
    char *root,
    size_t root_len)
{
    if (id == NULL || entry_name == NULL) {
        return false;
    }
    const size_t count = solar_os_storage_mount_count();
    for (int pass = 0; pass < 2; pass++) {
        const solar_os_storage_mount_type_t wanted =
            pass == 0 ? SOLAR_OS_STORAGE_MOUNT_SD :
                        SOLAR_OS_STORAGE_MOUNT_FLASH;
        for (size_t i = 0U; i < count; i++) {
            solar_os_storage_mount_info_t info;
            if (!solar_os_storage_get_mount(i, &info) ||
                info.type != wanted ||
                playground_app_root_fields(info.mount_point,
                                           runtime,
                                           id,
                                           root,
                                           root_len,
                                           false) != ESP_OK) {
                continue;
            }
            char entry[SOLAR_OS_STORAGE_PATH_MAX];
            struct stat st;
            if (solar_os_storage_join_path(root,
                                           entry_name,
                                           entry,
                                           sizeof(entry)) == ESP_OK &&
                stat(entry, &st) == 0 && S_ISREG(st.st_mode)) {
                return true;
            }
        }
    }
    return false;
}

static bool playground_find_installed_root(
    const solar_os_playground_app_info_t *app,
    char *root,
    size_t root_len)
{
    return app != NULL &&
        playground_find_installed_root_fields(
            app->runtime, app->id, app->entry, root, root_len);
}

static esp_err_t playground_read_installed_version(
    const char *root,
    char *version,
    size_t version_len)
{
    char manifest_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_join_path(root,
                                               PLAYGROUND_MANIFEST_FILE,
                                               manifest_path,
                                               sizeof(manifest_path));
    struct stat st;
    if (err != ESP_OK || stat(manifest_path, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 16 * 1024) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t len = (size_t)st.st_size;
    char *data = solar_os_memory_alloc(len + 1U,
                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "playground.manifest");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(manifest_path, "rb");
    if (file == NULL) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    const size_t read_len = fread(data, 1U, len, file);
    fclose(file);
    if (read_len != len) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    data[len] = '\0';
    solar_os_json_doc_t *document = NULL;
    err = solar_os_json_parse(data, len, &document);
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            solar_os_json_root(document), "version", version, version_len);
    }
    solar_os_json_free(document);
    solar_os_memory_free(data);
    return err;
}

static esp_err_t playground_read_installed_app(
    const char *root,
    const char *expected_id,
    solar_os_playground_runtime_t expected_runtime,
    solar_os_playground_app_info_t *app)
{
    if (root == NULL || expected_id == NULL || app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char manifest_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_join_path(root,
                                               PLAYGROUND_MANIFEST_FILE,
                                               manifest_path,
                                               sizeof(manifest_path));
    char *data = NULL;
    size_t data_len = 0U;
    if (err == ESP_OK) {
        err = playground_read_file(manifest_path, 16U * 1024U, &data, &data_len);
    }
    solar_os_json_doc_t *document = NULL;
    if (err == ESP_OK) {
        err = solar_os_json_parse(data, data_len, &document);
    }
    const solar_os_json_value_t *json =
        err == ESP_OK ? solar_os_json_root(document) : NULL;
    char runtime_name[16] = {0};
    memset(app, 0, sizeof(*app));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            json, "id", app->id, sizeof(app->id));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            json, "runtime", runtime_name, sizeof(runtime_name));
    }
    if (err == ESP_OK) {
        err = playground_parse_runtime(
            solar_os_json_object_get(json, "runtime"), &app->runtime);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            json, "entry", app->entry, sizeof(app->entry));
    }
    if (err == ESP_OK) {
        (void)solar_os_json_get_path_string(
            json, "name", app->name, sizeof(app->name));
        (void)solar_os_json_get_path_string(
            json, "version", app->version, sizeof(app->version));
    }
    const size_t entry_len = strlen(app->entry);
    if (err == ESP_OK &&
        (strcmp(app->id, expected_id) != 0 ||
         app->runtime != expected_runtime ||
         strcmp(runtime_name,
                solar_os_playground_runtime_name(expected_runtime)) != 0 ||
         !playground_id_valid(app->id) ||
         !playground_relative_path_valid(app->entry) ||
         (app->runtime == SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON &&
          (entry_len < 3U ||
           strcmp(app->entry + entry_len - 3U, ".py") != 0)) ||
         (app->runtime == SOLAR_OS_PLAYGROUND_RUNTIME_LUA &&
          (entry_len < 4U ||
           strcmp(app->entry + entry_len - 4U, ".lua") != 0)))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    char entry_path[SOLAR_OS_STORAGE_PATH_MAX];
    struct stat st;
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            root, app->entry, entry_path, sizeof(entry_path));
    }
    if (err == ESP_OK &&
        (stat(entry_path, &st) != 0 || !S_ISREG(st.st_mode))) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        app->compatible = true;
    }
    solar_os_json_free(document);
    solar_os_memory_free(data);
    return err;
}

bool solar_os_playground_find_installed_app(
    const char *id,
    solar_os_playground_app_info_t *app)
{
    if (!playground_id_valid(id) || app == NULL) {
        return false;
    }
    const size_t mount_count = solar_os_storage_mount_count();
    for (int pass = 0; pass < 2; pass++) {
        const solar_os_storage_mount_type_t wanted =
            pass == 0 ? SOLAR_OS_STORAGE_MOUNT_SD :
                        SOLAR_OS_STORAGE_MOUNT_FLASH;
        for (size_t mount_index = 0U;
             mount_index < mount_count;
             mount_index++) {
            solar_os_storage_mount_info_t mount;
            if (!solar_os_storage_get_mount(mount_index, &mount) ||
                mount.type != wanted) {
                continue;
            }
            for (int runtime_value = SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON;
                 runtime_value <= SOLAR_OS_PLAYGROUND_RUNTIME_LUA;
                 runtime_value++) {
                const solar_os_playground_runtime_t runtime =
                    (solar_os_playground_runtime_t)runtime_value;
                char root[SOLAR_OS_STORAGE_PATH_MAX];
                if (playground_app_root_fields(mount.mount_point,
                                               runtime,
                                               id,
                                               root,
                                               sizeof(root),
                                               false) == ESP_OK &&
                    playground_read_installed_app(
                        root, id, runtime, app) == ESP_OK) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool playground_alias_id_exists(const playground_alias_id_t *aliases,
                                       size_t count,
                                       const char *id)
{
    for (size_t i = 0U; i < count; i++) {
        if (strcmp(aliases[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static int playground_alias_id_compare(const void *left, const void *right)
{
    const playground_alias_id_t *a = left;
    const playground_alias_id_t *b = right;
    return strcmp(a->id, b->id);
}

static void playground_collect_aliases_from_mount(
    const solar_os_storage_mount_info_t *mount,
    playground_alias_id_t *aliases,
    size_t *count)
{
    if (mount == NULL || aliases == NULL || count == NULL) {
        return;
    }
    for (int runtime_value = SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON;
         runtime_value <= SOLAR_OS_PLAYGROUND_RUNTIME_LUA;
         runtime_value++) {
        const solar_os_playground_runtime_t runtime =
            (solar_os_playground_runtime_t)runtime_value;
        char data_root[SOLAR_OS_STORAGE_PATH_MAX];
        char runtime_root[SOLAR_OS_STORAGE_PATH_MAX];
        if (playground_data_root(mount->mount_point,
                                 data_root,
                                 sizeof(data_root),
                                 false) != ESP_OK ||
            solar_os_storage_join_path(
                data_root,
                solar_os_playground_runtime_name(runtime),
                runtime_root,
                sizeof(runtime_root)) != ESP_OK) {
            continue;
        }
        DIR *directory = opendir(runtime_root);
        if (directory == NULL) {
            continue;
        }
        struct dirent *entry = NULL;
        while (*count < SOLAR_OS_PLAYGROUND_APP_MAX &&
               (entry = readdir(directory)) != NULL) {
            if (!playground_id_valid(entry->d_name) ||
                playground_alias_id_exists(aliases, *count, entry->d_name)) {
                continue;
            }
            char root[SOLAR_OS_STORAGE_PATH_MAX];
            solar_os_playground_app_info_t app;
            if (playground_app_root_fields(mount->mount_point,
                                           runtime,
                                           entry->d_name,
                                           root,
                                           sizeof(root),
                                           false) != ESP_OK ||
                playground_read_installed_app(
                    root, entry->d_name, runtime, &app) != ESP_OK) {
                continue;
            }
            strlcpy(aliases[*count].id,
                    app.id,
                    sizeof(aliases[*count].id));
            (*count)++;
        }
        closedir(directory);
    }
}

static esp_err_t playground_write_alias_registry(
    const char *mount,
    const char *content,
    size_t content_len)
{
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char temp[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_join_path(
        mount, PLAYGROUND_ALIAS_DIR, directory, sizeof(directory));
    if (err == ESP_OK) {
        err = playground_mkdir_one(directory);
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            directory, PLAYGROUND_ALIAS_FILE, path, sizeof(path));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            directory, PLAYGROUND_ALIAS_TEMP_FILE, temp, sizeof(temp));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(
            directory, PLAYGROUND_ALIAS_BACKUP_FILE, backup, sizeof(backup));
    }
    if (err == ESP_OK) {
        err = playground_write_file(temp, content, content_len);
    }
    bool backed_up = false;
    bool committed = false;
    if (err == ESP_OK) {
        err = playground_backup_file(path, backup, &backed_up);
    }
    if (err == ESP_OK && rename(temp, path) != 0) {
        err = ESP_FAIL;
    } else if (err == ESP_OK) {
        committed = true;
    }
    if (err == ESP_OK) {
        (void)remove(backup);
    } else {
        playground_restore_file(path, backup, backed_up, committed);
        (void)remove(temp);
    }
    return err;
}

static esp_err_t playground_sync_aliases(void)
{
    playground_alias_id_t *aliases = solar_os_memory_calloc(
        SOLAR_OS_PLAYGROUND_APP_MAX,
        sizeof(*aliases),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "playground.alias.ids");
    char *content = solar_os_memory_alloc(
        PLAYGROUND_ALIAS_CONTENT_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "playground.alias.content");
    if (aliases == NULL || content == NULL) {
        solar_os_memory_free(aliases);
        solar_os_memory_free(content);
        return ESP_ERR_NO_MEM;
    }

    size_t alias_count = 0U;
    const size_t mount_count = solar_os_storage_mount_count();
    for (int pass = 0; pass < 2; pass++) {
        const solar_os_storage_mount_type_t wanted =
            pass == 0 ? SOLAR_OS_STORAGE_MOUNT_SD :
                        SOLAR_OS_STORAGE_MOUNT_FLASH;
        for (size_t i = 0U; i < mount_count; i++) {
            solar_os_storage_mount_info_t mount;
            if (solar_os_storage_get_mount(i, &mount) &&
                mount.type == wanted) {
                playground_collect_aliases_from_mount(
                    &mount, aliases, &alias_count);
            }
        }
    }
    qsort(aliases,
          alias_count,
          sizeof(*aliases),
          playground_alias_id_compare);

    size_t used = strlcpy(
        content, PLAYGROUND_ALIAS_HEADER, PLAYGROUND_ALIAS_CONTENT_MAX);
    esp_err_t err = used < PLAYGROUND_ALIAS_CONTENT_MAX ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
    for (size_t i = 0U; err == ESP_OK && i < alias_count; i++) {
        const int written = snprintf(content + used,
                                     PLAYGROUND_ALIAS_CONTENT_MAX - used,
                                     "%s playground run %s\n",
                                     aliases[i].id,
                                     aliases[i].id);
        if (written < 0 ||
            (size_t)written >= PLAYGROUND_ALIAS_CONTENT_MAX - used) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            used += (size_t)written;
        }
    }

    bool wrote_registry = false;
    for (size_t i = 0U; err == ESP_OK && i < mount_count; i++) {
        solar_os_storage_mount_info_t mount;
        if (solar_os_storage_get_mount(i, &mount) &&
            (mount.type == SOLAR_OS_STORAGE_MOUNT_SD ||
             mount.type == SOLAR_OS_STORAGE_MOUNT_FLASH)) {
            err = playground_write_alias_registry(
                mount.mount_point, content, used);
            wrote_registry = wrote_registry || err == ESP_OK;
        }
    }
    if (err == ESP_OK && !wrote_registry) {
        err = ESP_ERR_INVALID_STATE;
    }
    solar_os_memory_free(content);
    solar_os_memory_free(aliases);
    return err;
}

static void playground_zip_summarize(const solar_os_zip_event_info_t *info,
                                     void *user)
{
    playground_zip_summary_t *summary = user;
    if (info != NULL && summary != NULL &&
        info->event == SOLAR_OS_ZIP_EVENT_LIST &&
        info->archive_name != NULL) {
        summary->uncompressed_size += info->uncompressed_size;
        if (strcmp(info->archive_name, PLAYGROUND_MANIFEST_FILE) == 0) {
            summary->manifest_count++;
        }
        if (summary->entry != NULL &&
            strcmp(info->archive_name, summary->entry) == 0) {
            summary->entry_count++;
        }
    }
}

static esp_err_t playground_validate_package_manifest(
    const char *archive_path,
    const solar_os_playground_app_info_t *app)
{
    uint8_t *data = NULL;
    size_t data_len = 0U;
    esp_err_t err = solar_os_zip_read_file(archive_path,
                                           PLAYGROUND_MANIFEST_FILE,
                                           16U * 1024U,
                                           &data,
                                           &data_len);
    solar_os_json_doc_t *document = NULL;
    if (err == ESP_OK) {
        err = solar_os_json_parse((const char *)data, data_len, &document);
    }
    const solar_os_json_value_t *root =
        err == ESP_OK ? solar_os_json_root(document) : NULL;
    char id[SOLAR_OS_PLAYGROUND_ID_MAX] = {0};
    char version[SOLAR_OS_PLAYGROUND_VERSION_MAX] = {0};
    char runtime[16] = {0};
    char entry[SOLAR_OS_PLAYGROUND_ENTRY_MAX] = {0};
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "id", id, sizeof(id));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            root, "version", version, sizeof(version));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            root, "runtime", runtime, sizeof(runtime));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            root, "entry", entry, sizeof(entry));
    }
    if (err == ESP_OK &&
        (strcmp(id, app->id) != 0 ||
         strcmp(version, app->version) != 0 ||
         strcmp(runtime, solar_os_playground_runtime_name(app->runtime)) != 0 ||
         strcmp(entry, app->entry) != 0)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    solar_os_json_free(document);
    solar_os_zip_free(data);
    return err;
}

esp_err_t solar_os_playground_init(void)
{
    portENTER_CRITICAL(&playground_lock);
    if (playground_initialized) {
        portEXIT_CRITICAL(&playground_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&playground_lock);

    char stored_source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX] =
        SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE;
    char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX] =
        SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE;
    solar_os_playground_target_t storage =
        solar_os_storage_sd_is_mounted() ?
            SOLAR_OS_PLAYGROUND_TARGET_SD :
            SOLAR_OS_PLAYGROUND_TARGET_FLASH;
    nvs_handle_t nvs = 0;
    if (nvs_open(PLAYGROUND_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t source_len = sizeof(stored_source);
        if (nvs_get_str(nvs,
                        PLAYGROUND_NVS_SOURCE_KEY,
                        stored_source,
                        &source_len) != ESP_OK ||
            playground_normalize_source(stored_source,
                                        source,
                                        sizeof(source)) != ESP_OK) {
            strlcpy(source, SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE, sizeof(source));
        }
        uint8_t stored_storage = (uint8_t)SOLAR_OS_PLAYGROUND_TARGET_FLASH;
        if (nvs_get_u8(nvs,
                       PLAYGROUND_NVS_STORAGE_KEY,
                       &stored_storage) == ESP_OK &&
            (stored_storage == SOLAR_OS_PLAYGROUND_TARGET_FLASH ||
             stored_storage == SOLAR_OS_PLAYGROUND_TARGET_SD)) {
            storage = (solar_os_playground_target_t)stored_storage;
        }
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&playground_lock);
    strlcpy(playground_source, source, sizeof(playground_source));
    playground_storage = storage;
    playground_initialized = true;
    portEXIT_CRITICAL(&playground_lock);
    (void)playground_sync_aliases();
    return ESP_OK;
}

void solar_os_playground_get_source(char *url, size_t url_len)
{
    if (url == NULL || url_len == 0U) {
        return;
    }
    (void)solar_os_playground_init();
    portENTER_CRITICAL(&playground_lock);
    strlcpy(url, playground_source, url_len);
    portEXIT_CRITICAL(&playground_lock);
}

esp_err_t solar_os_playground_set_source(const char *url)
{
    char normalized[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    esp_err_t err = playground_normalize_source(
        url, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t nvs = 0;
    err = nvs_open(PLAYGROUND_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, PLAYGROUND_NVS_SOURCE_KEY, normalized);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        strlcpy(playground_source, normalized, sizeof(playground_source));
        playground_catalog_ready = false;
        playground_source_generation++;
        playground_initialized = true;
        portEXIT_CRITICAL(&playground_lock);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    return err;
}

esp_err_t solar_os_playground_reset_source(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(PLAYGROUND_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, PLAYGROUND_NVS_SOURCE_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        strlcpy(playground_source,
                SOLAR_OS_PLAYGROUND_DEFAULT_SOURCE,
                sizeof(playground_source));
        playground_catalog_ready = false;
        playground_source_generation++;
        playground_initialized = true;
        portEXIT_CRITICAL(&playground_lock);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    return err;
}

solar_os_playground_target_t solar_os_playground_get_storage(void)
{
    (void)solar_os_playground_init();
    portENTER_CRITICAL(&playground_lock);
    const solar_os_playground_target_t target = playground_storage;
    portEXIT_CRITICAL(&playground_lock);
    return target;
}

esp_err_t solar_os_playground_set_storage(
    solar_os_playground_target_t target)
{
    if (target != SOLAR_OS_PLAYGROUND_TARGET_FLASH &&
        target != SOLAR_OS_PLAYGROUND_TARGET_SD) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(PLAYGROUND_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, PLAYGROUND_NVS_STORAGE_KEY, (uint8_t)target);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        if (playground_storage != target) {
            playground_storage = target;
            playground_catalog_ready = false;
            playground_source_generation++;
        }
        playground_initialized = true;
        portEXIT_CRITICAL(&playground_lock);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    return err;
}

bool solar_os_playground_catalog_available(void)
{
    portENTER_CRITICAL(&playground_lock);
    const bool ready = playground_catalog_ready;
    portEXIT_CRITICAL(&playground_lock);
    return ready;
}

esp_err_t solar_os_playground_delete(void)
{
    (void)solar_os_playground_init();

    solar_os_playground_target_t storage =
        SOLAR_OS_PLAYGROUND_TARGET_FLASH;
    portENTER_CRITICAL(&playground_lock);
    if (playground_refresh_active) {
        portEXIT_CRITICAL(&playground_lock);
        return ESP_ERR_INVALID_STATE;
    }
    storage = playground_storage;
    playground_catalog_ready = false;
    playground_source_generation++;
    portEXIT_CRITICAL(&playground_lock);

    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    esp_err_t err =
        playground_mount_for_target(storage, mount, sizeof(mount));
    if (err != ESP_OK) {
        return err;
    }
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    err = playground_data_root(mount, root, sizeof(root), false);
    if (err == ESP_OK) {
        err = playground_remove_tree(root);
    }

    char legacy_root[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t legacy_err =
        playground_legacy_data_root(mount, legacy_root, sizeof(legacy_root));
    if (legacy_err == ESP_OK) {
        legacy_err = playground_remove_tree(legacy_root);
    }
    if (err == ESP_OK && legacy_err == ESP_OK) {
        (void)playground_sync_aliases();
    }
    return err != ESP_OK ? err : legacy_err;
}

esp_err_t solar_os_playground_reload(void)
{
    char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    solar_os_playground_target_t storage =
        SOLAR_OS_PLAYGROUND_TARGET_FLASH;
    uint32_t source_generation = 0U;
    (void)solar_os_playground_init();
    portENTER_CRITICAL(&playground_lock);
    if (playground_refresh_active) {
        portEXIT_CRITICAL(&playground_lock);
        return ESP_ERR_INVALID_STATE;
    }
    playground_refresh_active = true;
    source_generation = playground_source_generation;
    strlcpy(source, playground_source, sizeof(source));
    storage = playground_storage;
    playground_catalog_t *parsed =
        playground_catalog == &playground_catalog_banks[0]
            ? &playground_catalog_banks[1]
            : &playground_catalog_banks[0];
    portEXIT_CRITICAL(&playground_lock);

    playground_cache_paths_t paths = {0};
    esp_err_t err = playground_cache_paths(false, storage, &paths);
    char *stored_source = NULL;
    size_t stored_source_len = 0U;
    if (err == ESP_OK) {
        err = playground_read_file(paths.source,
                                   SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX - 1U,
                                   &stored_source,
                                   &stored_source_len);
    }
    if (err == ESP_OK &&
        (stored_source_len != strlen(source) ||
         memcmp(stored_source, source, stored_source_len) != 0)) {
        err = ESP_ERR_INVALID_STATE;
    }

    char *body = NULL;
    size_t body_len = 0U;
    if (err == ESP_OK) {
        err = playground_read_file(
            paths.catalog, PLAYGROUND_CATALOG_MAX, &body, &body_len);
    }
    if (err == ESP_OK) {
        memset(parsed, 0, sizeof(*parsed));
        err = playground_parse_catalog(body, body_len, parsed);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        if (source_generation == playground_source_generation) {
            playground_catalog = parsed;
            playground_catalog_ready = true;
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&playground_lock);
    }
    solar_os_memory_free(body);
    solar_os_memory_free(stored_source);
    portENTER_CRITICAL(&playground_lock);
    playground_refresh_active = false;
    portEXIT_CRITICAL(&playground_lock);
    return err;
}

void solar_os_playground_cancel(void)
{
    solar_os_http_request_t *request = NULL;
    portENTER_CRITICAL(&playground_lock);
    if (playground_active_request != NULL) {
        request = playground_active_request;
        playground_request_users++;
    }
    portEXIT_CRITICAL(&playground_lock);
    if (request != NULL) {
        (void)solar_os_http_request_cancel(request);
        portENTER_CRITICAL(&playground_lock);
        playground_request_users--;
        portEXIT_CRITICAL(&playground_lock);
    }
}

esp_err_t solar_os_playground_refresh(
    volatile bool *cancel,
    solar_os_playground_progress_fn progress_fn,
    void *progress_user)
{
    char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    solar_os_playground_target_t storage =
        SOLAR_OS_PLAYGROUND_TARGET_FLASH;
    uint32_t source_generation = 0U;
    (void)solar_os_playground_init();
    portENTER_CRITICAL(&playground_lock);
    if (playground_refresh_active) {
        portEXIT_CRITICAL(&playground_lock);
        return ESP_ERR_INVALID_STATE;
    }
    playground_refresh_active = true;
    source_generation = playground_source_generation;
    strlcpy(source, playground_source, sizeof(source));
    storage = playground_storage;
    portEXIT_CRITICAL(&playground_lock);

    char catalog_url[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    bool uses_github_api = false;
    esp_err_t err = playground_catalog_fetch_url(source,
                                                 catalog_url,
                                                 sizeof(catalog_url),
                                                 &uses_github_api);
    const solar_os_http_header_t github_headers[] = {
        {
            .name = "Accept",
            .value = "application/vnd.github.raw+json",
        },
        {
            .name = "X-GitHub-Api-Version",
            .value = "2022-11-28",
        },
    };
    char *body = NULL;
    size_t body_len = 0U;
    if (err == ESP_OK) {
        err = playground_download(
            catalog_url,
            PLAYGROUND_CATALOG_MAX,
            uses_github_api ? github_headers : NULL,
            uses_github_api ? sizeof(github_headers) / sizeof(github_headers[0])
                            : 0U,
            cancel,
            SOLAR_OS_PLAYGROUND_PROGRESS_CATALOG,
            progress_fn,
            progress_user,
            &body,
            &body_len);
    }
    playground_catalog_t *parsed = NULL;
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        parsed = playground_catalog == &playground_catalog_banks[0]
            ? &playground_catalog_banks[1]
            : &playground_catalog_banks[0];
        portEXIT_CRITICAL(&playground_lock);
        memset(parsed, 0, sizeof(*parsed));
    }
    if (err == ESP_OK) {
        err = playground_parse_catalog(body, body_len, parsed);
    }
    if (err == ESP_OK) {
        err = playground_save_catalog(source, body, body_len, storage);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&playground_lock);
        if (source_generation == playground_source_generation) {
            playground_catalog = parsed;
            playground_catalog_ready = true;
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&playground_lock);
    }
    if (err == ESP_OK) {
        solar_os_playground_progress_t progress = {
            .stage = SOLAR_OS_PLAYGROUND_PROGRESS_DONE,
        };
        playground_report(progress_fn, progress_user, &progress);
    }
    solar_os_memory_free(body);
    portENTER_CRITICAL(&playground_lock);
    playground_refresh_active = false;
    portEXIT_CRITICAL(&playground_lock);
    return err;
}

size_t solar_os_playground_category_count(void)
{
    portENTER_CRITICAL(&playground_lock);
    const size_t count =
        playground_catalog_ready ? playground_catalog->category_count : 0U;
    portEXIT_CRITICAL(&playground_lock);
    return count;
}

bool solar_os_playground_get_category(
    size_t index,
    solar_os_playground_category_t *category)
{
    if (category == NULL) {
        return false;
    }
    portENTER_CRITICAL(&playground_lock);
    const bool found = playground_catalog_ready &&
        index < playground_catalog->category_count;
    if (found) {
        *category = playground_catalog->categories[index];
    }
    portEXIT_CRITICAL(&playground_lock);
    return found;
}

size_t solar_os_playground_app_count(void)
{
    portENTER_CRITICAL(&playground_lock);
    const size_t count =
        playground_catalog_ready ? playground_catalog->app_count : 0U;
    portEXIT_CRITICAL(&playground_lock);
    return count;
}

bool solar_os_playground_get_app(size_t index,
                                 solar_os_playground_app_info_t *app)
{
    if (app == NULL) {
        return false;
    }
    portENTER_CRITICAL(&playground_lock);
    const bool found = playground_catalog_ready &&
        index < playground_catalog->app_count;
    if (found) {
        *app = playground_catalog->apps[index];
    }
    portEXIT_CRITICAL(&playground_lock);
    return found;
}

bool solar_os_playground_get_installed_app_id(size_t index,
                                              char *id,
                                              size_t id_len)
{
    if (id == NULL || id_len == 0U) {
        return false;
    }
    id[0] = '\0';
    char catalog_id[SOLAR_OS_PLAYGROUND_ID_MAX];
    char entry[SOLAR_OS_PLAYGROUND_ENTRY_MAX];
    solar_os_playground_runtime_t runtime =
        SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON;
    portENTER_CRITICAL(&playground_lock);
    const bool found = playground_catalog_ready &&
        index < playground_catalog->app_count;
    if (found) {
        const solar_os_playground_app_info_t *app =
            &playground_catalog->apps[index];
        strlcpy(catalog_id, app->id, sizeof(catalog_id));
        strlcpy(entry, app->entry, sizeof(entry));
        runtime = app->runtime;
    }
    portEXIT_CRITICAL(&playground_lock);
    if (!found) {
        return false;
    }
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    return playground_find_installed_root_fields(
               runtime, catalog_id, entry, root, sizeof(root)) &&
        strlcpy(id, catalog_id, id_len) < id_len;
}

bool solar_os_playground_find_app(const char *id,
                                  size_t *index,
                                  solar_os_playground_app_info_t *app)
{
    if (id == NULL) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&playground_lock);
    if (playground_catalog_ready) {
        for (size_t i = 0U; i < playground_catalog->app_count; i++) {
            if (strcmp(playground_catalog->apps[i].id, id) == 0) {
                if (index != NULL) {
                    *index = i;
                }
                if (app != NULL) {
                    *app = playground_catalog->apps[i];
                }
                found = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&playground_lock);
    return found;
}

bool solar_os_playground_is_installed(
    const solar_os_playground_app_info_t *app,
    char *version,
    size_t version_len)
{
    if (app == NULL) {
        return false;
    }
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    if (!playground_find_installed_root(app, root, sizeof(root))) {
        return false;
    }
    if (version != NULL && version_len > 0U &&
        playground_read_installed_version(root, version, version_len) != ESP_OK) {
        version[0] = '\0';
    }
    return true;
}

esp_err_t solar_os_playground_entry_path(
    const solar_os_playground_app_info_t *app,
    char *path,
    size_t path_len)
{
    if (app == NULL || path == NULL || path_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    if (!playground_find_installed_root(app, root, sizeof(root))) {
        return ESP_ERR_NOT_FOUND;
    }
    return solar_os_storage_join_path(root, app->entry, path, path_len);
}

esp_err_t solar_os_playground_install(
    const solar_os_playground_app_info_t *app,
    solar_os_playground_target_t target,
    volatile bool *cancel,
    solar_os_playground_progress_fn progress_fn,
    void *progress_user)
{
    if (app == NULL || !app->compatible) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    char package_url[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    solar_os_playground_get_source(source, sizeof(source));
    esp_err_t err = playground_join_url(source,
                                        app->archive,
                                        package_url,
                                        sizeof(package_url));
    char *archive = NULL;
    size_t archive_len = 0U;
    if (err == ESP_OK) {
        err = playground_download(
            package_url,
            app->size,
            NULL,
            0U,
            cancel,
            SOLAR_OS_PLAYGROUND_PROGRESS_PACKAGE,
            progress_fn,
            progress_user,
            &archive,
            &archive_len);
    }
    solar_os_playground_progress_t progress = {
        .stage = SOLAR_OS_PLAYGROUND_PROGRESS_VERIFY,
        .bytes_read = (uint32_t)archive_len,
        .bytes_total = app->size,
        .total_known = true,
    };
    if (err == ESP_OK) {
        playground_report(progress_fn, progress_user, &progress);
        uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
        err = archive_len == app->size ?
            solar_os_crypto_sha256_once(archive, archive_len, digest) :
            ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK &&
            !solar_os_crypto_sha256_matches_hex(digest, app->sha256)) {
            err = ESP_ERR_INVALID_CRC;
        }
    }

    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    char final_path[SOLAR_OS_STORAGE_PATH_MAX];
    char runtime_path[SOLAR_OS_STORAGE_PATH_MAX];
    char stage_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    char archive_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (err == ESP_OK) {
        err = playground_mount_for_target(target, mount, sizeof(mount));
    }
    if (err == ESP_OK) {
        err = playground_app_root(mount,
                                  app,
                                  final_path,
                                  sizeof(final_path),
                                  true);
    }
    if (err == ESP_OK) {
        strlcpy(runtime_path, final_path, sizeof(runtime_path));
        char *slash = strrchr(runtime_path, '/');
        if (slash == NULL) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            *slash = '\0';
        }
    }
    char stage_name[SOLAR_OS_PLAYGROUND_ID_MAX + 16U];
    char backup_name[SOLAR_OS_PLAYGROUND_ID_MAX + 16U];
    if (err == ESP_OK) {
        snprintf(stage_name, sizeof(stage_name), ".stage-%s", app->id);
        snprintf(backup_name, sizeof(backup_name), ".old-%s", app->id);
        err = solar_os_storage_join_path(runtime_path,
                                         stage_name,
                                         stage_path,
                                         sizeof(stage_path));
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(runtime_path,
                                         backup_name,
                                         backup_path,
                                         sizeof(backup_path));
    }
    if (err == ESP_OK) {
        struct stat final_stat;
        struct stat backup_stat;
        const bool final_exists = stat(final_path, &final_stat) == 0;
        const bool backup_exists = stat(backup_path, &backup_stat) == 0;
        if (!final_exists && backup_exists &&
            rename(backup_path, final_path) != 0) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        err = playground_remove_tree(stage_path);
    }
    if (err == ESP_OK) {
        err = playground_remove_tree(backup_path);
    }
    if (err == ESP_OK) {
        err = playground_mkdir_one(stage_path);
    }
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(stage_path,
                                         "package.sopkg",
                                         archive_path,
                                         sizeof(archive_path));
    }
    if (err == ESP_OK) {
        FILE *file = fopen(archive_path, "wb");
        if (file == NULL) {
            err = ESP_FAIL;
        } else {
            const size_t written = fwrite(archive, 1U, archive_len, file);
            const bool failed =
                written != archive_len || fflush(file) != 0 ||
                fsync(fileno(file)) != 0;
            fclose(file);
            if (failed) {
                err = ESP_FAIL;
            }
        }
    }
    solar_os_memory_free(archive);
    archive = NULL;

    progress.stage = SOLAR_OS_PLAYGROUND_PROGRESS_INSTALL;
    if (err == ESP_OK) {
        playground_report(progress_fn, progress_user, &progress);
        err = playground_validate_package_manifest(archive_path, app);
    }
    playground_zip_summary_t zip_summary = {
        .entry = app->entry,
    };
    if (err == ESP_OK) {
        err = solar_os_zip_list(
            archive_path, playground_zip_summarize, &zip_summary);
    }
    if (err == ESP_OK &&
        zip_summary.uncompressed_size > PLAYGROUND_EXTRACTED_MAX) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK &&
        (zip_summary.manifest_count != 1U ||
         zip_summary.entry_count != 1U)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        err = solar_os_zip_extract(archive_path, stage_path, NULL);
    }
    if (err == ESP_OK) {
        (void)remove(archive_path);
        char entry_path[SOLAR_OS_STORAGE_PATH_MAX];
        char manifest_path[SOLAR_OS_STORAGE_PATH_MAX];
        struct stat st;
        err = solar_os_storage_join_path(stage_path,
                                         app->entry,
                                         entry_path,
                                         sizeof(entry_path));
        if (err == ESP_OK &&
            (stat(entry_path, &st) != 0 || !S_ISREG(st.st_mode))) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK) {
            err = solar_os_storage_join_path(stage_path,
                                             PLAYGROUND_MANIFEST_FILE,
                                             manifest_path,
                                             sizeof(manifest_path));
        }
        if (err == ESP_OK &&
            (stat(manifest_path, &st) != 0 || !S_ISREG(st.st_mode))) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    struct stat final_stat;
    if (err == ESP_OK && stat(final_path, &final_stat) == 0) {
        if (rename(final_path, backup_path) != 0) {
            err = ESP_FAIL;
        }
    } else if (err == ESP_OK && errno != ENOENT) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && rename(stage_path, final_path) != 0) {
        (void)rename(backup_path, final_path);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        (void)playground_remove_tree(backup_path);
        progress.stage = SOLAR_OS_PLAYGROUND_PROGRESS_DONE;
        playground_report(progress_fn, progress_user, &progress);
        (void)playground_sync_aliases();
    } else {
        (void)playground_remove_tree(stage_path);
    }
    return err;
}

esp_err_t solar_os_playground_uninstall(
    const solar_os_playground_app_info_t *app,
    solar_os_playground_progress_fn progress_fn,
    void *progress_user)
{
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    if (!playground_find_installed_root(app, root, sizeof(root))) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_playground_progress_t progress = {
        .stage = SOLAR_OS_PLAYGROUND_PROGRESS_UNINSTALL,
    };
    playground_report(progress_fn, progress_user, &progress);
    const esp_err_t err = playground_remove_tree(root);
    if (err == ESP_OK) {
        progress.stage = SOLAR_OS_PLAYGROUND_PROGRESS_DONE;
        playground_report(progress_fn, progress_user, &progress);
        (void)playground_sync_aliases();
    }
    return err;
}

const char *solar_os_playground_runtime_name(
    solar_os_playground_runtime_t runtime)
{
    switch (runtime) {
    case SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON:
        return "python";
    case SOLAR_OS_PLAYGROUND_RUNTIME_LUA:
        return "lua";
    default:
        return "unknown";
    }
}

const char *solar_os_playground_target_name(
    solar_os_playground_target_t target)
{
    switch (target) {
    case SOLAR_OS_PLAYGROUND_TARGET_AUTO:
        return "auto";
    case SOLAR_OS_PLAYGROUND_TARGET_FLASH:
        return "flash";
    case SOLAR_OS_PLAYGROUND_TARGET_SD:
        return "sd";
    default:
        return "unknown";
    }
}
