#include "solar_os_media_playlist.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "solar_os_memory.h"

static atomic_flag playlist_busy[] = { ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT };
static uint32_t revisions[sizeof(playlist_busy) / sizeof(playlist_busy[0])];

typedef struct {
    uint32_t magic;
    uint16_t version, count;
    uint32_t generation;
} player_playlist_store_t;

static bool supported(solar_os_media_playlist_kind_t kind, const char *path)
{
    if (kind == SOLAR_OS_MEDIA_PLAYLIST_RTSP) return !strncmp(path, "rtsp://", 7U);
    if (strstr(path, "://")) return false;
    const char *ext = strrchr(path, '.');
    return ext && (kind == SOLAR_OS_MEDIA_PLAYLIST_AUDIO ?
        (!strcasecmp(ext, ".wav") || !strcasecmp(ext, ".mp3")) :
        (!strcasecmp(ext, ".mpg") || !strcasecmp(ext, ".mpeg")));
}

static esp_err_t append(const solar_os_m3u_entry_t *entry, void *user)
{
    solar_os_media_playlist_t *list = user;
    if (!supported(list->kind, entry->path)) return ESP_ERR_NOT_SUPPORTED;
    if (list->count == SOLAR_OS_MEDIA_PLAYLIST_CAPACITY) return ESP_ERR_INVALID_SIZE;
    void *entries = solar_os_memory_realloc(list->entries,
        (list->count + 1U) * sizeof(*list->entries),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "media.playlist");
    if (!entries) return ESP_ERR_NO_MEM;
    list->entries = entries;
    list->entries[list->count++] = *entry;
    return ESP_OK;
}

static bool get(size_t index, solar_os_m3u_entry_t *entry, void *user)
{
    solar_os_media_playlist_t *list = user;
    if (index >= list->count) return false;
    *entry = list->entries[index];
    return true;
}

static esp_err_t save_locked(solar_os_media_playlist_t *list)
{
    uint32_t *revision = &revisions[list->kind];
    if (list->revision != *revision) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = solar_os_m3u_write(list->path, list->count, get, list);
    if (err == ESP_OK) list->revision = ++*revision;
    return err;
}

static esp_err_t save(solar_os_media_playlist_t *list)
{
    if (!list->path[0]) {
        ++list->revision;
        return ESP_OK;
    }
    atomic_flag *busy = &playlist_busy[list->kind];
    if (atomic_flag_test_and_set(busy)) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = save_locked(list);
    atomic_flag_clear(busy);
    return err;
}

esp_err_t solar_os_media_playlist_import(solar_os_media_playlist_t *list, const char *path)
{
    if (!list || !path) return ESP_ERR_INVALID_ARG;
    solar_os_media_playlist_t imported = {.kind = list->kind, .revision = list->revision};
    esp_err_t err = solar_os_m3u_read(path, append, &imported);
    if (err == ESP_OK) {
        strlcpy(imported.path, list->path, sizeof(imported.path));
        err = save(&imported);
    }
    if (err == ESP_OK) {
        solar_os_media_playlist_close(list);
        *list = imported;
    } else solar_os_media_playlist_close(&imported);
    return err;
}

esp_err_t solar_os_media_playlist_add(solar_os_media_playlist_t *list, const char *path, size_t *index)
{
    if (!list || !path || !path[0]) return ESP_ERR_INVALID_ARG;
    solar_os_m3u_entry_t entry = {.duration = -1};
    esp_err_t err = ESP_OK;
    if (strstr(path, "://")) {
        if (strlcpy(entry.path, path, sizeof(entry.path)) >= sizeof(entry.path)) return ESP_ERR_INVALID_SIZE;
    } else err = solar_os_storage_resolve_path(path, entry.path, sizeof(entry.path));
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < list->count; i++) {
        if (!strcmp(entry.path, list->entries[i].path)) {
            if (index) *index = i;
            return ESP_OK;
        }
    }
    err = append(&entry, list);
    if (err != ESP_OK) return err;
    err = save(list);
    if (err != ESP_OK) list->count--;
    else if (index) *index = list->count - 1U;
    return err;
}

esp_err_t solar_os_media_playlist_remove(solar_os_media_playlist_t *list, size_t index)
{
    if (!list || index >= list->count) return ESP_ERR_INVALID_ARG;
    solar_os_m3u_entry_t removed = list->entries[index];
    memmove(list->entries + index, list->entries + index + 1,
            (--list->count - index) * sizeof(removed));
    esp_err_t err = save(list);
    if (err != ESP_OK) {
        memmove(list->entries + index + 1, list->entries + index,
                (list->count - index) * sizeof(removed));
        list->entries[index] = removed;
        list->count++;
    }
    return err;
}

esp_err_t solar_os_media_playlist_set(solar_os_media_playlist_t *list, size_t index,
                                      const solar_os_m3u_entry_t *entry)
{
    if (!list || !entry || index > list->count ||
        strnlen(entry->path, sizeof(entry->path)) == sizeof(entry->path) ||
        !supported(list->kind, entry->path)) return ESP_ERR_INVALID_ARG;
    const bool added = index == list->count;
    solar_os_m3u_entry_t previous;
    esp_err_t err;
    if (added) {
        err = append(entry, list);
        if (err != ESP_OK) return err;
    } else {
        previous = list->entries[index];
        list->entries[index] = *entry;
    }
    err = save(list);
    if (err != ESP_OK) {
        if (added) list->count--;
        else list->entries[index] = previous;
    }
    return err;
}

void solar_os_media_playlist_close(solar_os_media_playlist_t *list)
{
    if (!list) return;
    solar_os_memory_free(list->entries);
    memset(list, 0, sizeof(*list));
}

esp_err_t solar_os_media_playlist_refresh(solar_os_media_playlist_t *list)
{
    if (!list) return ESP_ERR_INVALID_ARG;
    if (!list->path[0]) return ESP_OK;
    atomic_flag *busy = &playlist_busy[list->kind];
    if (atomic_flag_test_and_set(busy)) return ESP_ERR_INVALID_STATE;
    const uint32_t revision = revisions[list->kind];
    esp_err_t err = ESP_OK;
    if (list->revision != revision) {
        solar_os_media_playlist_t loaded = {.kind = list->kind, .revision = revision};
        strlcpy(loaded.path, list->path, sizeof(loaded.path));
        err = solar_os_m3u_read(loaded.path, append, &loaded);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            solar_os_media_playlist_close(list);
            *list = loaded;
            err = ESP_OK;
        } else solar_os_media_playlist_close(&loaded);
    }
    atomic_flag_clear(busy);
    return err;
}

static esp_err_t migrate_player(solar_os_media_playlist_t *list, const char *directory)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_join_path(directory, "playlist.bin", path, sizeof(path));
    if (err != ESP_OK) return err;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    player_playlist_store_t store;
    bool valid = fread(&store, sizeof(store), 1, file) == 1 &&
        store.magic == 0x50534f53U && store.version == 1U &&
        store.count <= SOLAR_OS_MEDIA_PLAYLIST_CAPACITY;
    if (valid && store.count) {
        list->entries = solar_os_memory_calloc(store.count, sizeof(*list->entries),
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "player.migrate");
        if (!list->entries) { fclose(file); return ESP_ERR_NO_MEM; }
        for (size_t i = 0; valid && i < store.count; i++) {
            solar_os_m3u_entry_t *entry = &list->entries[i];
            /* Version 1 stored fixed 160-byte paths before M3U supported
             * longer network URLs. Keep its on-disk record width. */
            const size_t legacy_path_bytes = 160;
            valid = fread(entry->path, legacy_path_bytes, 1, file) == 1 &&
                memchr(entry->path, 0, legacy_path_bytes) != NULL;
            entry->duration = -1;
        }
    }
    valid = valid && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    list->count = store.count;
    /* M3U takes precedence after a successful migration. Keep the backup. */
    return save_locked(list);
}

esp_err_t solar_os_media_playlist_open(solar_os_media_playlist_t *list,
                                       solar_os_media_playlist_kind_t kind,
                                       const char *argument, size_t *selected)
{
    if (!list || (unsigned)kind > SOLAR_OS_MEDIA_PLAYLIST_RTSP) return ESP_ERR_INVALID_ARG;
    memset(list, 0, sizeof(*list));
    list->kind = kind;
    if (selected) *selected = 0;
    const char *directory = kind == SOLAR_OS_MEDIA_PLAYLIST_AUDIO ? ".player" :
        kind == SOLAR_OS_MEDIA_PLAYLIST_VIDEO ? ".vplay" : ".rtsp";
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_default_path(directory, path, sizeof(path));
    if (err == ESP_OK) err = solar_os_storage_makedirs(path, true);
    if (err == ESP_OK) err = solar_os_storage_join_path(path, "playlist.m3u", list->path, sizeof(list->path));
    if (err != ESP_OK) {
        /* Direct live playback also works on systems without mounted storage. */
        if (kind == SOLAR_OS_MEDIA_PLAYLIST_RTSP && (!argument || !solar_os_m3u_path(argument))) {
            list->path[0] = 0;
            return argument ? solar_os_media_playlist_add(list, argument, selected) : ESP_OK;
        }
        return err;
    }
    const bool import = argument && solar_os_m3u_path(argument);
    atomic_flag *busy = &playlist_busy[kind];
    if (atomic_flag_test_and_set(busy)) return ESP_ERR_INVALID_STATE;
    list->revision = revisions[kind];
    if (!import) {
        err = solar_os_m3u_read(list->path, append, list);
        if (err == ESP_ERR_NOT_FOUND)
            err = kind == SOLAR_OS_MEDIA_PLAYLIST_AUDIO ? migrate_player(list, path) : ESP_OK;
    }
    atomic_flag_clear(busy);
    if (import) {
        err = solar_os_storage_resolve_path(argument, path, sizeof(path));
        if (err == ESP_OK) err = solar_os_media_playlist_import(list, path);
    } else if (err == ESP_OK && argument) err = solar_os_media_playlist_add(list, argument, selected);
    if (err != ESP_OK) solar_os_media_playlist_close(list);
    return err;
}
