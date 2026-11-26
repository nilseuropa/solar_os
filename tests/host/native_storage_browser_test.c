#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doc_test_support.h"
#include "solar_os.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_storage_browser_abi.h"
#include "solar_os_native_ui.h"

static const solar_os_app_t owner = {0};

const solar_os_app_t *solar_os_application_descriptor(void)
{
    return &owner;
}

esp_err_t solar_os_storage_normalize_path(const char *path, char *out, size_t capacity)
{
    if (!path || *path != '/' || !out) return ESP_ERR_INVALID_ARG;
    const size_t bytes = strlen(path) + 1;
    if (bytes > capacity) return ESP_ERR_INVALID_SIZE;
    memcpy(out, path, bytes);
    return ESP_OK;
}

static void write_file(const char *path)
{
    FILE *file = fopen(path, "wb");
    assert(file && fputc('x', file) == 'x' && !fclose(file));
}

static bool wav_filter(const char *name, void *user)
{
    unsigned *calls = user;
    ++*calls;
    const char *extension = strrchr(name, '.');
    return extension && !strcmp(extension, ".wav");
}

int main(void)
{
    const solar_os_native_storage_browser_api_v1_t *api = solar_os_native_ui_get_service(
        SOLAR_OS_NATIVE_STORAGE_BROWSER_SERVICE, SOLAR_OS_NATIVE_STORAGE_BROWSER_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    const char base[] = "/tmp/solaros-native-browser-XXXXXX";
    char root[sizeof(base)];
    memcpy(root, base, sizeof(root));
    assert(mkdtemp(root));
    char directory[sizeof(root) + 10], wave[sizeof(root) + 10], mpeg[sizeof(root) + 10];
    assert(snprintf(directory, sizeof(directory), "%s/Music", root) > 0);
    assert(snprintf(wave, sizeof(wave), "%s/Audio.wav", root) > 0);
    assert(snprintf(mpeg, sizeof(mpeg), "%s/Other.mp3", root) > 0);
    assert(!mkdir(directory, 0700));
    write_file(wave);
    write_file(mpeg);

    solar_os_native_handle_t browser;
    assert(!api->create(&browser) && browser);
    assert(api->refresh(browser, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    unsigned filter_calls = 0;
    assert(!api->open(browser, root, wav_filter, &filter_calls));
    assert(filter_calls == 2);
    solar_os_native_storage_browser_info_v1_t info;
    assert(!api->info(browser, &info) && !strcmp(info.path, root));
    assert(info.count == 3 && info.cursor == 0);
    solar_os_storage_browser_entry_t entry;
    assert(!api->entry(browser, 0, &entry) && entry.is_parent);
    assert(!api->entry(browser, 1, &entry) && entry.is_directory && !strcmp(entry.name, "Music"));
    assert(!api->entry(browser, 2, &entry) && !entry.is_directory && entry.size == 1);
    assert(api->entry(browser, 3, &entry) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_m3u_entry_t media;
    assert(!api->media_entry(browser, 1, &media) && !strcmp(media.path, "[DIR] Music"));
    assert(!api->move(browser, INT_MAX));
    char selected[sizeof(info.path)];
    bool file_selected;
    assert(!api->activate(browser, selected, sizeof(selected), &file_selected, NULL, NULL));
    assert(file_selected && !strcmp(wave, selected));
    assert(!api->select_directory(browser, selected, sizeof(selected)) && !strcmp(root, selected));

    const size_t used = doc_test_memory_used();
    doc_test_memory_fail_after(0);
    assert(api->refresh(browser, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    doc_test_memory_fail_after(-1);
    assert(doc_test_memory_used() == used);
    assert(!api->info(browser, &info) && info.cursor == 2 && info.count == 3);
    assert(!api->refresh(browser, NULL, NULL) && filter_calls == 2);
    assert(!api->info(browser, &info) && info.cursor == 0 && info.count == 4);
    assert(!api->move(browser, 1));
    assert(!api->activate(browser, selected, sizeof(selected), &file_selected, NULL, NULL));
    assert(!file_selected && !api->info(browser, &info) && !strcmp(directory, info.path));
    assert(!api->move(browser, INT_MIN));
    assert(!api->activate(browser, selected, sizeof(selected), &file_selected, wav_filter, &filter_calls));
    assert(!file_selected && !api->info(browser, &info) && !strcmp(root, info.path));

    assert(!solar_os_native_handle_close_owner(&owner));
    assert(api->close(browser) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!doc_test_memory_used());
    assert(!unlink(wave) && !unlink(mpeg) && !rmdir(directory) && !rmdir(root));
    puts("native storage browser: scoped filters, paths, navigation, rollback and cleanup passed");
    return 0;
}
