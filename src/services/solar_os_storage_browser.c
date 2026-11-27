#include "solar_os_storage_browser.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_memory.h"

#define STORAGE_BROWSER_INITIAL_CAPACITY 24U

bool solar_os_storage_browser_media_entry(size_t index, solar_os_m3u_entry_t *entry, void *browser)
{
    solar_os_storage_browser_entry_t item;
    if (!entry || !solar_os_storage_browser_entry(browser, index, &item)) return false;
    memset(entry, 0, sizeof(*entry));
    entry->duration = -1;
    snprintf(entry->path, sizeof(entry->path), "%s%s", item.is_directory ? "[DIR] " : "", item.name);
    return true;
}

struct solar_os_storage_browser {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_storage_browser_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t cursor;
    solar_os_storage_browser_filter_t filter;
    void *filter_user;
};

static int browser_name_compare(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const int ca = tolower((unsigned char)*a++);
        const int cb = tolower((unsigned char)*b++);
        if (ca != cb) {
            return ca - cb;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int browser_entry_compare(const void *a, const void *b)
{
    const solar_os_storage_browser_entry_t *left = a;
    const solar_os_storage_browser_entry_t *right = b;
    if (left->is_parent != right->is_parent) {
        return left->is_parent ? -1 : 1;
    }
    if (left->is_directory != right->is_directory) {
        return left->is_directory ? -1 : 1;
    }
    return browser_name_compare(left->name, right->name);
}

static esp_err_t browser_reserve(solar_os_storage_browser_t *browser,
                                 size_t needed)
{
    if (needed <= browser->capacity) {
        return ESP_OK;
    }
    size_t capacity = browser->capacity != 0U ? browser->capacity :
        STORAGE_BROWSER_INITIAL_CAPACITY;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U / sizeof(browser->entries[0])) {
            return ESP_ERR_INVALID_SIZE;
        }
        capacity *= 2U;
    }
    void *entries = solar_os_memory_realloc(
        browser->entries,
        capacity * sizeof(browser->entries[0]),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "storage.browser.entries");
    if (entries == NULL) {
        return ESP_ERR_NO_MEM;
    }
    browser->entries = entries;
    browser->capacity = capacity;
    return ESP_OK;
}

static esp_err_t browser_add(solar_os_storage_browser_t *browser,
                             const char *name,
                             bool directory,
                             bool parent,
                             uint64_t size)
{
    esp_err_t err = browser_reserve(browser, browser->count + 1U);
    if (err != ESP_OK) {
        return err;
    }
    solar_os_storage_browser_entry_t *entry =
        &browser->entries[browser->count++];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->name, name, sizeof(entry->name));
    entry->is_directory = directory;
    entry->is_parent = parent;
    entry->size = size;
    return ESP_OK;
}

static bool browser_join(const char *directory,
                         const char *name,
                         char *path,
                         size_t path_len)
{
    const int written = strcmp(directory, "/") == 0 ?
        snprintf(path, path_len, "/%s", name) :
        snprintf(path, path_len, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < path_len;
}

static void browser_parent(const char *path, char *parent, size_t parent_len)
{
    strlcpy(parent, path, parent_len);
    size_t length = strlen(parent);
    while (length > 1U && parent[length - 1U] == '/') {
        parent[--length] = '\0';
    }
    while (length > 1U && parent[length - 1U] != '/') {
        parent[--length] = '\0';
    }
    while (length > 1U && parent[length - 1U] == '/') {
        parent[--length] = '\0';
    }
}

esp_err_t solar_os_storage_browser_create(
    solar_os_storage_browser_filter_t filter,
    void *filter_user,
    solar_os_storage_browser_t **browser_out)
{
    if (browser_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *browser_out = NULL;
    solar_os_storage_browser_t *browser = solar_os_memory_alloc(
        sizeof(*browser), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "storage.browser");
    if (browser == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(browser, 0, sizeof(*browser));
    browser->filter = filter;
    browser->filter_user = filter_user;
    *browser_out = browser;
    return ESP_OK;
}

void solar_os_storage_browser_destroy(solar_os_storage_browser_t *browser)
{
    if (browser != NULL) {
        solar_os_memory_free(browser->entries);
        solar_os_memory_free(browser);
    }
}

esp_err_t solar_os_storage_browser_open_filtered(solar_os_storage_browser_t *browser,
    const char *path, solar_os_storage_browser_filter_t filter, void *user)
{
    if (browser == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_storage_browser_t next = {0};
    esp_err_t err = solar_os_storage_normalize_path(path, next.path, sizeof(next.path));
    if (err != ESP_OK) {
        return err;
    }
    DIR *directory = opendir(next.path);
    if (directory == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (strcmp(next.path, "/") != 0) {
        err = browser_add(&next, "..", true, true, 0U);
    }
    while (err == ESP_OK) {
        errno = 0;
        const struct dirent *item = readdir(directory);
        if (!item) {
            if (errno) err = ESP_FAIL;
            break;
        }
        if (item->d_name[0] == '.' || strlen(item->d_name) >= SOLAR_OS_STORAGE_BROWSER_NAME_MAX) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        struct stat info;
        if (!browser_join(next.path, item->d_name, child, sizeof(child)) ||
            stat(child, &info) != 0) {
            continue;
        }
        const bool is_directory = S_ISDIR(info.st_mode);
        if (!is_directory && filter && !filter(item->d_name, user)) {
            continue;
        }
        err = browser_add(&next, item->d_name, is_directory, false,
                          (uint64_t)info.st_size);
    }
    if (closedir(directory) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err != ESP_OK) {
        solar_os_memory_free(next.entries);
        return err;
    }
    if (next.count > 1U)
        qsort(next.entries, next.count, sizeof(next.entries[0]), browser_entry_compare);
    next.filter = browser->filter;
    next.filter_user = browser->filter_user;
    solar_os_memory_free(browser->entries);
    *browser = next;
    return ESP_OK;
}

esp_err_t solar_os_storage_browser_open(solar_os_storage_browser_t *browser, const char *path)
{
    if (!browser) return ESP_ERR_INVALID_ARG;
    return solar_os_storage_browser_open_filtered(browser, path, browser->filter, browser->filter_user);
}

esp_err_t solar_os_storage_browser_refresh(solar_os_storage_browser_t *browser)
{
    return browser != NULL ? solar_os_storage_browser_open(browser, browser->path) :
        ESP_ERR_INVALID_ARG;
}

const char *solar_os_storage_browser_path(const solar_os_storage_browser_t *browser)
{
    return browser != NULL ? browser->path : "";
}

size_t solar_os_storage_browser_count(const solar_os_storage_browser_t *browser)
{
    return browser != NULL ? browser->count : 0U;
}

size_t solar_os_storage_browser_cursor(const solar_os_storage_browser_t *browser)
{
    return browser != NULL ? browser->cursor : 0U;
}

bool solar_os_storage_browser_entry(const solar_os_storage_browser_t *browser,
                                    size_t index,
                                    solar_os_storage_browser_entry_t *entry)
{
    if (browser == NULL || entry == NULL || index >= browser->count) {
        return false;
    }
    *entry = browser->entries[index];
    return true;
}

void solar_os_storage_browser_move(solar_os_storage_browser_t *browser,
                                   int offset)
{
    if (browser == NULL || browser->count == 0U) {
        return;
    }
    if (offset < 0) {
        const size_t distance = (size_t)(-(offset + 1)) + 1U;
        browser->cursor -= distance < browser->cursor ? distance : browser->cursor;
    } else {
        const size_t remaining = browser->count - browser->cursor - 1U;
        const size_t distance = (size_t)offset;
        browser->cursor += distance < remaining ? distance : remaining;
    }
}

esp_err_t solar_os_storage_browser_activate_filtered(solar_os_storage_browser_t *browser,
    char *selected, size_t selected_len, bool *file_selected,
    solar_os_storage_browser_filter_t filter, void *user)
{
    if (browser == NULL || selected == NULL || selected_len == 0U ||
        file_selected == NULL || browser->cursor >= browser->count) {
        return ESP_ERR_INVALID_ARG;
    }
    *file_selected = false;
    const solar_os_storage_browser_entry_t *entry =
        &browser->entries[browser->cursor];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (entry->is_parent) {
        browser_parent(browser->path, path, sizeof(path));
    } else if (!browser_join(browser->path, entry->name, path, sizeof(path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (entry->is_directory) {
        return solar_os_storage_browser_open_filtered(browser, path, filter, user);
    }
    esp_err_t err = solar_os_storage_normalize_path(path, selected, selected_len);
    if (err == ESP_OK) {
        *file_selected = true;
    }
    return err;
}

esp_err_t solar_os_storage_browser_activate(solar_os_storage_browser_t *browser,
    char *selected, size_t selected_len, bool *file_selected)
{
    if (!browser) return ESP_ERR_INVALID_ARG;
    return solar_os_storage_browser_activate_filtered(
        browser, selected, selected_len, file_selected, browser->filter, browser->filter_user);
}

esp_err_t solar_os_storage_browser_select_directory(
    const solar_os_storage_browser_t *browser,
    char *selected,
    size_t selected_len)
{
    if (browser == NULL || selected == NULL || selected_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_storage_normalize_path(browser->path, selected, selected_len);
}
