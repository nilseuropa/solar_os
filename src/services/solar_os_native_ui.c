#include "solar_os_native_ui.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_ui_abi.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_gfx_widgets.h"
#include "solar_os_tui_widgets.h"
static const solar_os_native_ui_api_v1_t ui_api = {
    .abi_version = SOLAR_OS_NATIVE_UI_ABI, .struct_size = sizeof(ui_api),
    .label = solar_os_gfx_label, .centered_label = solar_os_gfx_centered_label,
    .input = solar_os_gfx_input, .dialog = solar_os_gfx_dialog,
    .input_key = solar_os_tui_input_key, .input_text = solar_os_tui_input_text,
    .viewport_reconcile = solar_os_tui_viewport_reconcile, .viewport_key = solar_os_tui_viewport_key,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_TEXT_SEARCH
#include "solar_os_text_search.h"
static const solar_os_native_text_search_api_v1_t text_search_api = {
    .abi_version = SOLAR_OS_NATIVE_TEXT_SEARCH_ABI,
    .struct_size = sizeof(text_search_api),
    .reset = solar_os_text_search_reset,
    .begin_input = solar_os_text_search_begin_input,
    .cancel_input = solar_os_text_search_cancel_input,
    .input_append = solar_os_text_search_input_append,
    .input_backspace = solar_os_text_search_input_backspace,
    .submit_input = solar_os_text_search_submit_input,
    .set_query = solar_os_text_search_set_query,
    .find = solar_os_text_search_find,
    .find_segments = solar_os_text_search_find_segments,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UNISTROKE
#include "solar_os_unistroke.h"
#if SOLAR_OS_PACKAGE_SERVICE_GRAFFITI
#include "solar_os_graffiti.h"
#endif
static const solar_os_native_gesture_api_v1_t gesture_api = {
    .abi_version = SOLAR_OS_NATIVE_GESTURE_ABI,
    .struct_size = sizeof(gesture_api),
    .defaults = solar_os_unistroke_default_config,
    .prepare = solar_os_unistroke_prepare,
    .recognize = solar_os_unistroke_recognize,
#if SOLAR_OS_PACKAGE_SERVICE_GRAFFITI
    .graffiti_context_size = solar_os_graffiti_context_size,
    .graffiti_context_alignment = solar_os_graffiti_context_alignment,
    .graffiti_init = solar_os_graffiti_init,
    .graffiti_template_count = solar_os_graffiti_template_count,
    .graffiti_zone = solar_os_graffiti_zone_for_start,
    .graffiti_recognize = solar_os_graffiti_recognize,
    .graffiti_apply = solar_os_graffiti_apply,
#endif
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_STORAGE_BROWSER
#include "solar_os.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_storage_browser.h"

static int browser_destroy(void *state)
{
    solar_os_storage_browser_destroy(state);
    return SOLAR_OS_NATIVE_OK;
}

static int browser_create(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_storage_browser_t *browser;
    const esp_err_t error = solar_os_storage_browser_create(NULL, NULL, &browser);
    if (error) return solar_os_native_result(error);
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_STORAGE_BROWSER;
    const int result = owner ? solar_os_native_handle_create_owned(kind, browser, browser_destroy, owner, out) :
        solar_os_native_handle_create(kind, browser, browser_destroy, out);
    if (result) browser_destroy(browser);
    return result;
}

static int browser_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_STORAGE_BROWSER);
}

static solar_os_storage_browser_t *browser_acquire(solar_os_native_handle_t id)
{
    return solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_STORAGE_BROWSER);
}

static int browser_scan(solar_os_native_handle_t id, const char *path,
                        solar_os_storage_browser_filter_t filter, void *user)
{
    solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const char *directory = path ? path : solar_os_storage_browser_path(browser);
    const esp_err_t error = *directory ? solar_os_storage_browser_open_filtered(browser, directory, filter, user) :
        ESP_ERR_INVALID_STATE;
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int browser_open(solar_os_native_handle_t id, const char *path,
                        solar_os_storage_browser_filter_t filter, void *user)
{
    if (!path || !*path) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return browser_scan(id, path, filter, user);
}

static int browser_refresh(solar_os_native_handle_t id, solar_os_storage_browser_filter_t filter, void *user)
{
    return browser_scan(id, NULL, filter, user);
}

static int browser_info(solar_os_native_handle_t id, solar_os_native_storage_browser_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = (solar_os_native_storage_browser_info_v1_t){
        .count = solar_os_storage_browser_count(browser), .cursor = solar_os_storage_browser_cursor(browser),
    };
    _Static_assert(sizeof(out->path) == SOLAR_OS_STORAGE_PATH_MAX, "storage browser path capacity");
    const char *path = solar_os_storage_browser_path(browser);
    memcpy(out->path, path, strlen(path) + 1U);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int browser_entry(solar_os_native_handle_t id, size_t index, solar_os_storage_browser_entry_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const bool found = solar_os_storage_browser_entry(browser, index, out);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int browser_move(solar_os_native_handle_t id, int offset)
{
    solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_storage_browser_move(browser, offset);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int browser_activate(solar_os_native_handle_t id, char *selected, size_t capacity, bool *file_selected,
                            solar_os_storage_browser_filter_t filter, void *user)
{
    if (!selected || !capacity || !file_selected) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *selected = '\0';
    *file_selected = false;
    solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_storage_browser_activate_filtered(
        browser, selected, capacity, file_selected, filter, user);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int browser_select_directory(solar_os_native_handle_t id, char *selected, size_t capacity)
{
    const solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_storage_browser_select_directory(browser, selected, capacity);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int browser_media_entry(solar_os_native_handle_t id, size_t index, solar_os_m3u_entry_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_storage_browser_t *browser = browser_acquire(id);
    if (!browser) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const bool found = solar_os_storage_browser_media_entry(index, out, browser);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static const solar_os_native_storage_browser_api_v1_t storage_browser_api = {
    .abi_version = SOLAR_OS_NATIVE_STORAGE_BROWSER_ABI,
    .struct_size = sizeof(storage_browser_api),
    .create = browser_create,
    .close = browser_close,
    .open = browser_open,
    .refresh = browser_refresh,
    .info = browser_info,
    .entry = browser_entry,
    .move = browser_move,
    .activate = browser_activate,
    .select_directory = browser_select_directory,
    .media_entry = browser_media_entry,
};
#endif

const void *solar_os_native_ui_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ui_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
