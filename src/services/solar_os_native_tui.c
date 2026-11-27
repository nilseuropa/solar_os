#include "solar_os_native_tui.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_tui_widgets.h"
#include "solar_os_native_tui_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
static solar_os_tui_t *tui_acquire(solar_os_native_handle_t id)
{ return solar_os_native_handle_acquire_owned(id, SOLAR_OS_NATIVE_HANDLE_TUI, solar_os_application_descriptor()); }
static int tui_destroy(void *state)
{
    solar_os_tui_end(state);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}
static int tui_create(bool screen, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    solar_os_context_t *ctx = solar_os_application_context();
    if (!owner || !ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = solar_os_memory_calloc(1, sizeof(*tui), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.tui");
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = solar_os_native_result(screen ? solar_os_tui_screen_begin(tui, ctx) : solar_os_tui_begin(tui, ctx));
    if (result) { solar_os_memory_free(tui); return result; }
    result = solar_os_native_handle_create_owned(SOLAR_OS_NATIVE_HANDLE_TUI, tui, tui_destroy, owner, out);
    if (result) tui_destroy(tui);
    return result;
}
static int tui_begin(solar_os_native_handle_t *out) { return tui_create(false, out); }
static int tui_screen_begin(solar_os_native_handle_t *out) { return tui_create(true, out); }
static int tui_end(solar_os_native_handle_t id)
{
    const solar_os_app_t *owner = solar_os_application_descriptor();
    return owner ? solar_os_native_handle_close_owned(id, SOLAR_OS_NATIVE_HANDLE_TUI, owner) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}
static int tui_attach_session(solar_os_native_handle_t id)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_tui_attach_session(tui);
    const int result = SOLAR_OS_NATIVE_OK;
    solar_os_native_handle_release(id);
    return result;
}
static int tui_enable_diff(solar_os_native_handle_t id, bool enabled)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_enable_diff(tui, enabled));
    solar_os_native_handle_release(id);
    return result;
}
static size_t tui_rows(solar_os_native_handle_t id)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const size_t result = solar_os_tui_rows(tui);
    solar_os_native_handle_release(id);
    return result;
}
static size_t tui_cols(solar_os_native_handle_t id)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const size_t result = solar_os_tui_cols(tui);
    solar_os_native_handle_release(id);
    return result;
}
static int tui_clear(solar_os_native_handle_t id)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_tui_clear(tui);
    const int result = SOLAR_OS_NATIVE_OK;
    solar_os_native_handle_release(id);
    return result;
}
static int tui_refresh(solar_os_native_handle_t id)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_tui_refresh(tui);
    const int result = SOLAR_OS_NATIVE_OK;
    solar_os_native_handle_release(id);
    return result;
}
static int tui_move(solar_os_native_handle_t id, size_t row, size_t col)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_move(tui, row, col));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_set_cursor_visible(solar_os_native_handle_t id, bool visible)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_set_cursor_visible(tui, visible));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_write(solar_os_native_handle_t id, const char *text, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_write(tui, text, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_addstr(solar_os_native_handle_t id, size_t row, size_t col, const char *text, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_addstr(tui, row, col, text, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_putch(solar_os_native_handle_t id, size_t row, size_t col, uint32_t codepoint, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_putch(tui, row, col, codepoint, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_hline(solar_os_native_handle_t id, size_t row, size_t col, size_t width, uint32_t codepoint, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_hline(tui, row, col, width, codepoint, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_vline(solar_os_native_handle_t id, size_t row, size_t col, size_t height, uint32_t codepoint, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_vline(tui, row, col, height, codepoint, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_vrule(solar_os_native_handle_t id, size_t row, size_t col, size_t height, uint8_t width, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_vrule(tui, row, col, height, width, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_box(solar_os_native_handle_t id, size_t row, size_t col, size_t height, size_t width, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_box(tui, row, col, height, width, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_fill(solar_os_native_handle_t id, size_t row, size_t col, size_t height, size_t width, uint32_t codepoint, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_fill(tui, row, col, height, width, codepoint, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_text_popup(solar_os_native_handle_t id, const solar_os_tui_rect_t *bounds, const char *title, const char *text, solar_os_tui_rect_t *popup)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_text_popup(tui, bounds, title, text, popup));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_progress_bar(solar_os_native_handle_t id, size_t row, size_t col, size_t width, const char *label, uint64_t value, uint64_t total, bool total_known)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_progress_bar(tui, row, col, width, label, value, total, total_known));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_form(solar_os_native_handle_t id, const solar_os_tui_form_t *form)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_form(tui, form));
    solar_os_native_handle_release(id);
    return result;
}
static bool tui_screen_fullscreen(solar_os_native_handle_t id)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const bool result = solar_os_tui_screen_fullscreen(tui);
    solar_os_native_handle_release(id);
    return result;
}
static size_t tui_screen_bottom_rows(solar_os_native_handle_t id, size_t normal_rows)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const size_t result = solar_os_tui_screen_bottom_rows(tui, normal_rows);
    solar_os_native_handle_release(id);
    return result;
}
static size_t tui_screen_content_rows(solar_os_native_handle_t id, size_t top_rows, size_t normal_bottom_rows)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const size_t result = solar_os_tui_screen_content_rows(tui, top_rows, normal_bottom_rows);
    solar_os_native_handle_release(id);
    return result;
}
static size_t tui_screen_content_end(solar_os_native_handle_t id, size_t normal_bottom_rows)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const size_t result = solar_os_tui_screen_content_end(tui, normal_bottom_rows);
    solar_os_native_handle_release(id);
    return result;
}
static solar_os_tui_screen_key_action_t tui_screen_key(solar_os_native_handle_t id, uint8_t key)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const solar_os_tui_screen_key_action_t result = solar_os_tui_screen_key(tui, key);
    solar_os_native_handle_release(id);
    return result;
}
static bool tui_screen_layout(solar_os_native_handle_t id, size_t tab_rows, size_t status_rows, size_t input_rows, solar_os_tui_screen_layout_t *layout)
{
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return 0;
    const bool result = solar_os_tui_screen_layout(tui, tab_rows, status_rows, input_rows, layout);
    solar_os_native_handle_release(id);
    return result;
}
static int tui_write_cell(solar_os_native_handle_t id, size_t row, size_t col, size_t width, const char *text, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_write_cell(tui, row, col, width, text, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_title(solar_os_native_handle_t id, const char *title, const char *detail)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_title(tui, title, detail));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_help(solar_os_native_handle_t id, const char *text)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_help(tui, text));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_footer(solar_os_native_handle_t id, const char *status, const char *help)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_footer(tui, status, help));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_tab(solar_os_native_handle_t id, size_t row, size_t col, size_t width, const char *text, bool selected)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_tab(tui, row, col, width, text, selected));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_too_small(solar_os_native_handle_t id, const char *name)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_too_small(tui, name));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_input(solar_os_native_handle_t id, size_t row, size_t col, size_t width, const char *label, const char *text, solar_os_tui_input_state_t *state, uint8_t attr)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_input(tui, row, col, width, label, text, state, attr));
    solar_os_native_handle_release(id);
    return result;
}
static int tui_draw_input_ex(solar_os_native_handle_t id, size_t row, size_t col, size_t width, const char *label, const char *text, solar_os_tui_input_state_t *state, uint8_t attr, bool masked)
{
    if (!solar_os_application_descriptor()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_tui_t *tui = tui_acquire(id);
    if (!tui) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_tui_draw_input_ex(tui, row, col, width, label, text, state, attr, masked));
    solar_os_native_handle_release(id);
    return result;
}

static const solar_os_native_tui_api_v1_t tui_api = {
    .abi_version = SOLAR_OS_NATIVE_TUI_ABI, .struct_size = sizeof(tui_api),
    .begin = tui_begin,
    .attach_session = tui_attach_session,
    .end = tui_end,
    .enable_diff = tui_enable_diff,
    .rows = tui_rows,
    .cols = tui_cols,
    .clear = tui_clear,
    .refresh = tui_refresh,
    .move = tui_move,
    .set_cursor_visible = tui_set_cursor_visible,
    .write = tui_write,
    .addstr = tui_addstr,
    .putch = tui_putch,
    .hline = tui_hline,
    .vline = tui_vline,
    .vrule = tui_vrule,
    .box = tui_box,
    .fill = tui_fill,
    .text_popup = tui_text_popup,
    .progress_bar = tui_progress_bar,
    .draw_form = tui_draw_form,
    .screen_begin = tui_screen_begin,
    .screen_should_fullscreen = solar_os_tui_screen_should_fullscreen,
    .screen_fullscreen = tui_screen_fullscreen,
    .screen_bottom_rows = tui_screen_bottom_rows,
    .screen_content_rows = tui_screen_content_rows,
    .screen_content_end = tui_screen_content_end,
    .screen_key = tui_screen_key,
    .screen_layout = tui_screen_layout,
    .layout_compute = solar_os_tui_layout_compute,
    .layout_compute_fullscreen = solar_os_tui_layout_compute_fullscreen,
    .write_cell = tui_write_cell,
    .draw_title = tui_draw_title,
    .draw_help = tui_draw_help,
    .draw_footer = tui_draw_footer,
    .draw_tab = tui_draw_tab,
    .draw_too_small = tui_draw_too_small,
    .viewport_reconcile = solar_os_tui_viewport_reconcile,
    .viewport_key = solar_os_tui_viewport_key,
    .input_key = solar_os_tui_input_key,
    .input_text = solar_os_tui_input_text,
    .draw_input = tui_draw_input,
    .draw_input_ex = tui_draw_input_ex,
};
#endif
const void *solar_os_native_tui_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_tui_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
