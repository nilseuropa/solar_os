#pragma once
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_media_playlist.h"
#include "solar_os_memory.h"
#include "freertos/semphr.h"

static char root[] = "/tmp/solaros-playlist-XXXXXX";
static bool mounted = true, fail_save;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int mutex; return &mutex; }
#include "../../src/services/solar_os_player_playlist.c"

size_t strlcpy(char *out, const char *text, size_t size)
{
    size_t len = strlen(text);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(out, text, n); out[n] = 0; }
    return len;
}
void *solar_os_memory_realloc(void *ptr, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return realloc(ptr, size); }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(n, size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
esp_err_t solar_os_storage_default_path(const char *relative, char *path, size_t size)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;
    return snprintf(path, size, "%s/%s", root, relative) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t solar_os_storage_join_path(const char *base, const char *relative, char *path, size_t size)
{ return snprintf(path, size, "%s/%s", base, relative) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_resolve_path_at(const char *cwd, const char *arg, char *path, size_t size)
{
    char raw[512];
    int n = arg[0] == '/' ? snprintf(raw, sizeof(raw), "%s", arg) :
        snprintf(raw, sizeof(raw), "%s/%s", cwd ? cwd : root, arg);
    if (n < 0 || n >= SOLAR_OS_STORAGE_PATH_MAX) return ESP_ERR_INVALID_SIZE;
    char *parts[80], *save, *part; size_t count = 0;
    for (part = strtok_r(raw, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (count) count--; }
        else parts[count++] = part;
    }
    size_t used = 0;
    path[0] = 0;
    for (size_t i = 0; i < count; i++) {
        n = snprintf(path + used, size - used, "/%s", parts[i]);
        if (n < 0 || (size_t)n >= size - used) return ESP_ERR_INVALID_SIZE;
        used += n;
    }
    return ESP_OK;
}
esp_err_t solar_os_storage_resolve_path(const char *arg, char *path, size_t size)
{ return solar_os_storage_resolve_path_at(root, arg, path, size); }
esp_err_t solar_os_storage_makedirs(const char *path, bool exist_ok)
{ return mkdir(path, 0700) == 0 || (exist_ok && errno == EEXIST) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_exists(const char *path, bool *exists)
{ struct stat st; *exists = stat(path, &st) == 0; return ESP_OK; }
esp_err_t solar_os_storage_sibling_path(const char *path, const char *suffix, char *out, size_t size)
{ return snprintf(out, size, "%s%s", path, suffix) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_sync_file(FILE *file)
{ return fflush(file) == 0 && fsync(fileno(file)) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_remove(const char *path) { return unlink(path) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{
    if (fail_save) return ESP_FAIL;
    struct stat st; bool exists = stat(active, &st) == 0;
    if (exists && rename(active, backup)) return ESP_FAIL;
    if (rename(staged, active)) { if (exists) assert(rename(backup, active) == 0); return ESP_FAIL; }
    if (exists) assert(unlink(backup) == 0);
    return ESP_OK;
}

static char *fixture(const void *data, size_t len)
{
    static char path[160];
    snprintf(path, sizeof(path), "%s/input.m3u", root);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, len, file) == len); assert(!fclose(file));
    return path;
}
static const char *text_fixture(const char *text) { return fixture(text, strlen(text)); }

