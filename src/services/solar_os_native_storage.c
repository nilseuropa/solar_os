#include "solar_os_native_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include "solar_os_disk_image.h"

static atomic_bool write_busy;
static int storage_result(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}
static bool path_valid(const char *path)
{ return path && path[0] == '/' && strnlen(path, SOLAR_OS_STORAGE_PATH_MAX) < SOLAR_OS_STORAGE_PATH_MAX; }
static bool stop_requested(void *opaque)
{
    solar_os_native_storage_context_t *context = opaque;
    if (!context) return true;
    if (atomic_load(&context->stopped)) return true;
    if (context->cancel && context->cancel(context->cancel_user)) atomic_store(&context->stopped, true);
    return atomic_load(&context->stopped);
}
static int resolve_path(void *opaque, const char *path, char *resolved, size_t capacity)
{
    solar_os_native_storage_context_t *context = opaque;
    if (!context || !resolved || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return storage_result(solar_os_storage_resolve_path_at(context->cwd, path, resolved, capacity));
}
static int file_size(void *opaque, const char *path, uint32_t *bytes)
{
    if (!opaque || !path_valid(path) || !bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *bytes = 0;
    if (stop_requested(opaque)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    solar_os_storage_metadata_t metadata;
    esp_err_t code = solar_os_storage_stat(path, &metadata);
    if (code == ESP_OK && (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || metadata.size_bytes > UINT32_MAX))
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (code == ESP_OK) *bytes = metadata.size_bytes;
    return storage_result(code);
}
static int read_file(void *opaque, const char *path, uint32_t offset,
                      void *buffer, size_t capacity, size_t *read_bytes)
{
    if (!opaque || !path_valid(path) || !buffer || !read_bytes ||
        capacity > SOLAR_OS_NATIVE_STORAGE_READ_MAX || offset > INT32_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *read_bytes = 0;
    if (stop_requested(opaque)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? SOLAR_OS_NATIVE_ERROR_NOT_FOUND : SOLAR_OS_NATIVE_ERROR_FAILED;
    int code = SOLAR_OS_NATIVE_OK;
    if (fseek(file, (long)offset, SEEK_SET)) code = SOLAR_OS_NATIVE_ERROR_FAILED;
    else {
        *read_bytes = fread(buffer, 1, capacity, file);
        if (ferror(file)) code = SOLAR_OS_NATIVE_ERROR_FAILED;
    }
    if (fclose(file) && !code) code = SOLAR_OS_NATIVE_ERROR_FAILED;
    return code;
}
static int replace_file(void *opaque, const char *path, const void *buffer, size_t bytes)
{
    if (!opaque || !path_valid(path) || (!buffer && bytes) || bytes > SOLAR_OS_NATIVE_STORAGE_REPLACE_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (stop_requested(opaque)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    if (solar_os_disk_image_path_busy(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (atomic_exchange(&write_busy, true)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    char staged[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    FILE *file = NULL;
    bool owned = false;
    int code = storage_result(solar_os_storage_sibling_path(path, ".native.tmp", staged, sizeof(staged)));
    if (!code) code = storage_result(solar_os_storage_sibling_path(path, ".native.bak", backup, sizeof(backup)));
    if (code) goto done;
    int fd = open(staged, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) { code = SOLAR_OS_NATIVE_ERROR_FAILED; goto done; }
    owned = true;
    file = fdopen(fd, "wb");
    if (!file) { close(fd); code = SOLAR_OS_NATIVE_ERROR_FAILED; goto done; }
    const unsigned char *data = buffer;
    for (size_t offset = 0; offset < bytes && !code;) {
        if (stop_requested(opaque)) { code = SOLAR_OS_NATIVE_ERROR_TIMEOUT; break; }
        size_t count = bytes - offset;
        if (count > SOLAR_OS_STORAGE_WRITE_MAX_BYTES) count = SOLAR_OS_STORAGE_WRITE_MAX_BYTES;
        if (fwrite(data + offset, 1, count, file) != count) code = SOLAR_OS_NATIVE_ERROR_FAILED;
        offset += count;
    }
    if (!code) code = storage_result(solar_os_storage_sync_file(file));
    if (fclose(file) && !code) code = SOLAR_OS_NATIVE_ERROR_FAILED;
    file = NULL;
    if (!code && stop_requested(opaque)) code = SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    if (!code) code = storage_result(solar_os_storage_replace_file(staged, path, backup));
done:
    if (file) fclose(file);
    if (owned && code) (void)solar_os_storage_remove(staged);
    atomic_store(&write_busy, false);
    return code;
}
static int directory_entry(void *opaque, const char *path, uint32_t cursor,
    solar_os_native_storage_entry_v1_t *entry, bool *more)
{
    if (!opaque || !path_valid(path) || !entry || !more) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(entry, 0, sizeof(*entry)); *more = false;
    if (stop_requested(opaque)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    solar_os_storage_entry_t item;
    size_t count = 0, next = cursor;
    esp_err_t err = solar_os_storage_scandir(path, cursor, 1U, &item, &count, &next, more);
    if (err != ESP_OK) return storage_result(err);
    if (!count) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    memcpy(entry->name, item.name, strlen(item.name)+1);
    entry->bytes = item.metadata.size_bytes > UINT32_MAX ? UINT32_MAX : item.metadata.size_bytes;
    entry->flags = item.metadata.type == SOLAR_OS_STORAGE_ENTRY_FILE ? SOLAR_OS_NATIVE_STORAGE_ENTRY_FILE :
        item.metadata.type == SOLAR_OS_STORAGE_ENTRY_DIRECTORY ? SOLAR_OS_NATIVE_STORAGE_ENTRY_DIRECTORY : 0;
    return SOLAR_OS_NATIVE_OK;
}
static int make_directories(void *opaque, const char *path, bool exist_ok)
{
    if (!opaque || !path_valid(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (stop_requested(opaque)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    return storage_result(solar_os_storage_makedirs(path, exist_ok));
}
static int remove_path(void *opaque, const char *path, bool directory)
{
    if (!opaque || !path_valid(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (solar_os_disk_image_path_busy(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    bool exists = false;
    esp_err_t err = solar_os_storage_exists(path, &exists);
    if (err != ESP_OK) return storage_result(err);
    if (!exists) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return storage_result(directory ? solar_os_storage_rmdir(path) : solar_os_storage_remove(path));
}
esp_err_t solar_os_native_storage_init(solar_os_native_storage_context_t *context,
    const char *cwd, solar_os_native_cancel_fn cancel, void *cancel_user)
{
    if (!context) return ESP_ERR_INVALID_ARG;
    memset(context, 0, sizeof(*context));
    esp_err_t code = solar_os_storage_resolve_path_at(cwd, NULL, context->cwd, sizeof(context->cwd));
    if (code != ESP_OK) return code;
    context->cancel = cancel; context->cancel_user = cancel_user;
    atomic_init(&context->stopped, false);
    context->api = (solar_os_native_storage_api_v1_t){
        .abi_version = SOLAR_OS_NATIVE_STORAGE_ABI, .struct_size = sizeof(context->api), .context = context,
        .resolve_path = resolve_path, .file_size = file_size, .read_file = read_file,
        .replace_file = replace_file, .stop_requested = stop_requested,
        .directory_entry = directory_entry, .make_directories = make_directories, .remove_path = remove_path,
    };
    return ESP_OK;
}
const void *solar_os_native_storage_get_service(const char *name, uint32_t abi,
    uint32_t minimum_size, void *opaque)
{
    solar_os_native_storage_context_t *context = opaque;
    if (context && name && !strcmp(name, SOLAR_OS_NATIVE_STORAGE_SERVICE) &&
        abi == SOLAR_OS_NATIVE_STORAGE_ABI && minimum_size <= sizeof(context->api)) return &context->api;
    return NULL;
}
