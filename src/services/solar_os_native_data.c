#include "solar_os_native_data.h"
#include "solar_os_native_data_abi.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "solar_os_crypto.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_work.h"

static bool path_valid(const char *p)
{
    if (!p || *p != '/' || strlen(p) >= SOLAR_OS_STORAGE_PATH_MAX) return false;
    for (const char *s = p; *s; ++s) if ((unsigned char)*s < 32 || *s == '\\') return false;
    char mount[SOLAR_OS_STORAGE_PATH_MAX];
    return !strstr(p, "/../") && !strstr(p, "/./") &&
        solar_os_storage_path_mount_point(p, mount, sizeof(mount)) == ESP_OK;
}
static int file_open(const char *path, solar_os_native_file_t **out, uint32_t *size)
{
    if (!out || !size || !path_valid(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL; *size = 0;
    FILE *f = fopen(path, "rb"); if (!f) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > 64U*1024U*1024U) {
        fclose(f); return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    *out = (solar_os_native_file_t *)f; *size = st.st_size; return 0;
}
static int file_read(solar_os_native_file_t *file, void *data, size_t length, size_t *read)
{
    if (!file || !data || !read || length > 65536) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    FILE *f = (FILE *)file; *read = fread(data, 1, length, f);
    return ferror(f) ? SOLAR_OS_NATIVE_ERROR_FAILED : 0;
}
static int file_seek(solar_os_native_file_t *file, uint32_t position)
{
    if (!file || position > 64U*1024U*1024U) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return fseek((FILE *)file, position, SEEK_SET) ? SOLAR_OS_NATIVE_ERROR_FAILED : 0;
}
static void file_close(solar_os_native_file_t *file) { if (file) fclose((FILE *)file); }
static int file_hash(solar_os_native_file_t *file, char hex[65], solar_os_native_cancel_fn cancel, void *user)
{
    if (!file || !hex) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int error = file_seek(file, 0); uint8_t digest[32];
    uint8_t *buffer = solar_os_memory_alloc(4096, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.hash");
    if (!buffer) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_crypto_sha256_t sha; solar_os_crypto_sha256_init(&sha);
    if (!error && solar_os_crypto_sha256_start(&sha)) error = SOLAR_OS_NATIVE_ERROR_FAILED;
    solar_os_work_budget_t budget; solar_os_work_begin(&budget);
    while (!error) {
        if (cancel && cancel(user)) { error = SOLAR_OS_NATIVE_ERROR_TIMEOUT; break; }
        size_t n = 0; error = file_read(file, buffer, 4096, &n);
        if (!error && solar_os_crypto_sha256_update(&sha, buffer, n)) error = SOLAR_OS_NATIVE_ERROR_FAILED;
        if (error || n < 4096) break;
        solar_os_work_checkpoint(&budget);
    }
    if (!error && solar_os_crypto_sha256_finish(&sha, digest)) error = SOLAR_OS_NATIVE_ERROR_FAILED;
    if (!error && solar_os_crypto_bytes_to_hex(digest, 32, hex, 65)) error = SOLAR_OS_NATIVE_ERROR_FAILED;
    solar_os_crypto_sha256_free(&sha); solar_os_memory_free(buffer);
    int rewind_error = file_seek(file, 0); return error ? error : rewind_error;
}
static int file_replace(const char *path, const void *data, size_t length)
{
    if (!path_valid(path) || (!data && length) || length > 65536) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    char temp[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_sibling_path(path, ".native.tmp", temp, sizeof(temp)) ||
        solar_os_storage_sibling_path(path, ".native.old", backup, sizeof(backup))) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    esp_err_t error = solar_os_storage_write_file(temp, data, length, false);
    if (!error) error = solar_os_storage_replace_file(temp, path, backup);
    if (error) unlink(temp);
    return error ? SOLAR_OS_NATIVE_ERROR_FAILED : 0;
}
static bool unique_keys(const cJSON *root, unsigned depth)
{
    if (depth > 32) return false;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, root) {
        if (cJSON_IsObject(root)) for (b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
        if (!unique_keys(a, depth + 1)) return false;
    }
    return true;
}
static int json_parse(const char *text, size_t size, solar_os_native_json_value_t **out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (!text || !size || size > 65536 || memchr(text, 0, size)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    unsigned depth = 0; bool quote = false, escape = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = text[i];
        if (i + 6 <= size && !memcmp(text + i, "\\u0000", 6)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (quote) {
            if (c < 32) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') quote = false;
        } else if (c == '"') quote = true;
        else if (c == '{' || c == '[') { if (++depth > 32) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE; }
        else if (c == '}' || c == ']') { if (!depth) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; --depth; }
    }
    if (quote || depth || solar_os_json_init()) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, size, &end, false);
    while (end && end < text + size && isspace((unsigned char)*end)) ++end;
    if (!root || end != text + size || (!cJSON_IsObject(root) && !cJSON_IsArray(root)) || !unique_keys(root, 0)) {
        cJSON_Delete(root); return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *out = (solar_os_native_json_value_t *)root; return 0;
}
static const solar_os_native_json_value_t *json_get(const solar_os_native_json_value_t *v, const char *key)
{ return (const void *)cJSON_GetObjectItemCaseSensitive((const cJSON *)v, key); }
static const solar_os_native_json_value_t *json_at(const solar_os_native_json_value_t *v, size_t index)
{ return index > INT_MAX ? NULL : (const void *)cJSON_GetArrayItem((const cJSON *)v, index); }
static const char *json_string(const solar_os_native_json_value_t *v)
{ return cJSON_IsString((const cJSON *)v) ? ((const cJSON *)v)->valuestring : NULL; }
static bool json_uint(const solar_os_native_json_value_t *v, uint64_t *out)
{
    const cJSON *n = (const cJSON *)v;
    if (!out || !cJSON_IsNumber(n) || !isfinite(n->valuedouble) || n->valuedouble < 0 ||
        n->valuedouble > 9007199254740991.0 || floor(n->valuedouble) != n->valuedouble) return false;
    *out = n->valuedouble; return true;
}
static size_t json_count(const solar_os_native_json_value_t *v) { return cJSON_GetArraySize((const cJSON *)v); }
static const char *json_key(const solar_os_native_json_value_t *v) { return v ? ((const cJSON *)v)->string : NULL; }
static void json_free(solar_os_native_json_value_t *v) { cJSON_Delete((cJSON *)v); }
static uint32_t json_kind(const solar_os_native_json_value_t *v)
{ return cJSON_IsObject((const cJSON *)v) ? SOLAR_OS_NATIVE_JSON_OBJECT : cJSON_IsArray((const cJSON *)v) ? SOLAR_OS_NATIVE_JSON_ARRAY : 0; }
static const solar_os_native_files_api_v1_t files_api = {1, sizeof(files_api), file_open, file_read, file_seek, file_close, file_hash, file_replace};
static const solar_os_native_json_api_v1_t json_api = {1, sizeof(json_api), json_parse, json_get, json_at, json_string, json_uint, json_count, json_key, json_free, json_kind};
const void *solar_os_native_data_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    if (!name || abi != 1) return NULL;
    if (!strcmp(name, SOLAR_OS_NATIVE_FILES_SERVICE) && minimum <= sizeof(files_api)) return &files_api;
    if (!strcmp(name, SOLAR_OS_NATIVE_JSON_SERVICE) && minimum <= sizeof(json_api)) return &json_api;
    return NULL;
}
