#include "solar_os_native_m3u.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_m3u_abi.h"
#if SOLAR_OS_PACKAGE_SERVICE_M3U
#include "solar_os_m3u.h"
#include "solar_os_native_support.h"
static int m3u_read(const char *path, solar_os_native_m3u_accept_fn accept, void *user)
{
    /* Core propagates callback errors. Native errors are negative; Core's
     * own errors are positive except ESP_FAIL, identical to native FAILED. */
    const int result = solar_os_m3u_read(path, accept, user);
    return result < 0 ? result : solar_os_native_result(result);
}
static int m3u_write(const char *path, size_t count, solar_os_m3u_get_fn get, void *user)
{ return solar_os_native_result(solar_os_m3u_write(path, count, get, user)); }
static const solar_os_native_m3u_api_v1_t m3u_api = {
    .abi_version = SOLAR_OS_NATIVE_M3U_ABI, .struct_size = sizeof(m3u_api),
    .path = solar_os_m3u_path, .read = m3u_read, .write = m3u_write,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MEDIA_PLAYLIST
#include "solar_os.h"
#include "solar_os_media_playlist.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"

#define PLAYLIST_KIND SOLAR_OS_NATIVE_HANDLE_MEDIA_PLAYLIST

static int playlist_destroy(void *state)
{
    solar_os_media_playlist_close(state);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int playlist_open(solar_os_media_playlist_kind_t kind, const char *source,
    solar_os_native_handle_t *out, size_t *selected)
{
    if (selected) *selected = 0;
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_media_playlist_t *list = solar_os_memory_calloc(1, sizeof(*list),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.playlist");
    if (!list) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    solar_os_native_handle_t id;
    int result = owner ? solar_os_native_handle_create_owned(PLAYLIST_KIND, list, playlist_destroy, owner, &id) :
        solar_os_native_handle_create(PLAYLIST_KIND, list, playlist_destroy, &id);
    if (result) {
        (void)playlist_destroy(list);
        return result;
    }
    /* Reserve the handle before an import or add can change a saved list. */
    void *state;
    result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result == SOLAR_OS_NATIVE_OK) {
        result = solar_os_native_result(solar_os_media_playlist_open(state, kind, source, selected));
        solar_os_native_handle_release_exclusive(id);
    }
    if (result) (void)solar_os_native_handle_close_kind(id, PLAYLIST_KIND);
    else *out = id;
    return result;
}

static int playlist_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, PLAYLIST_KIND); }

static int playlist_info(solar_os_native_handle_t id, size_t *count, uint32_t *revision)
{
    if (!count) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0;
    if (revision) *revision = 0;
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    const solar_os_media_playlist_t *list = state;
    *count = list->count;
    if (revision) *revision = list->revision;
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static int playlist_get(solar_os_native_handle_t id, size_t index, solar_os_m3u_entry_t *entry)
{
    if (!entry) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(entry, 0, sizeof(*entry));
    void *state;
    int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    const solar_os_media_playlist_t *list = state;
    if (index < list->count) *entry = list->entries[index];
    else result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int playlist_refresh(solar_os_native_handle_t id)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    const esp_err_t err = solar_os_media_playlist_refresh(state);
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(err);
}

static int playlist_import(solar_os_native_handle_t id, const char *path)
{
    if (!path || !path[0]) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    char resolved[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_resolve_path(path, resolved, sizeof(resolved));
    if (err != ESP_OK) return solar_os_native_result(err);
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    err = solar_os_media_playlist_import(state, resolved);
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(err);
}

static int playlist_add(solar_os_native_handle_t id, const char *source, size_t *index)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    const esp_err_t err = solar_os_media_playlist_add(state, source, index);
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(err);
}

static int playlist_set(solar_os_native_handle_t id, size_t index, const solar_os_m3u_entry_t *entry)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    const esp_err_t err = solar_os_media_playlist_set(state, index, entry);
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(err);
}

static int playlist_remove(solar_os_native_handle_t id, size_t index)
{
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, PLAYLIST_KIND, &state);
    if (result) return result;
    const esp_err_t err = solar_os_media_playlist_remove(state, index);
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(err);
}

static const solar_os_native_playlist_api_v1_t playlist_api = {
    .abi_version = SOLAR_OS_NATIVE_PLAYLIST_ABI, .struct_size = sizeof(playlist_api),
    .open = playlist_open, .close = playlist_close, .info = playlist_info, .get = playlist_get,
    .refresh = playlist_refresh, .import_m3u = playlist_import, .add = playlist_add,
    .set = playlist_set, .remove = playlist_remove,
};
#endif

const void *solar_os_native_m3u_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_m3u_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
