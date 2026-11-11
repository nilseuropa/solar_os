#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_flash_cache.h"
#include "solar_os_flash.h"

static solar_os_storage_mount_info_t mounts[5];
static size_t mount_count;
static unsigned generation = 1, leases;
static bool fail_open;
static struct { DIR *dir; unsigned generation; } directories[8];
static DIR *test_opendir(const char *path);
static struct dirent *test_readdir(DIR *dir);
static int test_closedir(DIR *dir);
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#include "../../src/services/solar_os_flash_cache.c"
#undef opendir
#undef readdir
#undef closedir

size_t strlcpy(char *out, const char *in, size_t capacity)
{ size_t n = strlen(in); if (capacity) { size_t c = n < capacity - 1 ? n : capacity - 1; memcpy(out, in, c); out[c] = 0; } return n; }
size_t solar_os_storage_mount_count(void) { return mount_count; }
bool solar_os_storage_get_mount(size_t index, solar_os_storage_mount_info_t *mount)
{ if (index >= mount_count) return false; *mount = mounts[index]; return true; }
esp_err_t solar_os_storage_join_path(const char *base, const char *relative, char *out, size_t capacity)
{ int n = snprintf(out, capacity, "%s/%s", base, relative); return n < 0 || (size_t)n >= capacity ? ESP_ERR_INVALID_SIZE : ESP_OK; }
static DIR *test_opendir(const char *path)
{
    if (fail_open) return NULL;
    DIR *dir = opendir(path);
    if (dir) for (unsigned i = 0; i < 8; i++) if (!directories[i].dir) {
        directories[i].dir = dir; directories[i].generation = generation; leases++; return dir;
    }
    assert(!dir); return NULL;
}
static struct dirent *test_readdir(DIR *dir)
{
    for (unsigned i = 0; i < 8; i++) if (directories[i].dir == dir) {
        if (directories[i].generation != generation) { errno = ENODEV; return NULL; }
        return readdir(dir);
    }
    assert(false); return NULL;
}
static int test_closedir(DIR *dir)
{
    for (unsigned i = 0; i < 8; i++) if (directories[i].dir == dir) {
        directories[i].dir = NULL; leases--; return closedir(dir);
    }
    assert(false); return -1;
}
static void mount_at(const char *path, solar_os_storage_mount_type_t type)
{ assert(mount_count < 5); mounts[mount_count].type = type; strlcpy(mounts[mount_count++].mount_point, path, sizeof(mounts[0].mount_point)); }
#include "flash_cache_generated.inc"

static void test_operations(const char *usb, const char *sd)
{
    mount_count = 0;
    assert(solar_os_flash_catalog_refresh(NULL, NULL) == ESP_ERR_INVALID_STATE && !requests);
    mount_at(usb, SOLAR_OS_STORAGE_MOUNT_USB); runtime_sd = sd; attach_sd = true;
    assert(solar_os_flash_catalog_refresh(NULL, NULL) == ESP_OK && activations == 1 && requests == 2 && !leases);
    char expected[160]; snprintf(expected, sizeof(expected), "%s/.flash", usb);
    assert(!strcmp(activated_root, expected)); /* SD arrived during HTTP; activation stayed on USB. */
    mount_count = 1; requests = 0; fail_http = true;
    assert(solar_os_flash_catalog_refresh(NULL, NULL) == ESP_FAIL && requests == 1 && !leases);
    fail_http = false; remove_media = true;
    assert(solar_os_flash_catalog_refresh(NULL, NULL) == ESP_ERR_INVALID_STATE && activations == 1 && !leases);
    fail_signature = true;
    assert(solar_os_flash_catalog_refresh(NULL, NULL) == ESP_ERR_INVALID_CRC && activations == 1 && !leases);
    fail_signature = false;
    solar_os_flash_catalog_t *catalog = NULL;
    assert(solar_os_flash_catalog_load(&catalog) == ESP_OK && catalog && !leases);
    assert(!strcmp(catalog->cache_root, expected) && !strcmp(catalog->artifacts[0].cache_root, expected));
    solar_os_flash_catalog_free(catalog);

    /* Deletion uses the artifact's cache, even when an SD volume is preferred now. */
    mount_at(sd, SOLAR_OS_STORAGE_MOUNT_SD);
    assert(solar_os_flash_catalog_load_at(expected, &catalog) == ESP_OK && !leases);
    assert(!strcmp(catalog->cache_root, expected));
    solar_os_flash_catalog_free(catalog);
    solar_os_flash_artifact_t artifact = {0};
    strlcpy(artifact.cache_root, expected, sizeof(artifact.cache_root));
    strlcpy(artifact.board_id, "board", sizeof(artifact.board_id));
    strlcpy(artifact.flavor, "full", sizeof(artifact.flavor));
    strlcpy(artifact.version, "v1", sizeof(artifact.version));
    char path[160];
    for (unsigned i = 0; i < 2; i++) {
        const char *volume = i ? sd : usb;
        const char *parts[] = {".flash", ".flash/board", ".flash/board/full", ".flash/board/full/v1"};
        for (unsigned j = 0; j < 4; j++) { snprintf(path, sizeof(path), "%s/%s", volume, parts[j]); assert(!mkdir(path, 0700)); }
        snprintf(path, sizeof(path), "%s/.flash/board/full/v1/factory.bin", volume);
        FILE *file = fopen(path, "wb"); assert(file && fputs("factory", file) >= 0 && !fclose(file));
    }
    assert(solar_os_flash_artifact_delete(&artifact) == ESP_OK && !leases);
    snprintf(path, sizeof(path), "%s/.flash/board", usb); assert(access(path, F_OK));
    snprintf(path, sizeof(path), "%s/.flash/board/full/v1/factory.bin", sd); assert(!access(path, F_OK));
    assert(!unlink(path));
    const char *parts[] = {".flash/board/full/v1", ".flash/board/full", ".flash/board", ".flash"};
    for (unsigned j = 0; j < 4; j++) { snprintf(path, sizeof(path), "%s/%s", sd, parts[j]); assert(!rmdir(path)); }
    snprintf(path, sizeof(path), "%s/.flash", usb); assert(!rmdir(path));
    mount_count = 0;
    assert(solar_os_flash_artifact_delete(&artifact) == ESP_ERR_INVALID_STATE && !leases);
    puts("Flash refresh/load/delete preserve cache selection and release storage on errors: ok");
}
int main(void)
{
    char temporary[] = "/tmp/fcXXXXXX"; assert(mkdtemp(temporary));
    char usb[32], sd[32], secondary[32], root[160], selected[160];
    snprintf(usb, sizeof(usb), "%s/usb0", temporary);
    snprintf(sd, sizeof(sd), "%s/card", temporary);
    snprintf(secondary, sizeof(secondary), "%s/other", temporary);
    assert(!mkdir(usb, 0700) && !mkdir(sd, 0700) && !mkdir(secondary, 0700));
    mount_at(temporary, SOLAR_OS_STORAGE_MOUNT_FLASH);
    mount_at(temporary, SOLAR_OS_STORAGE_MOUNT_RAMFS);
    mount_at(temporary, SOLAR_OS_STORAGE_MOUNT_IMAGE);
    assert(solar_os_flash_cache_root(root, sizeof(root)) == ESP_ERR_INVALID_STATE && !root[0]);
    assert(solar_os_flash_cache_root(NULL, 0) == ESP_ERR_INVALID_ARG);
    mount_at(usb, SOLAR_OS_STORAGE_MOUNT_USB);
    assert(solar_os_flash_cache_root(root, sizeof(root)) == ESP_OK);
    snprintf(selected, sizeof(selected), "%s/.flash", usb); assert(!strcmp(root, selected));
    assert(solar_os_flash_cache_root(selected, 4) == ESP_ERR_INVALID_SIZE);
    solar_os_flash_cache_t cache;
    assert(solar_os_flash_cache_open(NULL, &cache) == ESP_OK && leases == 1);
    /* New runtime SD attachment cannot redirect an existing USB operation. */
    mount_at(sd, SOLAR_OS_STORAGE_MOUNT_SD);
    assert(solar_os_flash_cache_root(selected, sizeof(selected)) == ESP_OK && strstr(selected, "/card/.flash"));
    assert(!strcmp(cache.root, root) && solar_os_flash_cache_check(&cache) == ESP_OK);
    solar_os_flash_cache_t pinned;
    assert(solar_os_flash_cache_open(root, &pinned) == ESP_OK && !strcmp(pinned.root, root));
    solar_os_flash_cache_close(&pinned);
    /* A replacement at the same path cannot revive the old directory lease. */
    generation++;
    assert(solar_os_flash_cache_mounted(root));
    assert(solar_os_flash_cache_check(&cache) == ESP_ERR_INVALID_STATE);
    solar_os_flash_cache_close(&cache); assert(!leases);
    assert(solar_os_flash_cache_open(root, &cache) == ESP_OK);
    mounts[3].type = SOLAR_OS_STORAGE_MOUNT_FLASH;
    assert(!solar_os_flash_cache_mounted(root));
    assert(solar_os_flash_cache_check(&cache) == ESP_ERR_INVALID_STATE);
    solar_os_flash_cache_close(&cache);
    assert(solar_os_flash_cache_open(root, &cache) == ESP_ERR_INVALID_STATE && !leases);
    fail_open = true;
    assert(solar_os_flash_cache_open(NULL, &cache) == ESP_FAIL && !leases);
    fail_open = false;
    /* Runtime SD volumes participate even without a built-in SD default. */
    assert(solar_os_flash_cache_open(NULL, &cache) == ESP_OK && strstr(cache.root, "/card/.flash"));
    for (unsigned i = 0; i < 32; i++) assert(solar_os_flash_cache_check(&cache) == ESP_OK); /* EOF is healthy. */
    solar_os_flash_cache_close(&cache); solar_os_flash_cache_close(&cache); assert(!leases);
    mount_count = 0;
    mount_at(secondary, SOLAR_OS_STORAGE_MOUNT_SD);
    mount_at(sd, SOLAR_OS_STORAGE_MOUNT_SD);
    assert(solar_os_flash_cache_root(selected, sizeof(selected)) == ESP_OK && strstr(selected, "/card/.flash"));
    test_operations(usb, sd);
    assert(!rmdir(usb) && !rmdir(sd) && !rmdir(secondary) && !rmdir(temporary));
    puts("Flash cache runtime SD/USB selection, binding and media replacement: ok");
}
