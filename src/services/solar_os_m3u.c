#include "solar_os_m3u.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool solar_os_m3u_path(const char *path)
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    return ext && (!strcasecmp(ext, ".m3u") || !strcasecmp(ext, ".m3u8"));
}

static bool single_line(const char *text, size_t capacity)
{
    if (!text || strnlen(text, capacity) >= capacity) return false;
    for (; *text; text++) if ((unsigned char)*text < 32U || *text == 127) return false;
    return true;
}

static esp_err_t local_file_uri(char **text)
{
    if (strncmp(*text, "file://", 7U)) return ESP_OK;
    char *source = *text + 7U;
    if (!strncmp(source, "localhost/", 10U)) source += 9U;
    if (*source != '/') return ESP_ERR_NOT_SUPPORTED;
    char *out = *text;
    while (*source) {
        if (*source == '%') {
            if (!source[1] || !source[2] || !isxdigit((unsigned char)source[1]) ||
                !isxdigit((unsigned char)source[2])) return ESP_ERR_INVALID_ARG;
            char hex[3] = {source[1], source[2], 0};
            unsigned long value = strtoul(hex, NULL, 16);
            if (value < 32U || value == 127U) return ESP_ERR_INVALID_ARG;
            *out++ = (char)value;
            source += 3;
        } else *out++ = *source++;
    }
    *out = 0;
    return ESP_OK;
}

esp_err_t solar_os_m3u_read(const char *path, solar_os_m3u_read_fn accept, void *user)
{
    if (!path || !accept) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    char line[1024];
    solar_os_m3u_entry_t entry = {.duration = -1};
    esp_err_t err = ESP_OK;
    bool first = true;
    size_t bytes = 0;
    for (;;) {
        size_t len = 0;
        int ch;
        while ((ch = fgetc(file)) != EOF && ch != '\n') {
            if (!ch) { err = ESP_ERR_INVALID_ARG; break; }
            if (++bytes > SOLAR_OS_STORAGE_READ_MAX_BYTES || len == sizeof(line) - 1U) {
                err = ESP_ERR_INVALID_SIZE; break;
            }
            line[len++] = (char)ch;
        }
        if (err != ESP_OK || (ch == EOF && !len)) break;
        if (ch == '\n' && ++bytes > SOLAR_OS_STORAGE_READ_MAX_BYTES) { err = ESP_ERR_INVALID_SIZE; break; }
        line[len] = 0;
        char *text = line;
        if (first && len >= 3U && !memcmp(text, "\xef\xbb\xbf", 3U)) text += 3;
        first = false;
        while (isspace((unsigned char)*text)) text++;
        char *end = text + strlen(text);
        while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
        if (!*text) continue;
        if (!strncmp(text, "#EXT-X-", 7U)) { err = ESP_ERR_NOT_SUPPORTED; break; }
        if (!strncmp(text, "#EXTINF:", 8U)) {
            char *comma = strchr(text + 8U, ',');
            char *duration_end;
            errno = 0;
            double duration = strtod(text + 8U, &duration_end);
            if (!comma || duration_end != comma || errno ||
                !((duration == -1.0 || duration >= 0.0) && duration <= INT32_MAX)) {
                err = ESP_ERR_INVALID_ARG; break;
            }
            entry.duration = (int32_t)duration;
            /* Titles are display metadata; paths are never truncated. */
            const char *title = comma + 1U;
            size_t title_len = strnlen(title, sizeof(entry.title) - 1U);
            while (title_len && ((unsigned char)title[title_len] & 0xc0U) == 0x80U) title_len--;
            memcpy(entry.title, title, title_len);
            entry.title[title_len] = 0;
            continue;
        }
        if (*text == '#') continue;
        err = local_file_uri(&text);
        if (err != ESP_OK) break;
        if (!single_line(text, sizeof(entry.path))) { err = ESP_ERR_INVALID_SIZE; break; }
        if (strstr(text, "://")) {
            strlcpy(entry.path, text, sizeof(entry.path));
        } else {
            char directory[SOLAR_OS_STORAGE_PATH_MAX];
            strlcpy(directory, path, sizeof(directory));
            char *slash = strrchr(directory, '/');
            if (slash) slash[slash == directory ? 1 : 0] = 0;
            else strlcpy(directory, ".", sizeof(directory));
            err = solar_os_storage_resolve_path_at(directory, text, entry.path, sizeof(entry.path));
            if (err != ESP_OK) break;
        }
        if (!single_line(entry.title, sizeof(entry.title))) { err = ESP_ERR_INVALID_ARG; break; }
        err = accept(&entry, user);
        if (err != ESP_OK) break;
        memset(&entry, 0, sizeof(entry));
        entry.duration = -1;
    }
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t solar_os_m3u_write(const char *path, size_t count,
                             solar_os_m3u_get_fn get, void *user)
{
    if (!path || !get) return ESP_ERR_INVALID_ARG;
    char temporary[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_sibling_path(path, ".tmp", temporary, sizeof(temporary)) != ESP_OK ||
        solar_os_storage_sibling_path(path, ".bak", backup, sizeof(backup)) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(temporary, "wb");
    if (!file) return ESP_FAIL;
    esp_err_t err = fputs("#EXTM3U\n", file) < 0 ? ESP_FAIL : ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < count; i++) {
        solar_os_m3u_entry_t entry;
        if (!get(i, &entry, user) || !single_line(entry.path, sizeof(entry.path)) ||
            !entry.path[0] || entry.path[0] == '#' ||
            !single_line(entry.title, sizeof(entry.title)) || entry.duration < -1) {
            err = ESP_ERR_INVALID_ARG; break;
        }
        if (fprintf(file, "#EXTINF:%ld,%s\n%s\n", (long)entry.duration,
                    entry.title, entry.path) < 0) err = ESP_FAIL;
    }
    if (err == ESP_OK) err = solar_os_storage_sync_file(file);
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) err = solar_os_storage_replace_file(temporary, path, backup);
    if (err != ESP_OK) (void)solar_os_storage_remove(temporary);
    return err;
}
