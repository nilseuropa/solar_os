#include "solar_os_docs.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"

#include "solar_os_crypto.h"
#include "solar_os_http_client.h"
#include "solar_os_json.h"
#include "solar_os_log.h"
#include "solar_os_manual.h"
#include "solar_os_memory.h"
#include "solar_os_ota.h"
#include "solar_os_ota_key.h"
#include "solar_os_storage.h"
#include "solar_os_zip.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define DOCS_CATALOG_MAX (512U * 1024U)
#define DOCS_PAGE_COUNT_MAX 256U
#define DOCS_SIGNATURE_MAX 512U
#define DOCS_ARCHIVE_MAX (2U * 1024U * 1024U)
#define DOCS_URL_MAX 256U
#define DOCS_HTTP_TIMEOUT_MS 15000U
#define DOCS_HTTP_DEADLINE_MS 60000U
#define DOCS_SCHEMA "solaros.manual_catalog"
#define DOCS_SCHEMA_VERSION 2U
#define DOCS_ROOT_RELATIVE ".solar/docs"
#define DOCS_ACTIVE_FILE "active"
#define DOCS_ACTIVE_TEMP "active.new"
#define DOCS_ACTIVE_BACKUP "active.old"
#define DOCS_CATALOG_FILE "catalog.json"
#define DOCS_SIGNATURE_FILE "catalog.sig"
#define DOCS_ARCHIVE_FILE "manual.zip"

static const char *TAG = "solar_os_docs";

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    solar_os_docs_progress_fn progress_fn;
    void *progress_user;
    solar_os_docs_progress_t progress;
} docs_http_buffer_t;

typedef struct {
    char revision[SOLAR_OS_DOCS_REVISION_MAX];
    size_t page_count;
    char archive_path[32];
    char archive_sha[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
    uint32_t archive_size;
} docs_catalog_t;

static esp_err_t docs_validate_catalog_pages(
    const solar_os_json_value_t *pages,
    size_t page_count);

static portMUX_TYPE docs_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_docs_status_t docs_status;

static void docs_report_progress(solar_os_docs_progress_fn callback,
                                 void *user,
                                 solar_os_docs_progress_t *progress)
{
    if (callback != NULL && progress != NULL) {
        callback(progress, user);
    }
}

static void docs_set_error(const char *message)
{
    portENTER_CRITICAL(&docs_lock);
    strlcpy(docs_status.last_error,
            message != NULL ? message : "",
            sizeof(docs_status.last_error));
    portEXIT_CRITICAL(&docs_lock);
}

static void docs_set_result(bool available,
                            bool updating,
                            const char *revision,
                            size_t page_count,
                            const char *error)
{
    portENTER_CRITICAL(&docs_lock);
    docs_status.available = available;
    docs_status.updating = updating;
    strlcpy(docs_status.version, SOLAR_OS_VERSION, sizeof(docs_status.version));
    strlcpy(docs_status.revision,
            revision != NULL ? revision : "",
            sizeof(docs_status.revision));
    docs_status.page_count = page_count;
    strlcpy(docs_status.last_error,
            error != NULL ? error : "",
            sizeof(docs_status.last_error));
    portEXIT_CRITICAL(&docs_lock);
}

static bool docs_revision_valid(const char *revision)
{
    if (revision == NULL || strlen(revision) != SOLAR_OS_DOCS_REVISION_MAX - 1U) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)revision;
         *cursor != '\0';
         cursor++) {
        if (!isxdigit(*cursor) || isupper(*cursor)) {
            return false;
        }
    }
    return true;
}

static bool docs_id_valid(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)id;
         *cursor != '\0';
         cursor++) {
        if (!islower(*cursor) && !isdigit(*cursor) &&
            *cursor != '_' && *cursor != '.' && *cursor != '-') {
            return false;
        }
    }
    return true;
}

static esp_err_t docs_root_path(char *path, size_t path_len)
{
    if (!solar_os_storage_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_storage_join_path(solar_os_storage_sd_mount_point(),
                                      DOCS_ROOT_RELATIVE,
                                      path,
                                      path_len);
}

static esp_err_t docs_join(const char *base,
                           const char *name,
                           char *path,
                           size_t path_len)
{
    return solar_os_storage_join_path(base, name, path, path_len);
}

static esp_err_t docs_revision_path(const char *revision,
                                    char *path,
                                    size_t path_len)
{
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_root_path(root, sizeof(root));
    if (err != ESP_OK) {
        return err;
    }
    return docs_join(root, revision, path, path_len);
}

static esp_err_t docs_mkdir_one(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t docs_ensure_root(char *root, size_t root_len)
{
    if (!solar_os_storage_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *mount = solar_os_storage_sd_mount_point();
    char solar[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_join(mount, ".solar", solar, sizeof(solar));
    if (err == ESP_OK) {
        err = docs_mkdir_one(solar);
    }
    if (err == ESP_OK) {
        err = docs_join(solar, "docs", root, root_len);
    }
    if (err == ESP_OK) {
        err = docs_mkdir_one(root);
    }
    return err;
}

static esp_err_t docs_http_event(const solar_os_http_event_t *event, void *user_data)
{
    docs_http_buffer_t *buffer = (docs_http_buffer_t *)user_data;
    if (event == NULL || buffer == NULL) {
        return ESP_OK;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
        if (event->header_name != NULL && event->header_value != NULL &&
            strcasecmp(event->header_name, "Content-Length") == 0) {
            char *end = NULL;
            const unsigned long long parsed =
                strtoull(event->header_value, &end, 10);
            if (end != event->header_value && *end == '\0' &&
                parsed <= buffer->capacity - 1U) {
                buffer->progress.bytes_total = (size_t)parsed;
                buffer->progress.total_known = true;
                docs_report_progress(buffer->progress_fn,
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
    buffer->progress.bytes_read = buffer->length;
    docs_report_progress(buffer->progress_fn,
                         buffer->progress_user,
                         &buffer->progress);
    return ESP_OK;
}

static esp_err_t docs_download(const char *url,
                               size_t max_len,
                               char **body,
                               size_t *body_len,
                               solar_os_docs_progress_fn progress_fn,
                               void *progress_user,
                               const solar_os_docs_progress_t *progress)
{
    if (url == NULL || body == NULL || body_len == NULL || max_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *body = NULL;
    *body_len = 0U;
    char *data = solar_os_memory_alloc(max_len + 1U,
                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "docs.http");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    data[0] = '\0';
    docs_http_buffer_t buffer = {
        .data = data,
        .capacity = max_len + 1U,
        .progress_fn = progress_fn,
        .progress_user = progress_user,
    };
    if (progress != NULL) {
        buffer.progress = *progress;
    }
    docs_report_progress(progress_fn, progress_user, &buffer.progress);
    const solar_os_http_request_options_t options = {
        .url = url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .user_agent = "SolarOS-docs/" SOLAR_OS_VERSION,
        .follow_redirects = true,
        .timeout_ms = DOCS_HTTP_TIMEOUT_MS,
        .deadline_ms = DOCS_HTTP_DEADLINE_MS,
        .receive_buffer_size = 2048U,
        .transmit_buffer_size = 1024U,
        .event_handler = docs_http_event,
        .user_data = &buffer,
    };
    solar_os_http_request_t *request = NULL;
    esp_err_t err = solar_os_http_request_create(&options, &request);
    solar_os_http_response_t response = {0};
    if (err == ESP_OK) {
        err = solar_os_http_request_perform(request, &response);
    }
    if (request != NULL) {
        const esp_err_t destroy_err = solar_os_http_request_destroy(request);
        if (err == ESP_OK && destroy_err != ESP_OK) {
            err = destroy_err;
        }
    }
    if (err == ESP_OK && response.status_code != 200) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        solar_os_memory_free(data);
        return err;
    }
    buffer.progress.bytes_read = buffer.length;
    if (!buffer.progress.total_known && response.content_length >= 0 &&
        (uint64_t)response.content_length <= max_len) {
        buffer.progress.bytes_total = (size_t)response.content_length;
        buffer.progress.total_known = true;
    }
    docs_report_progress(progress_fn, progress_user, &buffer.progress);
    *body = data;
    *body_len = buffer.length;
    return ESP_OK;
}

static esp_err_t docs_write_file(const char *path, const void *data, size_t len)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const size_t written = len > 0U ? fwrite(data, 1U, len, file) : 0U;
    const bool failed = written != len || fflush(file) != 0 || fsync(fileno(file)) != 0;
    fclose(file);
    return failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t docs_read_file(const char *path,
                                size_t max_len,
                                char **body,
                                size_t *body_len)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t len = (size_t)st.st_size;
    char *data = solar_os_memory_alloc(len + 1U,
                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "docs.file");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    const size_t read_len = len > 0U ? fread(data, 1U, len, file) : 0U;
    const bool failed = read_len != len || ferror(file);
    fclose(file);
    if (failed) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    data[len] = '\0';
    *body = data;
    *body_len = len;
    return ESP_OK;
}

static esp_err_t docs_verify_signature(const char *catalog,
                                       size_t catalog_len,
                                       const char *signature)
{
    uint8_t der[SOLAR_OS_CRYPTO_ECDSA_P256_DER_SIGNATURE_MAX];
    size_t der_len = 0U;
    esp_err_t err =
        solar_os_crypto_base64_decode(signature, der, sizeof(der), &der_len);
    if (err == ESP_OK) {
        err = solar_os_crypto_ecdsa_p256_sha256_verify_pem(
            SOLAR_OS_OTA_PUBLIC_KEY_PEM, catalog, catalog_len, der, der_len);
    }
    return err;
}

static esp_err_t docs_parse_catalog(const char *catalog,
                                    size_t catalog_len,
                                    solar_os_json_doc_t **document,
                                    docs_catalog_t *info)
{
    if (catalog == NULL || document == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *document = NULL;
    memset(info, 0, sizeof(*info));
    esp_err_t err = solar_os_json_parse(catalog, catalog_len, document);
    if (err != ESP_OK) {
        return err;
    }
    const solar_os_json_value_t *root = solar_os_json_root(*document);
    char schema[32];
    char version[32];
    uint32_t schema_version = 0U;
    err = solar_os_json_get_path_string(root, "schema", schema, sizeof(schema));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(root, "schema_version", &schema_version);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root,
                                            "firmware_version",
                                            version,
                                            sizeof(version));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root,
                                            "revision",
                                            info->revision,
                                            sizeof(info->revision));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root,
                                            "archive.path",
                                            info->archive_path,
                                            sizeof(info->archive_path));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(
            root,
            "archive.sha256",
            info->archive_sha,
            sizeof(info->archive_sha));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(root,
                                            "archive.size",
                                            &info->archive_size);
    }
    const solar_os_json_value_t *pages =
        err == ESP_OK ? solar_os_json_object_get(root, "pages") : NULL;
    if (err != ESP_OK || strcmp(schema, DOCS_SCHEMA) != 0 ||
        schema_version != DOCS_SCHEMA_VERSION ||
        strcmp(version, SOLAR_OS_VERSION) != 0 ||
        !docs_revision_valid(info->revision) ||
        strcmp(info->archive_path, DOCS_ARCHIVE_FILE) != 0 ||
        !solar_os_crypto_sha256_hex_is_valid(info->archive_sha) ||
        info->archive_size == 0U ||
        info->archive_size > DOCS_ARCHIVE_MAX ||
        !solar_os_json_is_array(pages)) {
        solar_os_json_free(*document);
        *document = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    info->page_count = solar_os_json_array_size(pages);
    if (info->page_count < solar_os_manual_count() ||
        info->page_count > DOCS_PAGE_COUNT_MAX) {
        solar_os_json_free(*document);
        *document = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = docs_validate_catalog_pages(pages, info->page_count);
    if (err != ESP_OK) {
        solar_os_json_free(*document);
        *document = NULL;
        return err;
    }
    return ESP_OK;
}

static esp_err_t docs_page_metadata(const solar_os_json_value_t *page,
                                    char *id,
                                    size_t id_len,
                                    char *path,
                                    size_t path_len,
                                    char sha[static SOLAR_OS_CRYPTO_SHA256_HEX_LEN],
                                    uint32_t *size)
{
    esp_err_t err = solar_os_json_get_path_string(page, "id", id, id_len);
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(page, "path", path, path_len);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(page,
                                            "sha256",
                                            sha,
                                            SOLAR_OS_CRYPTO_SHA256_HEX_LEN);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(page, "size", size);
    }
    char expected[80];
    const int expected_len = snprintf(expected, sizeof(expected), "manual/%s.md", id);
    if (err != ESP_OK || !docs_id_valid(id) ||
        expected_len < 0 || (size_t)expected_len >= sizeof(expected) ||
        strcmp(path, expected) != 0 ||
        !solar_os_crypto_sha256_hex_is_valid(sha) ||
        *size == 0U || *size > SOLAR_OS_DOCS_PAGE_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t docs_validate_catalog_pages(
    const solar_os_json_value_t *pages,
    size_t page_count)
{
    for (size_t i = 0U; i < page_count; i++) {
        const solar_os_json_value_t *page = solar_os_json_array_get(pages, i);
        char id[64];
        char relative[80];
        char sha[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
        uint32_t expected_size = 0U;
        esp_err_t err = docs_page_metadata(page,
                                           id,
                                           sizeof(id),
                                           relative,
                                           sizeof(relative),
                                           sha,
                                           &expected_size);
        if (err != ESP_OK) {
            return err;
        }
        for (size_t previous = 0U; previous < i; previous++) {
            char previous_id[64];
            err = solar_os_json_get_path_string(
                solar_os_json_array_get(pages, previous),
                "id",
                previous_id,
                sizeof(previous_id));
            if (err != ESP_OK || strcmp(previous_id, id) == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    for (size_t topic = 0U; topic < solar_os_manual_count(); topic++) {
        const solar_os_manual_page_t *manual = solar_os_manual_get(topic);
        bool found = false;
        for (size_t page_index = 0U;
             manual != NULL && page_index < page_count;
             page_index++) {
            char id[64];
            const esp_err_t err = solar_os_json_get_path_string(
                solar_os_json_array_get(pages, page_index),
                "id",
                id,
                sizeof(id));
            if (err != ESP_OK) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (strcmp(id, manual->id) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static esp_err_t docs_verify_data(const void *data,
                                  size_t len,
                                  uint32_t expected_size,
                                  const char *expected_sha)
{
    if (len != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    esp_err_t err = solar_os_crypto_sha256_once(data, len, digest);
    if (err == ESP_OK &&
        !solar_os_crypto_sha256_matches_hex(digest, expected_sha)) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

static esp_err_t docs_verify_catalog_files(
    const char *base,
    const solar_os_json_value_t *pages,
    size_t page_count)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    for (size_t i = 0U; i < page_count; i++) {
        const solar_os_json_value_t *page = solar_os_json_array_get(pages, i);
        char id[64];
        char relative[80];
        char sha[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
        uint32_t expected_size = 0U;
        esp_err_t err = docs_page_metadata(page,
                                           id,
                                           sizeof(id),
                                           relative,
                                           sizeof(relative),
                                           sha,
                                           &expected_size);
        char *data = NULL;
        size_t data_len = 0U;
        if (err == ESP_OK) {
            err = docs_join(base, relative, path, sizeof(path));
        }
        if (err == ESP_OK) {
            err = docs_read_file(path, SOLAR_OS_DOCS_PAGE_MAX, &data, &data_len);
        }
        if (err == ESP_OK) {
            err = docs_verify_data(data, data_len, expected_size, sha);
        }
        solar_os_memory_free(data);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t docs_verify_revision(const char *revision,
                                      docs_catalog_t *verified,
                                      bool verify_files)
{
    char base[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_revision_path(revision, base, sizeof(base));
    char *catalog = NULL;
    char *signature = NULL;
    size_t catalog_len = 0U;
    size_t signature_len = 0U;
    solar_os_json_doc_t *document = NULL;
    docs_catalog_t info = {0};
    if (err == ESP_OK) {
        err = docs_join(base, DOCS_CATALOG_FILE, path, sizeof(path));
    }
    if (err == ESP_OK) {
        err = docs_read_file(path, DOCS_CATALOG_MAX, &catalog, &catalog_len);
    }
    if (err == ESP_OK) {
        err = docs_join(base, DOCS_SIGNATURE_FILE, path, sizeof(path));
    }
    if (err == ESP_OK) {
        err = docs_read_file(path,
                             DOCS_SIGNATURE_MAX,
                             &signature,
                             &signature_len);
    }
    if (err == ESP_OK) {
        err = docs_verify_signature(catalog, catalog_len, signature);
    }
    if (err == ESP_OK) {
        err = docs_parse_catalog(catalog, catalog_len, &document, &info);
    }
    if (err == ESP_OK && strcmp(revision, info.revision) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    const solar_os_json_value_t *pages =
        document != NULL ?
            solar_os_json_object_get(solar_os_json_root(document), "pages") : NULL;
    if (err == ESP_OK && verify_files) {
        err = docs_verify_catalog_files(base, pages, info.page_count);
    }
    if (err == ESP_OK && verified != NULL) {
        *verified = info;
    }
    solar_os_json_free(document);
    solar_os_memory_free(signature);
    solar_os_memory_free(catalog);
    return err;
}

static esp_err_t docs_read_revision_pointer(
    const char *name,
    char revision[SOLAR_OS_DOCS_REVISION_MAX])
{
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_root_path(root, sizeof(root));
    if (err == ESP_OK) {
        err = docs_join(root, name, path, sizeof(path));
    }
    char *data = NULL;
    size_t len = 0U;
    if (err == ESP_OK) {
        err = docs_read_file(path, SOLAR_OS_DOCS_REVISION_MAX, &data, &len);
    }
    if (err == ESP_OK) {
        while (len > 0U && isspace((unsigned char)data[len - 1U])) {
            data[--len] = '\0';
        }
        if (!docs_revision_valid(data)) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            strlcpy(revision, data, SOLAR_OS_DOCS_REVISION_MAX);
        }
    }
    solar_os_memory_free(data);
    return err;
}

static esp_err_t docs_read_active_revision(char revision[SOLAR_OS_DOCS_REVISION_MAX])
{
    esp_err_t err = docs_read_revision_pointer(DOCS_ACTIVE_FILE, revision);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        err = docs_read_revision_pointer(DOCS_ACTIVE_BACKUP, revision);
    }
    return err;
}

static esp_err_t docs_activate(const docs_catalog_t *info)
{
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    char active[SOLAR_OS_STORAGE_PATH_MAX];
    char temporary[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_ensure_root(root, sizeof(root));
    if (err == ESP_OK) {
        err = docs_join(root, DOCS_ACTIVE_FILE, active, sizeof(active));
    }
    if (err == ESP_OK) {
        err = docs_join(root, DOCS_ACTIVE_TEMP, temporary, sizeof(temporary));
    }
    if (err == ESP_OK) {
        err = docs_join(root, DOCS_ACTIVE_BACKUP, backup, sizeof(backup));
    }
    char pointer[SOLAR_OS_DOCS_REVISION_MAX + 1U];
    const int len = snprintf(pointer, sizeof(pointer), "%s\n", info->revision);
    if (err == ESP_OK && (len < 0 || (size_t)len >= sizeof(pointer))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = docs_write_file(temporary, pointer, (size_t)len);
    }
    if (err == ESP_OK) {
        (void)remove(backup);
        if (rename(active, backup) != 0 && errno != ENOENT) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK && rename(temporary, active) != 0) {
        (void)rename(backup, active);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        (void)remove(backup);
    }
    return err;
}

static esp_err_t docs_exact_release_base(char *url, size_t url_len)
{
    char configured[SOLAR_OS_OTA_URL_MAX];
    solar_os_ota_get_url(configured, sizeof(configured));
    size_t len = strlen(configured);
    while (len > 0U && configured[len - 1U] == '/') {
        configured[--len] = '\0';
    }
    const char suffix[] = "/latest";
    if (len >= sizeof(suffix) - 1U &&
        strcmp(configured + len - (sizeof(suffix) - 1U), suffix) == 0) {
        configured[len - (sizeof(suffix) - 1U)] = '\0';
        const int written =
            snprintf(url, url_len, "%s/%s", configured, SOLAR_OS_VERSION);
        return written >= 0 && (size_t)written < url_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    return strlcpy(url, configured, url_len) < url_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t docs_url(const char *base,
                          const char *relative,
                          char *url,
                          size_t url_len)
{
    const int written = snprintf(url, url_len, "%s/doc/%s", base, relative);
    return written >= 0 && (size_t)written < url_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_docs_init(void)
{
    docs_set_result(false, false, "", 0U, "");
    char revision[SOLAR_OS_DOCS_REVISION_MAX];
    esp_err_t err = docs_read_active_revision(revision);
    docs_catalog_t info;
    /* Updates verify every page before activation. At boot, revalidate only
     * the signed catalog so slow removable storage does not delay the shell. */
    if (err == ESP_OK) {
        err = docs_verify_revision(revision, &info, false);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        const esp_err_t backup_err =
            docs_read_revision_pointer(DOCS_ACTIVE_BACKUP, revision);
        if (backup_err == ESP_OK) {
            err = docs_verify_revision(revision, &info, false);
        }
    }
    if (err == ESP_OK) {
        docs_set_result(true, false, info.revision, info.page_count, "");
    } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE) {
        docs_set_error("cached documentation is invalid");
        SOLAR_OS_LOGW(TAG, "cached documentation rejected: %s", esp_err_to_name(err));
    }
    return err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t solar_os_docs_get_status(solar_os_docs_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&docs_lock);
    *status = docs_status;
    portEXIT_CRITICAL(&docs_lock);
    return ESP_OK;
}

esp_err_t solar_os_docs_page_path(const char *id, char *path, size_t path_len)
{
    if (!docs_id_valid(id) || path == NULL || path_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char revision[SOLAR_OS_DOCS_REVISION_MAX];
    portENTER_CRITICAL(&docs_lock);
    const bool available = docs_status.available;
    strlcpy(revision, docs_status.revision, sizeof(revision));
    portEXIT_CRITICAL(&docs_lock);
    if (!available) {
        return ESP_ERR_NOT_FOUND;
    }
    char base[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_revision_path(revision, base, sizeof(base));
    if (err != ESP_OK) {
        return err;
    }
    char relative[80];
    const int written = snprintf(relative, sizeof(relative), "manual/%s.md", id);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return docs_join(base, relative, path, path_len);
}

typedef struct {
    solar_os_docs_progress_fn callback;
    void *user;
    solar_os_docs_progress_t progress;
} docs_extract_progress_t;

static void docs_extract_progress_cb(
    const solar_os_zip_event_info_t *info,
    void *user)
{
    docs_extract_progress_t *state = (docs_extract_progress_t *)user;
    if (info == NULL || state == NULL ||
        info->event != SOLAR_OS_ZIP_EVENT_EXTRACT) {
        return;
    }
    if (state->progress.page_index < state->progress.page_count) {
        state->progress.page_index++;
    }
    state->progress.bytes_read = state->progress.page_index;
    docs_report_progress(state->callback, state->user, &state->progress);
}

esp_err_t solar_os_docs_update(solar_os_docs_progress_fn progress_fn,
                               void *progress_user)
{
    solar_os_docs_status_t before;
    portENTER_CRITICAL(&docs_lock);
    before = docs_status;
    if (docs_status.updating) {
        portEXIT_CRITICAL(&docs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    docs_status.updating = true;
    docs_status.last_error[0] = '\0';
    portEXIT_CRITICAL(&docs_lock);

    char base_url[DOCS_URL_MAX];
    char url[DOCS_URL_MAX];
    char *catalog = NULL;
    char *signature = NULL;
    char *archive = NULL;
    size_t catalog_len = 0U;
    size_t signature_len = 0U;
    size_t archive_len = 0U;
    solar_os_json_doc_t *document = NULL;
    docs_catalog_t info;
    solar_os_docs_progress_t progress = {
        .stage = SOLAR_OS_DOCS_PROGRESS_CATALOG,
    };
    esp_err_t err = docs_exact_release_base(base_url, sizeof(base_url));
    if (err == ESP_OK) {
        err = docs_url(base_url, DOCS_CATALOG_FILE, url, sizeof(url));
    }
    if (err == ESP_OK) {
        err = docs_download(url,
                            DOCS_CATALOG_MAX,
                            &catalog,
                            &catalog_len,
                            progress_fn,
                            progress_user,
                            &progress);
    }
    if (err == ESP_OK) {
        err = docs_url(base_url, DOCS_SIGNATURE_FILE, url, sizeof(url));
    }
    if (err == ESP_OK) {
        memset(&progress, 0, sizeof(progress));
        progress.stage = SOLAR_OS_DOCS_PROGRESS_SIGNATURE;
        err = docs_download(url,
                            DOCS_SIGNATURE_MAX,
                            &signature,
                            &signature_len,
                            progress_fn,
                            progress_user,
                            &progress);
    }
    if (err == ESP_OK) {
        memset(&progress, 0, sizeof(progress));
        progress.stage = SOLAR_OS_DOCS_PROGRESS_VERIFYING;
        docs_report_progress(progress_fn, progress_user, &progress);
        err = docs_verify_signature(catalog, catalog_len, signature);
    }
    if (err == ESP_OK) {
        err = docs_parse_catalog(catalog, catalog_len, &document, &info);
    }

    char root[SOLAR_OS_STORAGE_PATH_MAX];
    char stage[SOLAR_OS_STORAGE_PATH_MAX];
    char file_path[SOLAR_OS_STORAGE_PATH_MAX];
    char archive_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (err == ESP_OK) {
        err = docs_ensure_root(root, sizeof(root));
    }
    char stage_name[32];
    if (err == ESP_OK) {
        const int written =
            snprintf(stage_name, sizeof(stage_name), ".stage-%s", info.revision);
        if (written < 0 || (size_t)written >= sizeof(stage_name)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err == ESP_OK) {
        err = docs_join(root, stage_name, stage, sizeof(stage));
    }
    if (err == ESP_OK) {
        err = docs_mkdir_one(stage);
    }
    if (err == ESP_OK) {
        err = docs_join(stage, DOCS_CATALOG_FILE, file_path, sizeof(file_path));
    }
    if (err == ESP_OK) {
        err = docs_write_file(file_path, catalog, catalog_len);
    }
    if (err == ESP_OK) {
        err = docs_join(stage, DOCS_SIGNATURE_FILE, file_path, sizeof(file_path));
    }
    if (err == ESP_OK) {
        err = docs_write_file(file_path, signature, signature_len);
    }

    const solar_os_json_value_t *pages =
        document != NULL ?
            solar_os_json_object_get(solar_os_json_root(document), "pages") : NULL;
    if (err == ESP_OK) {
        err = docs_url(base_url, info.archive_path, url, sizeof(url));
    }
    if (err == ESP_OK) {
        memset(&progress, 0, sizeof(progress));
        progress.stage = SOLAR_OS_DOCS_PROGRESS_ARCHIVE;
        progress.bytes_total = info.archive_size;
        progress.total_known = true;
        err = docs_download(url,
                            info.archive_size,
                            &archive,
                            &archive_len,
                            progress_fn,
                            progress_user,
                            &progress);
    }
    if (err == ESP_OK) {
        err = docs_verify_data(archive,
                               archive_len,
                               info.archive_size,
                               info.archive_sha);
    }
    if (err == ESP_OK) {
        err = docs_join(stage,
                        info.archive_path,
                        archive_path,
                        sizeof(archive_path));
    }
    if (err == ESP_OK) {
        err = docs_write_file(archive_path, archive, archive_len);
    }
    solar_os_memory_free(archive);
    archive = NULL;

    docs_extract_progress_t extract = {
        .callback = progress_fn,
        .user = progress_user,
        .progress = {
            .stage = SOLAR_OS_DOCS_PROGRESS_EXTRACTING,
            .page_count = info.page_count,
            .bytes_total = info.page_count,
            .total_known = true,
        },
    };
    if (err == ESP_OK) {
        docs_report_progress(progress_fn, progress_user, &extract.progress);
        const solar_os_unzip_options_t options = {
            .progress = docs_extract_progress_cb,
            .user = &extract,
        };
        err = solar_os_zip_extract(archive_path, stage, &options);
    }
    if (err == ESP_OK) {
        (void)remove(archive_path);
        memset(&progress, 0, sizeof(progress));
        progress.stage = SOLAR_OS_DOCS_PROGRESS_VERIFYING;
        progress.page_count = info.page_count;
        docs_report_progress(progress_fn, progress_user, &progress);
        err = docs_verify_catalog_files(stage, pages, info.page_count);
    }

    char final_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (err == ESP_OK) {
        err = docs_join(root, info.revision, final_path, sizeof(final_path));
    }
    struct stat final_stat;
    if (err == ESP_OK && stat(final_path, &final_stat) == 0) {
        if (S_ISDIR(final_stat.st_mode)) {
            err = docs_verify_revision(info.revision, NULL, true);
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
    } else if (err == ESP_OK && errno != ENOENT) {
        err = ESP_FAIL;
    } else if (err == ESP_OK && rename(stage, final_path) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        progress.stage = SOLAR_OS_DOCS_PROGRESS_ACTIVATING;
        docs_report_progress(progress_fn, progress_user, &progress);
        err = docs_activate(&info);
    }

    solar_os_json_free(document);
    solar_os_memory_free(signature);
    solar_os_memory_free(catalog);
    if (err == ESP_OK) {
        docs_set_result(true, false, info.revision, info.page_count, "");
        memset(&progress, 0, sizeof(progress));
        progress.stage = SOLAR_OS_DOCS_PROGRESS_DONE;
        progress.page_index = info.page_count;
        progress.page_count = info.page_count;
        docs_report_progress(progress_fn, progress_user, &progress);
    } else {
        char message[SOLAR_OS_DOCS_ERROR_MAX];
        snprintf(message, sizeof(message), "update failed: %s", esp_err_to_name(err));
        docs_set_result(before.available,
                        false,
                        before.revision,
                        before.page_count,
                        message);
        SOLAR_OS_LOGW(TAG, "%s", message);
    }
    return err;
}

esp_err_t solar_os_docs_reset(void)
{
    solar_os_docs_status_t before;
    portENTER_CRITICAL(&docs_lock);
    before = docs_status;
    if (docs_status.updating) {
        portEXIT_CRITICAL(&docs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    docs_status.updating = true;
    portEXIT_CRITICAL(&docs_lock);

    char root[SOLAR_OS_STORAGE_PATH_MAX];
    char active[SOLAR_OS_STORAGE_PATH_MAX];
    char temporary[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = docs_root_path(root, sizeof(root));
    if (err == ESP_OK) {
        err = docs_join(root, DOCS_ACTIVE_FILE, active, sizeof(active));
    }
    if (err == ESP_OK) {
        err = docs_join(root, DOCS_ACTIVE_TEMP, temporary, sizeof(temporary));
    }
    if (err == ESP_OK) {
        err = docs_join(root, DOCS_ACTIVE_BACKUP, backup, sizeof(backup));
    }
    if (err == ESP_OK) {
        if (remove(active) != 0 && errno != ENOENT) {
            err = ESP_FAIL;
        }
        (void)remove(temporary);
        (void)remove(backup);
    }
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        docs_set_result(false, false, "", 0U, "");
        return ESP_OK;
    }
    docs_set_result(before.available,
                    false,
                    before.revision,
                    before.page_count,
                    "reset failed");
    return err;
}
