#include "solar_os_zoo.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_config.h"
#include "solar_os.h"
#include "solar_os_crypto.h"
#include "solar_os_http_client.h"
#include "solar_os_inference_types_v1.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_zip.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif
#define ZOO_FILE_MAX (64U * 1024U * 1024U)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("serialize zoo storage transactions across app sessions")
static portMUX_TYPE transaction_lock = portMUX_INITIALIZER_UNLOCKED;
static bool transaction_busy;
static bool transaction_begin(void)
{ portENTER_CRITICAL(&transaction_lock); bool available = !transaction_busy; if (available) transaction_busy = true; portEXIT_CRITICAL(&transaction_lock); return available; }
static void transaction_end(void)
{ portENTER_CRITICAL(&transaction_lock); transaction_busy = false; portEXIT_CRITICAL(&transaction_lock); }
struct solar_os_zoo {
    char source[SOLAR_OS_ZOO_SOURCE_MAX], root[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_json_doc_t *doc;
};
static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static const char *str(const cJSON *o) { return cJSON_IsString(o) ? o->valuestring : NULL; }
static bool eq(const cJSON *o, const char *s) { return str(o) && !strcmp(str(o), s); }
static bool number(const cJSON *o, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(o) || !isfinite(o->valuedouble) || o->valuedouble < 0 ||
        o->valuedouble > max || floor(o->valuedouble) != o->valuedouble) return false;
    *out = o->valuedouble; return true;
}
static bool identity(const char *s)
{
    if (!s || !*s || strlen(s) > 100 || !isalnum((unsigned char)*s)) return false;
    if (s[strlen(s) - 1] == '.') return false;
    for (; *s; ++s) if (!isalnum((unsigned char)*s) && !strchr("._-", *s)) return false;
    return true;
}
static bool relative(const char *s)
{
    if (!s || !*s || *s == '/' || strlen(s) >= SOLAR_OS_STORAGE_PATH_MAX) return false;
    const char *start = s;
    for (;; ++s) {
        if (*s && ((unsigned char)*s < 32 || strchr("\\:<>\"|?*", *s))) return false;
        if (!*s || *s == '/') {
            size_t n = s - start;
            if (!n || (n == 1 && *start == '.') || (n == 2 && !strncmp(start, "..", 2)) ||
                s[-1] == '.' || s[-1] == ' ') return false;
            if (!*s) break;
            start = s + 1;
        }
    }
    return true;
}
static bool hash(const cJSON *o) { return solar_os_crypto_sha256_hex_is_valid(str(o)); }
static bool plain_string(const cJSON *o, size_t max)
{
    const char *s = str(o);
    if (!s || !*s || strlen(s) > max) return false;
    for (; *s; ++s) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return false;
    return true;
}
static bool tags(const cJSON *array)
{
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) < 1 || cJSON_GetArraySize(array) > 32) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) if (!plain_string(item, 64)) return false;
    return true;
}
static bool unique(const cJSON *o, unsigned depth)
{
    if (depth > 32) return false;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, o) {
        if (cJSON_IsObject(o)) for (b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
        if (!unique(a, depth + 1)) return false;
    }
    return true;
}
static esp_err_t parse(const uint8_t *data, size_t size, solar_os_json_doc_t **out)
{
    *out = NULL;
    if (!size || memchr(data, 0, size)) return ESP_ERR_INVALID_ARG;
    /* Bound nesting before invoking cJSON; reject strings it cannot represent. */
    unsigned depth = 0; bool quote = false, escape = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (quote) {
            if (c < 32) return ESP_ERR_INVALID_ARG;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') quote = false;
        } else if (c == '"') quote = true;
        else if (c == '{' || c == '[') { if (++depth > 32) return ESP_ERR_INVALID_SIZE; }
        else if (c == '}' || c == ']') { if (!depth) return ESP_ERR_INVALID_ARG; --depth; }
        if (i + 6 <= size && !memcmp(data + i, "\\u0000", 6)) return ESP_ERR_INVALID_ARG;
    }
    if (quote || depth) return ESP_ERR_INVALID_ARG;
    const char *end = NULL;
    solar_os_json_init();
    cJSON *checked = cJSON_ParseWithLengthOpts((const char *)data, size, &end, false);
    if (!checked) return ESP_ERR_INVALID_ARG;
    while (end < (const char *)data + size && isspace((unsigned char)*end)) ++end;
    bool ok = end == (const char *)data + size && unique(checked, 0);
    cJSON_Delete(checked);
    return ok ? solar_os_json_parse((const char *)data, size, out) : ESP_ERR_INVALID_ARG;
}
static const cJSON *entry(const solar_os_zoo_t *z, size_t index)
{ return cJSON_GetArrayItem(get(solar_os_json_root(z->doc), "models"), index); }
static const cJSON *metadata(const cJSON *e) { return get(e, "metadata"); }
static bool catalog_valid(solar_os_json_doc_t *doc)
{
    const cJSON *root = solar_os_json_root(doc), *models = get(root, "models");
    uint32_t schema, bytes;
    int count = cJSON_GetArraySize(models);
    if (!number(get(root, "schema"), 1, &schema) || schema != 1 || !cJSON_IsArray(models) ||
        count < 1 || (unsigned)count > SOLAR_OS_ZOO_MODELS_MAX) return false;
    const cJSON *e, *prior;
    cJSON_ArrayForEach(e, models) {
        const cJSON *m = metadata(e), *a = get(e, "artifact");
        const char *id = str(get(m, "id")), *version = str(get(m, "version"));
        const char *path = str(get(a, "file"));
        char expected[384];
        if (!identity(id) || !identity(version) || !plain_string(get(m, "name"), 120) ||
            !plain_string(get(m, "summary"), 1024) || !tags(get(m, "tasks")) || !tags(get(m, "modalities")) ||
            !hash(get(a, "sha256")) || !hash(get(e, "bundle_sha256")) ||
            !number(get(a, "bytes"), ZOO_FILE_MAX, &bytes) || !bytes ||
            !number(get(a, "unpacked_bytes"), ZOO_FILE_MAX, &bytes) || !bytes ||
            !cJSON_IsObject(get(e, "runtime")) || !cJSON_IsObject(get(e, "inputs")) ||
            !cJSON_IsObject(get(e, "outputs")) || !cJSON_IsObject(get(m, "requirements")) ||
            !cJSON_IsArray(get(e, "packages")) || !cJSON_IsBool(get(e, "psram")) ||
            !plain_string(get(get(e, "license"), "id"), 63) ||
            !plain_string(get(get(e, "result"), "type"), 31) ||
            !(eq(get(e, "status"), "declared") || eq(get(e, "status"), "hardware-validated"))) return false;
        snprintf(expected, sizeof(expected), "models/%s/%s/%s.zip", id, version, str(get(a, "sha256")));
        if (!path || strcmp(path, expected)) return false;
        const cJSON *requirements = get(m, "requirements");
        if ((get(requirements, "min_free_internal_bytes") && !number(get(requirements, "min_free_internal_bytes"), UINT32_MAX, &bytes)) ||
            (get(requirements, "min_free_psram_bytes") && !number(get(requirements, "min_free_psram_bytes"), UINT32_MAX, &bytes))) return false;
        for (prior = models->child; prior != e; prior = prior->next)
            if (!strcasecmp(str(get(metadata(prior), "id")), id) &&
                !strcasecmp(str(get(metadata(prior), "version")), version)) return false;
    }
    return true;
}
static bool cancelled(const volatile bool *cancel) { return cancel && *cancel; }
static void report(solar_os_zoo_progress_fn fn, void *user, const char *stage, uint32_t bytes, uint32_t total)
{ if (fn) { solar_os_zoo_progress_t p = {.bytes = bytes, .total = total}; strlcpy(p.stage, stage, sizeof(p.stage)); fn(&p, user); } }
static esp_err_t remove_tree(const char *path)
{
    struct stat st;
#ifdef ESP_PLATFORM
    if (stat(path, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
#else
    if (lstat(path, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
#endif
    if (!S_ISDIR(st.st_mode)) return unlink(path) ? ESP_FAIL : ESP_OK;
    DIR *dir = opendir(path); if (!dir) return ESP_FAIL;
    esp_err_t error = ESP_OK; struct dirent *item;
    while ((item = readdir(dir)) && !error) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        error = solar_os_storage_join_path(path, item->d_name, child, sizeof(child));
        if (!error) error = remove_tree(child);
    }
    closedir(dir); return error ? error : rmdir(path) ? ESP_FAIL : ESP_OK;
}
static esp_err_t read_doc(const char *path, size_t max, solar_os_json_doc_t **doc)
{
    FILE *file = fopen(path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
    uint8_t *data = solar_os_memory_alloc(max + 1, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "zoo.json");
    if (!data) { fclose(file); return ESP_ERR_NO_MEM; }
    size_t size = fread(data, 1, max + 1, file);
    bool io_error = ferror(file);
    esp_err_t error = size > max || io_error ? ESP_ERR_INVALID_SIZE : parse(data, size, doc);
    fclose(file); solar_os_memory_free(data); return error;
}
static esp_err_t write_cache(const char *path, const char *data)
{
    size_t size = strlen(data); if (size > SOLAR_OS_ZOO_CATALOG_MAX) return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(path, "wb"); if (!file) return ESP_FAIL;
    esp_err_t error = fwrite(data, 1, size, file) == size ? solar_os_storage_sync_file(file) : ESP_FAIL;
    if (fclose(file) && !error) error = ESP_FAIL;
    return error;
}
static esp_err_t hash_file(const char *path, const char *expected, const volatile bool *cancel)
{
    FILE *file = fopen(path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
    uint8_t *buffer = solar_os_memory_alloc(8192, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "zoo.hash");
    solar_os_crypto_sha256_t sha; solar_os_crypto_sha256_init(&sha);
    esp_err_t error = buffer ? solar_os_crypto_sha256_start(&sha) : ESP_ERR_NO_MEM;
    while (!error) {
        if (cancelled(cancel)) { error = ESP_ERR_TIMEOUT; break; }
        size_t n = fread(buffer, 1, 8192, file);
        if (n) error = solar_os_crypto_sha256_update(&sha, buffer, n);
        if (n < 8192) { if (ferror(file)) error = ESP_FAIL; break; }
        vTaskDelay(1);
    }
    if (!error) { uint8_t digest[32]; error = solar_os_crypto_sha256_finish(&sha, digest);
        if (!error && !solar_os_crypto_sha256_matches_hex(digest, expected)) error = ESP_ERR_INVALID_CRC; }
    solar_os_crypto_sha256_free(&sha); solar_os_memory_free(buffer); fclose(file); return error;
}
#define ZOO_DOWNLOAD_CHUNK 4096U
typedef struct { FILE *file; uint8_t *buffer; size_t pending; solar_os_crypto_sha256_t sha; uint32_t bytes, max; solar_os_zoo_progress_fn fn; void *user; const char *stage; } download_t;
static esp_err_t download_event(const solar_os_http_event_t *event, void *user)
{
    download_t *d = user;
    /* ESP-IDF emits headers while status_code is still -1. Only the completed
     * response and body events carry the final status. */
    if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) return ESP_OK;
    if (event->status_code != 200) return ESP_ERR_INVALID_RESPONSE;
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA) return ESP_OK;
    if (event->data_len > d->max - d->bytes) return ESP_ERR_INVALID_SIZE;
    esp_err_t hashed = solar_os_crypto_sha256_update(&d->sha, event->data, event->data_len);
    if (hashed) return hashed;
    /* Batch borrowed transport chunks in transient storage. Full-block writes
     * keep memory bounded and avoid I/O for each small network fragment. */
    const uint8_t *source = event->data;
    size_t remaining = event->data_len;
    while (remaining) {
        size_t room = ZOO_DOWNLOAD_CHUNK - d->pending;
        size_t count = remaining > room ? room : remaining;
        memcpy(d->buffer + d->pending, source, count);
        d->pending += count;
        if (d->pending == ZOO_DOWNLOAD_CHUNK) {
            if (fwrite(d->buffer, 1, d->pending, d->file) != d->pending) return ESP_FAIL;
            d->pending = 0;
        }
        source += count; remaining -= count;
    }
    d->bytes += event->data_len; report(d->fn, d->user, d->stage, d->bytes, d->max); return ESP_OK;
}
static esp_err_t download(const char *url, const char *path, uint32_t max, const volatile bool *cancel,
    solar_os_zoo_progress_fn fn, void *user, const char *stage)
{
    FILE *file = fopen(path, "wb"); if (!file) return ESP_FAIL;
    /* Stream writes directly from the staging allocation. A stdio buffer
     * would reintroduce a separate, allocator-selected transfer buffer. */
    if (setvbuf(file, NULL, _IONBF, 0)) { fclose(file); unlink(path); return ESP_FAIL; }
    download_t data = {.file = file, .max = max, .fn = fn, .user = user, .stage = stage};
    data.buffer = solar_os_memory_alloc(ZOO_DOWNLOAD_CHUNK, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "zoo.download");
    solar_os_http_request_options_t options = {.url = url, .method = SOLAR_OS_HTTP_METHOD_GET,
        .user_agent = "SolarOS-zoo/" SOLAR_OS_VERSION, .follow_redirects = false, .timeout_ms = 10000,
        .read_poll_ms = 200, .cancel_flag = cancel, .event_handler = download_event, .user_data = &data};
    solar_os_http_request_t *request = NULL; solar_os_http_response_t response = {0};
    solar_os_crypto_sha256_init(&data.sha);
    esp_err_t error = data.buffer ? solar_os_crypto_sha256_start(&data.sha) : ESP_ERR_NO_MEM;
    if (!error) error = solar_os_http_request_create(&options, &request);
    if (!error) error = solar_os_http_request_perform(request, &response);
    if (!error && response.status_code != 200) error = ESP_ERR_INVALID_RESPONSE;
    if (!error && cancelled(cancel)) error = ESP_ERR_TIMEOUT;
    if (!error && data.pending && fwrite(data.buffer, 1, data.pending, file) != data.pending) error = ESP_FAIL;
    if (!error) error = solar_os_storage_sync_file(file);
    if (fclose(file) && !error) error = ESP_FAIL;
    if (request) solar_os_http_request_destroy(request);
    char hex[65];
    if (!error) { uint8_t digest[32];
        error = solar_os_crypto_sha256_finish(&data.sha, digest);
        if (!error) for (size_t i = 0; i < 32; ++i) snprintf(hex + i*2, 3, "%02x", digest[i]); }
    solar_os_crypto_sha256_free(&data.sha);
    /* Verify stored bytes against the received body before accepting a cache
     * or archive, including when the filesystem reports successful writes. */
    if (!error) { report(fn, user, "verify download", 0, 0); error = hash_file(path, hex, cancel); }
    solar_os_memory_free(data.buffer);
    if (error) unlink(path);
    return error;
}
static void setting(const char *key, char *value, size_t size, const char *fallback)
{
    strlcpy(value, fallback, size); nvs_handle_t nvs;
    if (nvs_open("zoo", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t length = size; if (nvs_get_str(nvs, key, value, &length) != ESP_OK) strlcpy(value, fallback, size);
    nvs_close(nvs);
}
static esp_err_t set_setting(const char *key, const char *value)
{
    nvs_handle_t nvs; esp_err_t error = nvs_open("zoo", NVS_READWRITE, &nvs);
    if (!error) { error = nvs_set_str(nvs, key, value); if (!error) error = nvs_commit(nvs); nvs_close(nvs); }
    return error;
}
static bool url_valid(const char *url)
{
    if (!url || strncmp(url, "https://", 8) || strlen(url) >= SOLAR_OS_ZOO_SOURCE_MAX ||
        !strchr(url + 8, '/') || url[8] == '/' || strpbrk(url, "?#@\\ \t\r\n")) return false;
    for (const char *s = url; *s; ++s) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return false;
    return true;
}
esp_err_t solar_os_zoo_set_source(const char *url)
{ return url_valid(url) ? set_setting("source", url) : ESP_ERR_INVALID_ARG; }
void solar_os_zoo_get_source(char *buffer, size_t size)
{ setting("source", buffer, size, SOLAR_OS_ZOO_DEFAULT_SOURCE); }
esp_err_t solar_os_zoo_set_storage(const char *target)
{ return target && (!strcmp(target, "sd") || !strcmp(target, "flash")) ? set_setting("storage", target) : ESP_ERR_INVALID_ARG; }
const char *solar_os_zoo_storage(void)
{ char value[12]; setting("storage", value, sizeof(value), "auto"); return !strcmp(value, "sd") ? "sd" : !strcmp(value, "flash") ? "flash" : "auto"; }
esp_err_t solar_os_zoo_open(solar_os_zoo_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    solar_os_zoo_t *z = solar_os_memory_calloc(1, sizeof(*z), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "zoo");
    if (!z) return ESP_ERR_NO_MEM;
    solar_os_zoo_get_source(z->source, sizeof(z->source));
    const char *target = solar_os_zoo_storage(), *mount = NULL;
    if (!strcmp(target, "sd")) { if (solar_os_storage_sd_is_mounted()) mount = solar_os_storage_sd_mount_point(); }
    else if (!strcmp(target, "flash")) { if (solar_os_storage_flash_is_mounted()) mount = solar_os_storage_flash_mount_point(); }
    else if (solar_os_storage_sd_is_mounted()) mount = solar_os_storage_sd_mount_point();
    else if (solar_os_storage_flash_is_mounted()) mount = solar_os_storage_flash_mount_point();
    esp_err_t error = mount ? solar_os_storage_join_path(mount, "dl/zoo", z->root, sizeof(z->root)) : ESP_ERR_INVALID_STATE;
    if (!error) error = solar_os_storage_makedirs(z->root, true);
    if (error) { solar_os_zoo_close(z); return error; }
    *out = z; return ESP_OK;
}
void solar_os_zoo_close(solar_os_zoo_t *z) { if (z) { solar_os_json_free(z->doc); solar_os_memory_free(z); } }
const char *solar_os_zoo_source(const solar_os_zoo_t *z) { return z->source; }
const char *solar_os_zoo_root(const solar_os_zoo_t *z) { return z->root; }
static esp_err_t cache_path(const solar_os_zoo_t *z, const char *name, char path[SOLAR_OS_STORAGE_PATH_MAX])
{ return solar_os_storage_join_path(z->root, name, path, SOLAR_OS_STORAGE_PATH_MAX); }
static esp_err_t reload(solar_os_zoo_t *z)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX]; solar_os_json_doc_t *doc = NULL;
    esp_err_t error = cache_path(z, "catalog.json", path);
    if (!error) error = cache_path(z, ".catalog.old", backup);
    struct stat st;
    if (!error && stat(path, &st) && errno == ENOENT && !stat(backup, &st) && rename(backup, path)) error = ESP_FAIL;
    if (!error) error = read_doc(path, SOLAR_OS_ZOO_CATALOG_MAX, &doc);
    /* Cache is wrapped with its source so changing repositories cannot use stale URLs. */
    if (!error && !eq(get(solar_os_json_root(doc), "source"), z->source)) error = ESP_ERR_INVALID_STATE;
    const cJSON *catalog = doc ? get(solar_os_json_root(doc), "catalog") : NULL;
    solar_os_json_doc_t *parsed = NULL;
    if (!error) { char *raw = cJSON_PrintUnformatted(catalog);
        error = raw ? parse((uint8_t *)raw, strlen(raw), &parsed) : ESP_ERR_NO_MEM; cJSON_free(raw); }
    if (!error && !catalog_valid(parsed)) error = ESP_ERR_INVALID_RESPONSE;
    solar_os_json_free(doc);
    if (error) solar_os_json_free(parsed);
    else { solar_os_json_free(z->doc); z->doc = parsed; }
    return error;
}
static esp_err_t refresh(solar_os_zoo_t *z, const volatile bool *cancel, solar_os_zoo_progress_fn fn, void *user)
{
    char temp[SOLAR_OS_STORAGE_PATH_MAX], path[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (cache_path(z, ".catalog.new", temp) || cache_path(z, "catalog.json", path) || cache_path(z, ".catalog.old", backup)) return ESP_ERR_INVALID_SIZE;
    esp_err_t error = download(z->source, temp, SOLAR_OS_ZOO_CATALOG_MAX - 1024, cancel, fn, user, "catalog");
    solar_os_json_doc_t *doc = NULL;
    if (!error) error = read_doc(temp, SOLAR_OS_ZOO_CATALOG_MAX, &doc);
    if (!error && !catalog_valid(doc)) error = ESP_ERR_INVALID_RESPONSE;
    if (!error && cancelled(cancel)) error = ESP_ERR_TIMEOUT;
    if (!error) {
        cJSON *wrapped = cJSON_CreateObject();
        bool ok = wrapped && cJSON_AddStringToObject(wrapped, "source", z->source) &&
            cJSON_AddItemToObject(wrapped, "catalog", cJSON_Duplicate(solar_os_json_root(doc), true));
        char *raw = ok ? cJSON_PrintUnformatted(wrapped) : NULL;
        error = raw ? write_cache(temp, raw) : ESP_ERR_NO_MEM;
        cJSON_free(raw); cJSON_Delete(wrapped);
        if (!error) error = solar_os_storage_replace_file(temp, path, backup);
    }
    unlink(temp);
    if (error) solar_os_json_free(doc);
    else { solar_os_json_free(z->doc); z->doc = doc; }
    return error;
}
size_t solar_os_zoo_count(const solar_os_zoo_t *z) { return z && z->doc ? cJSON_GetArraySize(get(solar_os_json_root(z->doc), "models")) : 0; }
static void join_tags(const cJSON *array, char *out, size_t size)
{
    out[0] = 0; const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (!plain_string(item, 64)) continue;
        size_t n = strlen(out); snprintf(out + n, size - n, "%s%s", n ? ", " : "", str(item));
    }
}
static bool version_minimum(const char *minimum)
{
    unsigned a, b, c, x, y, v; char tail;
    return minimum && sscanf(minimum, "%u.%u.%u%c", &a, &b, &c, &tail) == 3 &&
        sscanf(SOLAR_OS_VERSION, "%u.%u.%u", &x, &y, &v) == 3 &&
        (x > a || (x == a && (y > b || (y == b && v >= c))));
}
bool solar_os_zoo_get(const solar_os_zoo_t *z, size_t index, solar_os_zoo_model_t *model)
{
    if (!z || !model || index >= solar_os_zoo_count(z)) return false;
    const cJSON *e = entry(z, index), *m = metadata(e), *r = get(e, "runtime"), *a = get(e, "artifact"), *requirements = get(m, "requirements");
    memset(model, 0, sizeof(*model));
    strlcpy(model->id, str(get(m, "id")), sizeof(model->id)); strlcpy(model->version, str(get(m, "version")), sizeof(model->version));
    strlcpy(model->name, str(get(m, "name")), sizeof(model->name)); strlcpy(model->summary, str(get(m, "summary")), sizeof(model->summary));
    strlcpy(model->result, str(get(get(e, "result"), "type")), sizeof(model->result));
    strlcpy(model->license, str(get(get(e, "license"), "id")), sizeof(model->license));
    strlcpy(model->validation, str(get(e, "status")), sizeof(model->validation));
    join_tags(get(m, "tasks"), model->tasks, sizeof(model->tasks)); join_tags(get(m, "modalities"), model->modalities, sizeof(model->modalities));
    number(get(a, "bytes"), ZOO_FILE_MAX, &model->archive_bytes); number(get(a, "unpacked_bytes"), ZOO_FILE_MAX, &model->unpacked_bytes);
    model->compatible = eq(get(r, "backend"), "espdl") && eq(get(r, "version"), SOLAR_OS_INFERENCE_BACKEND_VERSION) && eq(get(r, "target"), "esp32s3");
    if (!model->compatible) strlcpy(model->reason, "runtime or target mismatch", sizeof(model->reason));
    const cJSON *package;
    cJSON_ArrayForEach(package, get(e, "packages")) {
        bool available = eq(package, "service_inference");
#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
        available |= eq(package, "service_image");
#endif
        if (!available) { model->compatible = false; strlcpy(model->reason, "required package is unavailable", sizeof(model->reason)); }
    }
    if (get(requirements, "min_solaros_version") && !version_minimum(str(get(requirements, "min_solaros_version")))) {
        model->compatible = false; strlcpy(model->reason, "newer SolarOS required", sizeof(model->reason));
    }
    model->memory_known = number(get(requirements, "min_free_internal_bytes"), UINT32_MAX, &model->min_internal_bytes) &&
        number(get(requirements, "min_free_psram_bytes"), UINT32_MAX, &model->min_psram_bytes);
    if (model->compatible) strlcpy(model->reason, model->memory_known ? "memory requirements declared" : "memory requirements unmeasured", sizeof(model->reason));
    if (strlen(z->root) + strlen(model->id) + strlen(model->version) + 26 >= SOLAR_OS_STORAGE_PATH_MAX) {
        model->compatible = false; strlcpy(model->reason, "installation path exceeds device limit", sizeof(model->reason));
    }
    return true;
}
static esp_err_t model_path(const solar_os_zoo_t *z, size_t index, char *out, size_t size)
{
    if (!z || index >= solar_os_zoo_count(z)) return ESP_ERR_NOT_FOUND;
    const cJSON *m = metadata(entry(z, index)); char relative_path[224];
    snprintf(relative_path, sizeof(relative_path), "models/%s/%s", str(get(m, "id")), str(get(m, "version")));
    return solar_os_storage_join_path(z->root, relative_path, out, size);
}
esp_err_t solar_os_zoo_bundle_path(const solar_os_zoo_t *z, size_t i, char *out, size_t size)
{ char path[SOLAR_OS_STORAGE_PATH_MAX]; esp_err_t e = model_path(z, i, path, sizeof(path)); return e ? e : solar_os_storage_join_path(path, "bundle.json", out, size); }
bool solar_os_zoo_installed(const solar_os_zoo_t *z, size_t i)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX], receipt[SOLAR_OS_STORAGE_PATH_MAX], value[66];
    if (model_path(z, i, path, sizeof(path)) || solar_os_storage_join_path(path, ".zoo-receipt", receipt, sizeof(receipt))) return false;
    FILE *file = fopen(receipt, "rb"); if (!file) return false;
    size_t n = fread(value, 1, sizeof(value), file); fclose(file);
    return n == 64 && !memcmp(value, str(get(entry(z, i), "bundle_sha256")), 64);
}
typedef struct { char names[35][SOLAR_OS_STORAGE_PATH_MAX]; uint64_t seen, bytes; size_t count; esp_err_t error; } members_t;
static bool add_member(members_t *m, const char *name)
{
    if (!relative(name) || m->count >= 35) return false;
    for (size_t i = 0; i < m->count; ++i) {
        if (!strcasecmp(m->names[i], name)) return false;
        size_t a = strlen(m->names[i]), b = strlen(name);
        if ((a < b && !strncasecmp(m->names[i], name, a) && name[a] == '/') ||
            (b < a && !strncasecmp(m->names[i], name, b) && m->names[i][b] == '/')) return false;
    }
    strlcpy(m->names[m->count++], name, SOLAR_OS_STORAGE_PATH_MAX); return true;
}
static void check_member(const solar_os_zip_event_info_t *info, void *user)
{
    members_t *m = user; size_t i;
    for (i = 0; i < m->count; ++i) if (!strcmp(m->names[i], info->archive_name)) break;
    if (i == m->count || (m->seen & (UINT64_C(1) << i)) || info->method != 0 ||
        info->compressed_size != info->uncompressed_size ||
        (info->external_attributes >> 16 & 0170000) != 0100000 || (info->flags & ~0x800U)) m->error = ESP_ERR_INVALID_RESPONSE;
    else m->seen |= UINT64_C(1) << i;
    m->bytes += info->uncompressed_size;
}
static esp_err_t install(solar_os_zoo_t *z, size_t i, const volatile bool *cancel, solar_os_zoo_progress_fn fn, void *user)
{
    solar_os_zoo_model_t model;
    if (!solar_os_zoo_get(z, i, &model)) return ESP_ERR_NOT_FOUND;
    if (!model.compatible) return ESP_ERR_NOT_SUPPORTED;
    char final[SOLAR_OS_STORAGE_PATH_MAX], stage[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    char archive[SOLAR_OS_STORAGE_PATH_MAX], url[640], path[SOLAR_OS_STORAGE_PATH_MAX];
    if (model_path(z, i, final, sizeof(final)) || solar_os_storage_sibling_path(final, ".stage", stage, sizeof(stage)) ||
        solar_os_storage_sibling_path(final, ".old", backup, sizeof(backup)) ||
        solar_os_storage_sibling_path(final, ".zip", archive, sizeof(archive))) return ESP_ERR_INVALID_SIZE;
    solar_os_storage_usage_t usage;
    uint64_t required = (uint64_t)model.archive_bytes + model.unpacked_bytes + 4096;
    if (solar_os_storage_get_usage_for_path(z->root, &usage) != ESP_OK || usage.free_bytes < required) return ESP_ERR_NO_MEM;
    /* Recover an interrupted previous directory swap before starting a new transaction. */
    struct stat st;
    if (stat(final, &st) && !stat(backup, &st) && rename(backup, final)) return ESP_FAIL;
    esp_err_t error = remove_tree(stage); if (!error) error = remove_tree(backup);
    if (!error) error = solar_os_storage_makedirs(stage, true);
    strlcpy(url, z->source, sizeof(url)); char *slash = strrchr(url, '/');
    if (!slash) return ESP_ERR_INVALID_ARG;
    const cJSON *e = entry(z, i), *artifact = get(e, "artifact");
    snprintf(slash + 1, sizeof(url) - (size_t)(slash + 1 - url), "%s", str(get(artifact, "file")));
    if (!error) error = download(url, archive, model.archive_bytes, cancel, fn, user, "download");
    if (!error && (stat(archive, &st) || (uint64_t)st.st_size != model.archive_bytes)) error = ESP_ERR_INVALID_SIZE;
    if (!error) { report(fn, user, "verify archive", 0, 0); error = hash_file(archive, str(get(artifact, "sha256")), cancel); }
    uint8_t *manifest = NULL; size_t length = 0; solar_os_json_doc_t *doc = NULL;
    if (!error) error = solar_os_zip_read_file(archive, "bundle.json", 65536, &manifest, &length);
    if (!error) { uint8_t digest[32]; error = solar_os_crypto_sha256_once(manifest, length, digest);
        if (!error && !solar_os_crypto_sha256_matches_hex(digest, str(get(e, "bundle_sha256")))) error = ESP_ERR_INVALID_CRC; }
    if (!error) error = parse(manifest, length, &doc);
    solar_os_zip_free(manifest);
    const cJSON *bundle = doc ? solar_os_json_root(doc) : NULL, *model_file = get(bundle, "model"), *assets = get(bundle, "assets");
    members_t *members = solar_os_memory_calloc(1, sizeof(*members), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "zoo.members");
    if (!members && !error) error = ESP_ERR_NO_MEM;
    if (!error && (!eq(get(bundle, "id"), model.id) || !eq(get(bundle, "version"), model.version) ||
        !cJSON_Compare(get(bundle, "runtime"), get(e, "runtime"), true) ||
        !cJSON_Compare(get(bundle, "inputs"), get(e, "inputs"), true) ||
        !cJSON_Compare(get(bundle, "outputs"), get(e, "outputs"), true) ||
        !cJSON_Compare(get(bundle, "result"), get(e, "result"), true) ||
        !cJSON_Compare(get(bundle, "license"), get(e, "license"), true) ||
        !hash(get(model_file, "sha256")) || !cJSON_IsObject(assets) ||
        !add_member(members, "bundle.json") || !add_member(members, "zoo.json") ||
        !add_member(members, str(get(model_file, "file"))))) error = ESP_ERR_INVALID_RESPONSE;
    const cJSON *asset;
    if (!error) cJSON_ArrayForEach(asset, assets)
        if (!hash(asset) || !add_member(members, asset->string)) { error = ESP_ERR_INVALID_ARG; break; }
    if (!error) error = solar_os_zip_list(archive, check_member, members);
    if (!error && (members->error || members->seen != (UINT64_C(1) << members->count) - 1 || members->bytes != model.unpacked_bytes)) error = ESP_ERR_INVALID_RESPONSE;
    if (!error && cancelled(cancel)) error = ESP_ERR_TIMEOUT;
    if (!error) { report(fn, user, "extract", 0, 0); solar_os_unzip_options_t options = {.cancel_flag = cancel};
        error = solar_os_zip_extract(archive, stage, &options); }
    if (!error) {
        report(fn, user, "verify files", 0, 0);
        error = solar_os_storage_join_path(stage, "bundle.json", path, sizeof(path));
        if (!error) error = hash_file(path, str(get(e, "bundle_sha256")), cancel);
        if (!error) error = solar_os_storage_join_path(stage, str(get(model_file, "file")), path, sizeof(path));
        if (!error) error = hash_file(path, str(get(model_file, "sha256")), cancel);
        if (!error) cJSON_ArrayForEach(asset, assets) {
            error = solar_os_storage_join_path(stage, asset->string, path, sizeof(path));
            if (!error) error = hash_file(path, str(asset), cancel);
            if (error) break;
        }
    }
    if (!error) { solar_os_json_doc_t *sidecar = NULL;
        error = solar_os_storage_join_path(stage, "zoo.json", path, sizeof(path));
        if (!error) error = read_doc(path, 65536, &sidecar);
        if (!error && !cJSON_Compare(solar_os_json_root(sidecar), metadata(e), true)) error = ESP_ERR_INVALID_RESPONSE;
        solar_os_json_free(sidecar); }
    if (!error) { error = solar_os_storage_join_path(stage, ".zoo-receipt", path, sizeof(path));
        if (!error) error = solar_os_storage_write_file(path, str(get(e, "bundle_sha256")), 64, false); }
    if (!error && cancelled(cancel)) error = ESP_ERR_TIMEOUT;
    if (!error && rename(final, backup) && errno != ENOENT) error = ESP_FAIL;
    if (!error && rename(stage, final)) { rename(backup, final); error = ESP_FAIL; }
    if (!error) { remove_tree(backup); report(fn, user, "installed", model.archive_bytes, model.archive_bytes); }
    else remove_tree(stage);
    unlink(archive); solar_os_memory_free(members); solar_os_json_free(doc); return error;
}
esp_err_t solar_os_zoo_refresh(solar_os_zoo_t *z, const volatile bool *cancel, solar_os_zoo_progress_fn fn, void *user)
{
    if (!z) return ESP_ERR_INVALID_ARG;
    if (!transaction_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = refresh(z, cancel, fn, user); transaction_end(); return error;
}
esp_err_t solar_os_zoo_reload(solar_os_zoo_t *z)
{
    if (!z) return ESP_ERR_INVALID_ARG;
    if (!transaction_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = reload(z); transaction_end(); return error;
}
esp_err_t solar_os_zoo_install(solar_os_zoo_t *z, size_t i, const volatile bool *cancel, solar_os_zoo_progress_fn fn, void *user)
{
    if (!z) return ESP_ERR_INVALID_ARG;
    if (!transaction_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = install(z, i, cancel, fn, user); transaction_end(); return error;
}
