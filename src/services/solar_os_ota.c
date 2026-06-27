#include "solar_os_ota.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_timer.h"
#include "solar_os_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "solar_os_board.h"
#include "solar_os_config.h"
#include "solar_os_crypto.h"
#include "solar_os_json.h"
#include "solar_os_ota_key.h"

#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_URL_KEY "url"
#define OTA_NVS_FLAVOR_KEY "flavor"
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_INDEX_MAX 16384
#define OTA_INDEX_SIGNATURE_MAX 512
#define OTA_HTTP_FETCH_URL_MAX (SOLAR_OS_OTA_ARTIFACT_URL_MAX + 40)

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#ifndef SOLAR_OS_FLAVOR_NAME
#define SOLAR_OS_FLAVOR_NAME "full"
#endif

static char ota_url[SOLAR_OS_OTA_URL_MAX] = SOLAR_OS_OTA_DEFAULT_URL;
static char ota_target_flavor[SOLAR_OS_OTA_FLAVOR_MAX] = SOLAR_OS_FLAVOR_NAME;
static bool ota_loaded;
static const char *TAG = "solar_os_ota";

typedef struct {
    char *body;
    size_t body_len;
    size_t body_cap;
    bool truncated;
} ota_http_body_t;

typedef struct {
    solar_os_crypto_sha256_t sha256;
    uint32_t bytes_hashed;
    bool hash_failed;
} ota_firmware_verify_t;

static void ota_load(void);
static esp_err_t ota_join_url(const char *base_url,
                              const char *path,
                              char *out,
                              size_t out_len);

static esp_err_t ota_make_uncached_url(const char *url, char *out, size_t out_len)
{
    if (url == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *separator = strchr(url, '?') != NULL ? "&" : "?";
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s_solaros=%" PRId64,
                                 url,
                                 separator,
                                 (int64_t)esp_timer_get_time());
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void *ota_malloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }

    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void ota_free(void *ptr)
{
    heap_caps_free(ptr);
}

static bool ota_url_is_valid(const char *url)
{
    if (url == NULL || url[0] == '\0' || strlen(url) >= SOLAR_OS_OTA_URL_MAX) {
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)url; *p != '\0'; p++) {
        if (!isprint(*p) || isspace(*p)) {
            return false;
        }
    }
    return true;
}

static bool ota_flavor_is_valid(const char *flavor)
{
    if (flavor == NULL || flavor[0] == '\0' || strlen(flavor) >= SOLAR_OS_OTA_FLAVOR_MAX) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)flavor; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

static bool ota_url_has_suffix(const char *url, const char *suffix)
{
    if (url == NULL || suffix == NULL) {
        return false;
    }
    const size_t url_len = strlen(url);
    const size_t suffix_len = strlen(suffix);
    return url_len >= suffix_len && strcmp(url + url_len - suffix_len, suffix) == 0;
}

static esp_err_t ota_build_index_url(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_load();

    if (ota_url_has_suffix(ota_url, ".json")) {
        return strlcpy(out, ota_url, out_len) < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    const size_t url_len = strlen(ota_url);
    const bool has_slash = url_len > 0 && ota_url[url_len - 1] == '/';
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 ota_url,
                                 has_slash ? "" : "/",
                                 SOLAR_OS_OTA_INDEX_FILE);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t ota_url_directory(const char *url, char *out, size_t out_len)
{
    if (url == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *slash = strrchr(url, '/');
    if (slash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t len = (size_t)(slash - url + 1);
    if (len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, url, len);
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t ota_build_index_signature_url(const char *index_url, char *out, size_t out_len)
{
    if (index_url == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char base_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    esp_err_t err = ota_url_directory(index_url, base_url, sizeof(base_url));
    if (err != ESP_OK) {
        return err;
    }
    return ota_join_url(base_url, SOLAR_OS_OTA_INDEX_SIGNATURE_FILE, out, out_len);
}

static bool ota_path_is_absolute_url(const char *path)
{
    return path != NULL &&
        (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
}

static bool ota_relative_path_is_valid(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return false;
    }

    const char *p = path;
    while (*p != '\0') {
        if (!isprint((unsigned char)*p) || isspace((unsigned char)*p)) {
            return false;
        }
        if ((p == path || p[-1] == '/') &&
            p[0] == '.' &&
            p[1] == '.' &&
            (p[2] == '/' || p[2] == '\0')) {
            return false;
        }
        p++;
    }
    return true;
}

static esp_err_t ota_join_url(const char *base_url,
                              const char *path,
                              char *out,
                              size_t out_len)
{
    if (base_url == NULL || path == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ota_path_is_absolute_url(path)) {
        if (!ota_url_is_valid(path)) {
            return ESP_ERR_INVALID_ARG;
        }
        return strlcpy(out, path, out_len) < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    if (!ota_relative_path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t base_len = strlen(base_url);
    const bool has_slash = base_len > 0 && base_url[base_len - 1] == '/';
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 base_url,
                                 has_slash ? "" : "/",
                                 path);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void ota_load(void)
{
    if (ota_loaded) {
        return;
    }
    ota_loaded = true;

    strlcpy(ota_target_flavor, SOLAR_OS_FLAVOR_NAME, sizeof(ota_target_flavor));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return;
    }

    char stored_url[SOLAR_OS_OTA_URL_MAX];
    size_t url_len = sizeof(stored_url);
    ret = nvs_get_str(nvs, OTA_NVS_URL_KEY, stored_url, &url_len);
    if (ret == ESP_OK && ota_url_is_valid(stored_url)) {
        strlcpy(ota_url, stored_url, sizeof(ota_url));
    }

    char stored_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
    size_t flavor_len = sizeof(stored_flavor);
    ret = nvs_get_str(nvs, OTA_NVS_FLAVOR_KEY, stored_flavor, &flavor_len);
    nvs_close(nvs);

    if (ret == ESP_OK && ota_flavor_is_valid(stored_flavor)) {
        strlcpy(ota_target_flavor, stored_flavor, sizeof(ota_target_flavor));
    }
}

static esp_err_t ota_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, OTA_NVS_URL_KEY, ota_url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, OTA_NVS_FLAVOR_KEY, ota_target_flavor);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t ota_http_event(esp_http_client_event_t *event)
{
    if (event == NULL ||
        event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }

    ota_http_body_t *body = (ota_http_body_t *)event->user_data;
    if (body == NULL || body->body == NULL || body->body_cap == 0) {
        return ESP_OK;
    }

    const size_t remaining = body->body_cap - body->body_len - 1;
    const size_t copy_len = (size_t)event->data_len > remaining ?
        remaining :
        (size_t)event->data_len;
    if (copy_len > 0) {
        memcpy(body->body + body->body_len, event->data, copy_len);
        body->body_len += copy_len;
        body->body[body->body_len] = '\0';
    }
    if (copy_len < (size_t)event->data_len) {
        body->truncated = true;
    }
    return ESP_OK;
}

static esp_err_t ota_firmware_http_event(esp_http_client_event_t *event)
{
    if (event == NULL ||
        event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }

    const int status = event->client != NULL ? esp_http_client_get_status_code(event->client) : 0;
    if (status < 200 || status >= 300) {
        return ESP_OK;
    }

    ota_firmware_verify_t *verify = (ota_firmware_verify_t *)event->user_data;
    if (verify == NULL) {
        return ESP_OK;
    }

    const esp_err_t err =
        solar_os_crypto_sha256_update(&verify->sha256, event->data, (size_t)event->data_len);
    if (err != ESP_OK) {
        verify->hash_failed = true;
        return err;
    }
    if (UINT32_MAX - verify->bytes_hashed < (uint32_t)event->data_len) {
        verify->hash_failed = true;
        return ESP_ERR_INVALID_SIZE;
    }
    verify->bytes_hashed += (uint32_t)event->data_len;
    return ESP_OK;
}

static esp_err_t ota_http_get_text(const char *url,
                                   char *body_buffer,
                                   size_t body_buffer_len,
                                   int *status_code,
                                   int64_t *content_length)
{
    if (url == NULL || body_buffer == NULL || body_buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_http_body_t body = {
        .body = body_buffer,
        .body_cap = body_buffer_len,
    };
    body_buffer[0] = '\0';
    if (status_code != NULL) {
        *status_code = -1;
    }
    if (content_length != NULL) {
        *content_length = -1;
    }

    char fetch_url[OTA_HTTP_FETCH_URL_MAX];
    esp_err_t err = ota_make_uncached_url(url, fetch_url, sizeof(fetch_url));
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_config_t config = {
        .url = fetch_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .event_handler = ota_http_event,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "SolarOS-ota/0.1",
        .user_data = &body,
        .max_redirection_count = 3,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    SOLAR_OS_LOGI(TAG, "check %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)esp_http_client_set_header(client, "Cache-Control", "no-cache, no-store, max-age=0");
    (void)esp_http_client_set_header(client, "Pragma", "no-cache");

    err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    const int64_t http_len = esp_http_client_get_content_length(client);
    if (status_code != NULL) {
        *status_code = http_status;
    }
    if (content_length != NULL) {
        *content_length = http_len;
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "check failed: %s", esp_err_to_name(err));
        return err;
    }
    if (http_status < 200 || http_status >= 300) {
        SOLAR_OS_LOGW(TAG, "check HTTP %d", http_status);
        return ESP_FAIL;
    }
    if (body.truncated) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t ota_http_get_text_alloc(const char *url,
                                         size_t max_len,
                                         char **out_body,
                                         size_t *out_len,
                                         int *status_code,
                                         int64_t *content_length)
{
    if (url == NULL || out_body == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }

    char *body = ota_malloc(max_len + 1U);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = ota_http_get_text(url,
                                            body,
                                            max_len + 1U,
                                            status_code,
                                            content_length);
    if (err != ESP_OK) {
        ota_free(body);
        return err;
    }

    if (out_len != NULL) {
        *out_len = strlen(body);
    }
    *out_body = body;
    return ESP_OK;
}

static esp_err_t ota_verify_index_signature(const char *index_body,
                                            size_t index_len,
                                            const char *signature_body,
                                            size_t signature_len)
{
    if (index_body == NULL || index_len == 0 || signature_body == NULL ||
        signature_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t signature_der[SOLAR_OS_CRYPTO_ECDSA_P256_DER_SIGNATURE_MAX];
    size_t signature_der_len = 0;
    esp_err_t err = solar_os_crypto_base64_decode(signature_body,
                                                  signature_der,
                                                  sizeof(signature_der),
                                                  &signature_der_len);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "index signature decode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = solar_os_crypto_ecdsa_p256_sha256_verify_pem(SOLAR_OS_OTA_PUBLIC_KEY_PEM,
                                                       index_body,
                                                       index_len,
                                                       signature_der,
                                                       signature_der_len);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "index signature verify failed: %s", esp_err_to_name(err));
        return err;
    }

    SOLAR_OS_LOGI(TAG, "index signature verified");
    return ESP_OK;
}

static esp_err_t ota_index_base_url(const solar_os_json_value_t *root,
                                    const char *index_url,
                                    char *base_url,
                                    size_t base_url_len)
{
    if (root == NULL || index_url == NULL || base_url == NULL || base_url_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t base_err =
        solar_os_json_get_path_string(root, "base_url", base_url, base_url_len);
    if (base_err == ESP_OK) {
        return ota_url_is_valid(base_url) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (base_err != ESP_ERR_NOT_FOUND) {
        return base_err;
    }

    return ota_url_directory(index_url, base_url, base_url_len);
}

static esp_err_t ota_read_artifact_string(const solar_os_json_value_t *artifact,
                                          const char *path,
                                          char *out,
                                          size_t out_len)
{
    const esp_err_t err = solar_os_json_get_path_string(artifact, path, out, out_len);
    return err == ESP_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t ota_fill_artifact_from_json(const solar_os_json_value_t *artifact,
                                             const char *base_url,
                                             solar_os_ota_check_result_t *result)
{
    if (artifact == NULL || base_url == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char firmware_path[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    char manifest_path[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    uint32_t image_size = 0;

    esp_err_t err = ota_read_artifact_string(artifact,
                                             "version",
                                             result->available_version,
                                             sizeof(result->available_version));
    if (err != ESP_OK) {
        return err;
    }

    err = ota_read_artifact_string(artifact,
                                   "firmware",
                                   firmware_path,
                                   sizeof(firmware_path));
    if (err != ESP_OK) {
        return err;
    }

    err = ota_read_artifact_string(artifact,
                                   "manifest",
                                   manifest_path,
                                   sizeof(manifest_path));
    if (err != ESP_OK) {
        return err;
    }

    err = ota_read_artifact_string(artifact,
                                   "sha256",
                                   result->image_sha256,
                                   sizeof(result->image_sha256));
    if (err != ESP_OK) {
        return err;
    }
    if (!solar_os_crypto_sha256_hex_is_valid(result->image_sha256)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = solar_os_json_get_path_uint32(artifact, "size", &image_size);
    if (err != ESP_OK) {
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : err;
    }
    result->image_size = image_size;
    result->image_size_known = image_size > 0;

    err = ota_join_url(base_url,
                       firmware_path,
                       result->firmware_url,
                       sizeof(result->firmware_url));
    if (err != ESP_OK) {
        return err;
    }

    return ota_join_url(base_url,
                        manifest_path,
                        result->manifest_url,
                        sizeof(result->manifest_url));
}

static esp_err_t ota_find_artifact_in_index(const solar_os_json_value_t *root,
                                            const char *base_url,
                                            solar_os_ota_check_result_t *result)
{
    if (root == NULL || base_url == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_json_value_t *artifacts = solar_os_json_path_get(root, "artifacts");
    if (!solar_os_json_is_array(artifacts)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t count = solar_os_json_array_size(artifacts);
    for (size_t i = 0; i < count; i++) {
        const solar_os_json_value_t *artifact = solar_os_json_array_get(artifacts, i);
        if (!solar_os_json_is_object(artifact)) {
            continue;
        }

        char board_id[SOLAR_OS_OTA_BOARD_MAX];
        char flavor[SOLAR_OS_OTA_FLAVOR_MAX];
        if (solar_os_json_get_path_string(artifact, "board_id", board_id, sizeof(board_id)) !=
                ESP_OK ||
            solar_os_json_get_path_string(artifact, "flavor", flavor, sizeof(flavor)) != ESP_OK) {
            continue;
        }

        if (strcmp(board_id, SOLAR_OS_BOARD_ID) != 0 ||
            strcmp(flavor, result->target_flavor) != 0) {
            continue;
        }

        strlcpy(result->board_id, board_id, sizeof(result->board_id));
        return ota_fill_artifact_from_json(artifact, base_url, result);
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ota_parse_release_index(char *index_body,
                                         size_t index_len,
                                         solar_os_ota_check_result_t *result)
{
    if (index_body == NULL || index_len == 0 || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_json_doc_t *doc = NULL;
    size_t parse_error = 0;
    esp_err_t err = solar_os_json_parse_ex(index_body, index_len, &doc, &parse_error);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "index JSON parse failed at %u", (unsigned)parse_error);
        return err;
    }

    const solar_os_json_value_t *root = solar_os_json_root(doc);
    char schema[40];
    err = solar_os_json_get_path_string(root, "schema", schema, sizeof(schema));
    if (err != ESP_OK || strcmp(schema, "solaros.release_index") != 0) {
        solar_os_json_free(doc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char project[16];
    err = solar_os_json_get_path_string(root, "project", project, sizeof(project));
    if (err != ESP_OK || strcmp(project, "SolarOS") != 0) {
        solar_os_json_free(doc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char base_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    err = ota_index_base_url(root, result->index_url, base_url, sizeof(base_url));
    if (err == ESP_OK) {
        err = ota_find_artifact_in_index(root, base_url, result);
    }

    solar_os_json_free(doc);
    return err;
}

static esp_err_t ota_resolve_artifact(solar_os_ota_check_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    strlcpy(result->compiled_flavor, SOLAR_OS_FLAVOR_NAME, sizeof(result->compiled_flavor));
    solar_os_ota_get_flavor(result->target_flavor, sizeof(result->target_flavor));
    strlcpy(result->current_version, SOLAR_OS_VERSION, sizeof(result->current_version));
    strlcpy(result->board_id, SOLAR_OS_BOARD_ID, sizeof(result->board_id));
    result->status_code = -1;
    result->content_length = -1;

    esp_err_t err = ota_build_index_url(result->index_url, sizeof(result->index_url));
    if (err != ESP_OK) {
        return err;
    }

    char *index_body = NULL;
    size_t index_len = 0;
    err = ota_http_get_text_alloc(result->index_url,
                                  OTA_INDEX_MAX,
                                  &index_body,
                                  &index_len,
                                  &result->status_code,
                                  &result->content_length);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_build_index_signature_url(result->index_url,
                                        result->index_sig_url,
                                        sizeof(result->index_sig_url));
    if (err != ESP_OK) {
        ota_free(index_body);
        return err;
    }

    char *signature_body = NULL;
    size_t signature_len = 0;
    err = ota_http_get_text_alloc(result->index_sig_url,
                                  OTA_INDEX_SIGNATURE_MAX,
                                  &signature_body,
                                  &signature_len,
                                  NULL,
                                  NULL);
    if (err == ESP_OK) {
        err = ota_verify_index_signature(index_body,
                                         index_len,
                                         signature_body,
                                         signature_len);
    }
    ota_free(signature_body);
    if (err != ESP_OK) {
        ota_free(index_body);
        return err;
    }
    result->index_signature_verified = true;

    err = ota_parse_release_index(index_body, index_len, result);
    ota_free(index_body);
    if (err != ESP_OK) {
        return err;
    }

    result->update_available =
        strcmp(result->available_version, SOLAR_OS_VERSION) != 0 ||
        strcmp(result->target_flavor, SOLAR_OS_FLAVOR_NAME) != 0;

    SOLAR_OS_LOGI(TAG,
                  "check board=%s current=%s/%s target=%s available=%s update=%s",
                  result->board_id,
                  SOLAR_OS_VERSION,
                  SOLAR_OS_FLAVOR_NAME,
                  result->target_flavor,
                  result->available_version,
                  result->update_available ? "yes" : "no");
    return ESP_OK;
}

static int ota_partition_slot(const esp_partition_t *partition)
{
    if (partition == NULL ||
        partition->type != ESP_PARTITION_TYPE_APP ||
        partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
        partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return -1;
    }

    return (int)partition->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_0;
}

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

static void ota_fill_partition(solar_os_ota_partition_t *info, const esp_partition_t *partition)
{
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->slot = -1;
    strlcpy(info->state, "unknown", sizeof(info->state));

    if (partition == NULL) {
        return;
    }

    info->valid = true;
    info->slot = ota_partition_slot(partition);
    info->subtype = (uint8_t)partition->subtype;
    info->address = partition->address;
    info->size = partition->size;
    strlcpy(info->label, partition->label, sizeof(info->label));

    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(partition, &app_desc) == ESP_OK) {
        strlcpy(info->version, app_desc.version, sizeof(info->version));
        strlcpy(info->project_name, app_desc.project_name, sizeof(info->project_name));
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) == ESP_OK) {
        strlcpy(info->state, ota_state_name(state), sizeof(info->state));
    }
}

static const esp_partition_t *ota_find_slot(uint8_t slot)
{
    if (slot >= (uint8_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MAX -
                          ESP_PARTITION_SUBTYPE_APP_OTA_0)) {
        return NULL;
    }

    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot,
                                    NULL);
}

esp_err_t solar_os_ota_init(void)
{
    ota_load();
    return ESP_OK;
}

void solar_os_ota_get_url(char *url, size_t len)
{
    if (url == NULL || len == 0) {
        return;
    }

    ota_load();
    strlcpy(url, ota_url, len);
}

esp_err_t solar_os_ota_set_url(const char *url)
{
    ota_load();
    if (!ota_url_is_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(ota_url, url, sizeof(ota_url));
    return ota_save();
}

void solar_os_ota_get_flavor(char *flavor, size_t len)
{
    if (flavor == NULL || len == 0) {
        return;
    }

    ota_load();
    strlcpy(flavor, ota_target_flavor, len);
}

esp_err_t solar_os_ota_set_flavor(const char *flavor)
{
    ota_load();
    if (!ota_flavor_is_valid(flavor)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(ota_target_flavor, flavor, sizeof(ota_target_flavor));
    return ota_save();
}

esp_err_t solar_os_ota_get_index_url(char *index_url, size_t index_url_len)
{
    return ota_build_index_url(index_url, index_url_len);
}

esp_err_t solar_os_ota_get_status(solar_os_ota_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    solar_os_ota_get_url(status->url, sizeof(status->url));
    strlcpy(status->compiled_flavor, SOLAR_OS_FLAVOR_NAME, sizeof(status->compiled_flavor));
    solar_os_ota_get_flavor(status->target_flavor, sizeof(status->target_flavor));
    status->ota_partition_count = esp_ota_get_app_partition_count();

    ota_fill_partition(&status->running, esp_ota_get_running_partition());
    ota_fill_partition(&status->boot, esp_ota_get_boot_partition());
    ota_fill_partition(&status->next_update, esp_ota_get_next_update_partition(NULL));
    return ESP_OK;
}

esp_err_t solar_os_ota_set_boot_slot(uint8_t slot)
{
    const esp_partition_t *partition = ota_find_slot(slot);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    return esp_ota_set_boot_partition(partition);
}

esp_err_t solar_os_ota_check(solar_os_ota_check_result_t *result)
{
    return ota_resolve_artifact(result);
}

static void ota_report_progress(solar_os_ota_progress_cb_t cb,
                                void *user,
                                const solar_os_ota_progress_t *progress)
{
    if (cb != NULL && progress != NULL) {
        cb(progress, user);
    }
}

esp_err_t solar_os_ota_upgrade(solar_os_ota_progress_cb_t progress, void *user)
{
    solar_os_ota_check_result_t *artifact = ota_malloc(sizeof(*artifact));
    solar_os_ota_progress_t *event = ota_malloc(sizeof(*event));
    ota_firmware_verify_t *verify = ota_malloc(sizeof(*verify));
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = ESP_OK;
    bool verify_initialized = false;

    if (artifact == NULL || event == NULL || verify == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    memset(artifact, 0, sizeof(*artifact));
    memset(event, 0, sizeof(*event));
    memset(verify, 0, sizeof(*verify));

    err = ota_resolve_artifact(artifact);
    if (err != ESP_OK) {
        goto cleanup;
    }

    event->stage = SOLAR_OS_OTA_PROGRESS_CONNECTING;
    event->status_code = -1;
    event->image_size = artifact->image_size;
    event->image_size_known = artifact->image_size_known;
    strlcpy(event->firmware_url, artifact->firmware_url, sizeof(event->firmware_url));
    strlcpy(event->version, artifact->available_version, sizeof(event->version));
    ota_report_progress(progress, user, event);

    solar_os_crypto_sha256_init(&verify->sha256);
    verify_initialized = true;
    err = solar_os_crypto_sha256_start(&verify->sha256);
    if (err != ESP_OK) {
        goto cleanup;
    }

    SOLAR_OS_LOGI(TAG, "upgrade %s", artifact->firmware_url);
    esp_http_client_config_t http_config = {
        .url = artifact->firmware_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .event_handler = ota_firmware_http_event,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .user_agent = "SolarOS-ota/0.1",
        .user_data = verify,
        .keep_alive_enable = true,
        .max_redirection_count = 3,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade begin failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    event->status_code = esp_https_ota_get_status_code(handle);
    int image_size = esp_https_ota_get_image_size(handle);
    if (image_size > 0) {
        event->image_size = (uint32_t)image_size;
        event->image_size_known = true;
    }

    esp_app_desc_t app_desc;
    if (esp_https_ota_get_img_desc(handle, &app_desc) == ESP_OK) {
        strlcpy(event->version, app_desc.version, sizeof(event->version));
        event->stage = SOLAR_OS_OTA_PROGRESS_IMAGE;
        const int desc_bytes_read = esp_https_ota_get_image_len_read(handle);
        event->bytes_read = desc_bytes_read > 0 ? (uint32_t)desc_bytes_read : 0;
        ota_report_progress(progress, user, event);
        SOLAR_OS_LOGI(TAG, "image version=%s project=%s size=%" PRIu32,
                 app_desc.version,
                 app_desc.project_name,
                 event->image_size);
    }

    do {
        err = esp_https_ota_perform(handle);
        const int bytes_read = esp_https_ota_get_image_len_read(handle);
        image_size = esp_https_ota_get_image_size(handle);
        event->stage = SOLAR_OS_OTA_PROGRESS_WRITING;
        event->bytes_read = bytes_read > 0 ? (uint32_t)bytes_read : 0;
        if (image_size > 0) {
            event->image_size = (uint32_t)image_size;
            event->image_size_known = true;
        }
        ota_report_progress(progress, user, event);
        vTaskDelay(1);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade perform failed: %s", esp_err_to_name(err));
        (void)esp_https_ota_abort(handle);
        handle = NULL;
        goto cleanup;
    }

    event->stage = SOLAR_OS_OTA_PROGRESS_VERIFYING;
    ota_report_progress(progress, user, event);

    if (!esp_https_ota_is_complete_data_received(handle)) {
        SOLAR_OS_LOGW(TAG, "upgrade incomplete");
        (void)esp_https_ota_abort(handle);
        handle = NULL;
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (verify->hash_failed) {
        SOLAR_OS_LOGW(TAG, "upgrade hash failed");
        (void)esp_https_ota_abort(handle);
        handle = NULL;
        err = ESP_FAIL;
        goto cleanup;
    }

    uint8_t image_digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    err = solar_os_crypto_sha256_finish(&verify->sha256, image_digest);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade hash finish failed: %s", esp_err_to_name(err));
        (void)esp_https_ota_abort(handle);
        handle = NULL;
        goto cleanup;
    }

    char image_digest_hex[SOLAR_OS_CRYPTO_SHA256_HEX_LEN];
    err = solar_os_crypto_bytes_to_hex(image_digest,
                                       sizeof(image_digest),
                                       image_digest_hex,
                                       sizeof(image_digest_hex));
    if (err != ESP_OK) {
        (void)esp_https_ota_abort(handle);
        handle = NULL;
        goto cleanup;
    }
    if (!solar_os_crypto_sha256_matches_hex(image_digest, artifact->image_sha256)) {
        SOLAR_OS_LOGW(TAG,
                      "upgrade hash mismatch expected=%s actual=%s",
                      artifact->image_sha256,
                      image_digest_hex);
        (void)esp_https_ota_abort(handle);
        handle = NULL;
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }
    SOLAR_OS_LOGI(TAG,
                  "upgrade hash verified sha256=%s bytes=%" PRIu32,
                  image_digest_hex,
                  verify->bytes_hashed);

    err = esp_https_ota_finish(handle);
    handle = NULL;
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade finish failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    event->stage = SOLAR_OS_OTA_PROGRESS_DONE;
    ota_report_progress(progress, user, event);
    SOLAR_OS_LOGI(TAG, "upgrade complete");
    err = ESP_OK;

cleanup:
    if (handle != NULL) {
        (void)esp_https_ota_abort(handle);
    }
    if (verify_initialized) {
        solar_os_crypto_sha256_free(&verify->sha256);
    }
    ota_free(verify);
    ota_free(event);
    ota_free(artifact);
    return err;
}
