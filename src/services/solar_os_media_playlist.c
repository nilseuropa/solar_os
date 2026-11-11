#include "solar_os_media_playlist.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "solar_os_memory.h"

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

esp_err_t solar_os_media_playlist_save(solar_os_media_playlist_t *list)
{
    if (!list) return ESP_ERR_INVALID_ARG;
    if (!list->path[0]) return ESP_OK;
    return solar_os_m3u_write(list->path, list->count, get, list);
}

esp_err_t solar_os_media_playlist_import(solar_os_media_playlist_t *list, const char *path)
{
    if (!list || !path) return ESP_ERR_INVALID_ARG;
    solar_os_media_playlist_t imported = {.kind = list->kind};
    esp_err_t err = solar_os_m3u_read(path, append, &imported);
    if (err == ESP_OK) {
        strlcpy(imported.path, list->path, sizeof(imported.path));
        err = solar_os_media_playlist_save(&imported);
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
    err = solar_os_media_playlist_save(list);
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
    esp_err_t err = solar_os_media_playlist_save(list);
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
    err = solar_os_media_playlist_save(list);
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

esp_err_t solar_os_media_playlist_open(solar_os_media_playlist_t *list,
                                       solar_os_media_playlist_kind_t kind,
                                       const char *argument, size_t *selected)
{
    if (!list || kind > SOLAR_OS_MEDIA_PLAYLIST_RTSP) return ESP_ERR_INVALID_ARG;
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
    if (argument && solar_os_m3u_path(argument)) {
        err = solar_os_storage_resolve_path(argument, path, sizeof(path));
        return err == ESP_OK ? solar_os_media_playlist_import(list, path) : err;
    }
    err = solar_os_m3u_read(list->path, append, list);
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK && argument) err = solar_os_media_playlist_add(list, argument, selected);
    if (err != ESP_OK) solar_os_media_playlist_close(list);
    return err;
}
