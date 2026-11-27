#include "solar_os_native_data.h"
#include "solar_os_native_data_abi.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_crypto.h"
#include "solar_os_native_json.h"
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
static int state_directory(const char *name, char *path, size_t capacity)
{
    if (!name || !*name || strlen(name) > 32 || !path || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *path = 0;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!solar_os_storage_flash_is_mounted()) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const char *mount = solar_os_storage_flash_mount_point();
    if (!mount || !*mount) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int n = snprintf(path, capacity, "%s/.%s", mount, name);
    if (n < 0 || (size_t)n >= capacity || !path_valid(path)) { *path = 0; return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE; }
    if (solar_os_storage_makedirs(path, true)) { *path = 0; return SOLAR_OS_NATIVE_ERROR_FAILED; }
    return 0;
}
static const solar_os_native_files_api_v1_t files_api = {1, sizeof(files_api), file_open, file_read, file_seek, file_close, file_hash, file_replace, state_directory};
const void *solar_os_native_data_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_data_dispatch.inc"
#include "generated/native_data_providers.inc"
    return NULL;
}
