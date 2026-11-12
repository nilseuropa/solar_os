#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_native_storage.h"
#include "solar_os_disk_image.h"

static bool busy_path;
static unsigned cancel_calls, cancel_at, rename_calls, fail_rename;
static bool reenter;
static int reentered;
static solar_os_native_storage_api_v1_t *current;
static char nested_path[SOLAR_OS_STORAGE_PATH_MAX];
static bool cancelled(void *user)
{ assert(user == &cancel_calls); return cancel_at && ++cancel_calls >= cancel_at; }
bool solar_os_disk_image_path_busy(const char *path) { (void)path; return busy_path; }
void solar_os_storage_notify_changed(void) {}
esp_err_t solar_os_storage_resolve_path_at(const char *cwd, const char *path, char *out, size_t capacity)
{
    if (!cwd) cwd = "/";
    int n = !path ? snprintf(out, capacity, "%s", cwd) : path[0] == '/' ?
        snprintf(out, capacity, "%s", path) : snprintf(out, capacity, "%s/%s", cwd, path);
    return n >= 0 && (size_t)n < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t solar_os_storage_stat(const char *path, solar_os_storage_metadata_t *metadata)
{
    struct stat info;
    if (stat(path, &info)) return ESP_ERR_NOT_FOUND;
    *metadata = (solar_os_storage_metadata_t){
        .type = S_ISREG(info.st_mode) ? SOLAR_OS_STORAGE_ENTRY_FILE : SOLAR_OS_STORAGE_ENTRY_DIRECTORY,
        .size_bytes = info.st_size,
    };
    return ESP_OK;
}
esp_err_t solar_os_storage_remove(const char *path) { return remove(path) ? ESP_FAIL : ESP_OK; }
static int checked_rename(const char *from, const char *to)
{
    if (reenter) {
        reenter = false;
        reentered = current->replace_file(current->context, nested_path, "other", 5);
    }
    if (++rename_calls == fail_rename) { errno = EIO; return -1; }
    /* Match FatFs: rename refuses an existing destination. */
    if (access(to, F_OK) == 0) { errno = EEXIST; return -1; }
    return rename(from, to);
}
#define ESP_LOGW(...) ((void)0)
#define rename checked_rename
#include "storage_replace.inc"
#undef rename

static void put(const char *path, const char *text)
{ FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(!fclose(f)); }
static void expect(const char *path, const char *text)
{ char buffer[64] = {0}; FILE *f = fopen(path, "rb"); assert(f); size_t n = fread(buffer, 1, sizeof(buffer), f); assert(!fclose(f)); assert(n == strlen(text) && !memcmp(buffer, text, n)); }
int main(int argc, char **argv)
{
    assert(argc == 2);
    solar_os_native_storage_context_t one, two;
    char cwd[] = "/first";
    assert(!solar_os_native_storage_init(&one, cwd, NULL, NULL));
    assert(!solar_os_native_storage_init(&two, "/second", NULL, NULL));
    cwd[1] = 'X';
    const solar_os_native_storage_api_v1_t *a = solar_os_native_storage_get_service("storage", 1, sizeof(*a), &one);
    const solar_os_native_storage_api_v1_t *b = solar_os_native_storage_get_service("storage", 1, sizeof(*b), &two);
    assert(a && b && a != b && a->context != b->context);
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!a->resolve_path(a->context, "input.c", path, sizeof(path)) && !strcmp(path, "/first/input.c"));
    assert(!b->resolve_path(b->context, "input.c", path, sizeof(path)) && !strcmp(path, "/second/input.c"));
    assert(!solar_os_native_storage_get_service("storage", 2, sizeof(*a), &one));
    assert(!solar_os_native_storage_get_service("storage", 1, sizeof(*a)+1, &one));
    assert(!solar_os_native_storage_init(&one, argv[1], cancelled, &cancel_calls)); current = &one.api;
    assert(!current->resolve_path(current->context, "output.elf", path, sizeof(path)));
    char staged[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_storage_sibling_path(path, ".native.tmp", staged, sizeof(staged)));
    assert(!solar_os_storage_sibling_path(path, ".native.bak", backup, sizeof(backup)));
    put(path, "original");
    uint32_t bytes = 999; size_t count = 999; char data[16] = {0};
    assert(!current->file_size(current->context, path, &bytes) && bytes == 8);
    assert(!current->read_file(current->context, path, 2, data, 3, &count) && count == 3 && !memcmp(data, "igi", 3));
    assert(current->file_size(current->context, "relative", &bytes) == -2);
    assert(current->read_file(current->context, path, 0, data, 65537, &count) == -2);
    assert(current->replace_file(current->context, path, data, SOLAR_OS_NATIVE_STORAGE_REPLACE_MAX+1) == -2);
    busy_path = true; assert(current->replace_file(current->context, path, "new", 3) == -3); busy_path = false;
    expect(path, "original");
    put(staged, "unowned");
    assert(current->replace_file(current->context, path, "new", 3) == -1);
    expect(path, "original"); expect(staged, "unowned"); assert(!remove(staged));
    rename_calls = 0; fail_rename = 2;
    assert(current->replace_file(current->context, path, "new", 3) == -1);
    expect(path, "original"); assert(access(staged, F_OK) && access(backup, F_OK));
    fail_rename = 0;
    assert(!current->replace_file(current->context, path, "new", 3)); expect(path, "new");
    unsigned char *large = calloc(1, 131072); assert(large);
    cancel_calls = 0; cancel_at = 3;
    assert(current->replace_file(current->context, path, large, 131072) == -7);
    expect(path, "new"); assert(access(staged, F_OK) && access(backup, F_OK));
    assert(current->stop_requested(current->context));
    free(large); cancel_at = 0;
    assert(!solar_os_native_storage_init(&one, argv[1], cancelled, &cancel_calls));
    assert(!current->resolve_path(current->context, "nested.elf", nested_path, sizeof(nested_path)));
    reenter = true;
    assert(!current->replace_file(current->context, path, "final", 5));
    assert(reentered == -3 && access(nested_path, F_OK)); expect(path, "final");
    assert(!current->replace_file(current->context, path, "again", 5)); expect(path, "again");
    assert(!remove(path));
    puts("native storage scope, bounds, rollback, cancellation and writer serialization passed");
    return 0;
}
