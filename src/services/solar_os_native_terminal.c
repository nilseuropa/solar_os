#include "solar_os_native_terminal.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_terminal.h"
#include "solar_os_terminal_geometry.h"
#include "solar_os_native_terminal_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_terminal_preferences.h"
static int terminal_set_status_bar_preference(bool visible)
{ return solar_os_native_result(solar_os_terminal_set_status_bar_preference(visible)); }
static int terminal_set_status_bar_visible(solar_os_terminal_t *terminal, bool visible)
{ return solar_os_native_result(solar_os_terminal_set_status_bar_visible(terminal, visible)); }
static int terminal_set_status_bar_visible_transient(solar_os_terminal_t *terminal, bool visible)
{ return solar_os_native_result(solar_os_terminal_set_status_bar_visible_transient(terminal, visible)); }
static int terminal_set_palette_preference(bool inverted)
{ return solar_os_native_result(solar_os_terminal_set_palette_preference(inverted)); }
static int terminal_set_palette_inverted(solar_os_terminal_t *terminal, bool inverted)
{ return solar_os_native_result(solar_os_terminal_set_palette_inverted(terminal, inverted)); }
static int terminal_set_palette_inverted_transient(solar_os_terminal_t *terminal, bool inverted)
{ return solar_os_native_result(solar_os_terminal_set_palette_inverted_transient(terminal, inverted)); }
static int terminal_set_cursor_preference(solar_os_terminal_cursor_shape_t shape, bool blink)
{ return solar_os_native_result(solar_os_terminal_set_cursor_preference(shape, blink)); }
static int terminal_add_vrule(solar_os_terminal_t *terminal, size_t row, size_t col, size_t height, uint8_t width, bool inverse)
{ return solar_os_native_result(solar_os_terminal_add_vrule(terminal, row, col, height, width, inverse)); }
static int terminal_set_orientation(solar_os_terminal_t *terminal, uint16_t degrees)
{ return solar_os_native_result(solar_os_terminal_set_orientation(terminal, degrees)); }
static int terminal_set_orientation_transient(solar_os_terminal_t *terminal, uint16_t degrees)
{ return solar_os_native_result(solar_os_terminal_set_orientation_transient(terminal, degrees)); }
static int terminal_set_font(solar_os_terminal_t *terminal, solar_os_terminal_font_t font)
{ return solar_os_native_result(solar_os_terminal_set_font(terminal, font)); }
static int terminal_set_font_transient(solar_os_terminal_t *terminal, solar_os_terminal_font_t font)
{ return solar_os_native_result(solar_os_terminal_set_font_transient(terminal, font)); }
static int terminal_set_text_size(solar_os_terminal_t *terminal, solar_os_terminal_text_size_t text_size)
{ return solar_os_native_result(solar_os_terminal_set_text_size(terminal, text_size)); }
static int terminal_set_text_size_transient(solar_os_terminal_t *terminal, solar_os_terminal_text_size_t text_size)
{ return solar_os_native_result(solar_os_terminal_set_text_size_transient(terminal, text_size)); }
static int terminal_apply_profile_transient(solar_os_terminal_t *terminal, const solar_os_terminal_profile_t *profile)
{ return solar_os_native_result(solar_os_terminal_apply_profile_transient(terminal, profile)); }

static int terminal_load_display_preferences(const char *display, uint16_t fallback,
    solar_os_native_terminal_display_preferences_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_terminal_display_preferences_v1_t){0};
    if (!display || !*display || strlen(display) > 15 || fallback >= 360 || fallback % 90)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    out->orientation_degrees = solar_os_terminal_profile_load_display_orientation(display, fallback);
    out->scale = solar_os_terminal_profile_load_scale(display);
    return SOLAR_OS_NATIVE_OK;
}

static int terminal_save_preference(const char *display, uint32_t field, uint32_t value)
{
    if (value > UINT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (display) {
        if (field == SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_ORIENTATION)
            return solar_os_native_result(solar_os_terminal_profile_save_display_orientation(display, (uint16_t)value));
        if (field == SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_SCALE && value <= UINT8_MAX)
            return solar_os_native_result(solar_os_terminal_profile_save_scale(display, (uint8_t)value));
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    switch (field) {
    case SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_ORIENTATION:
        return solar_os_native_result(solar_os_terminal_profile_save_orientation((uint16_t)value));
    case SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_FONT:
        return solar_os_native_result(solar_os_terminal_profile_save_font((solar_os_terminal_font_t)value));
    case SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_TEXT_SIZE:
        return solar_os_native_result(solar_os_terminal_profile_save_text_size((solar_os_terminal_text_size_t)value));
    case SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE:
        if (value <= 1) return solar_os_native_result(solar_os_terminal_profile_save_palette(value));
        break;
    case SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_STATUS_BAR:
        if (value <= 1) return solar_os_native_result(solar_os_terminal_profile_save_status_bar(value));
        break;
    }
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}

static const solar_os_native_terminal_api_v1_t terminal_api = {
    .abi_version = SOLAR_OS_NATIVE_TERMINAL_ABI, .struct_size = sizeof(terminal_api),
    .clear = solar_os_terminal_clear,
    .newline = solar_os_terminal_newline,
    .backspace = solar_os_terminal_backspace,
    .put_printable = solar_os_terminal_put_printable,
    .put_codepoint = solar_os_terminal_put_codepoint,
    .put_char = solar_os_terminal_put_char,
    .put_utf8_byte = solar_os_terminal_put_utf8_byte,
    .write_utf8 = solar_os_terminal_write_utf8,
    .utf8_reset = solar_os_terminal_utf8_reset,
    .write = solar_os_terminal_write,
    .writeln = solar_os_terminal_writeln,
    .printf = solar_os_terminal_printf,
    .set_bold = solar_os_terminal_set_bold,
    .bold = solar_os_terminal_bold,
    .set_italic = solar_os_terminal_set_italic,
    .italic = solar_os_terminal_italic,
    .set_underline = solar_os_terminal_set_underline,
    .underline = solar_os_terminal_underline,
    .set_inverse = solar_os_terminal_set_inverse,
    .inverse = solar_os_terminal_inverse,
    .write_bold = solar_os_terminal_write_bold,
    .writeln_bold = solar_os_terminal_writeln_bold,
    .printf_bold = solar_os_terminal_printf_bold,
    .set_status_bar = solar_os_terminal_set_status_bar,
    .get_status_bar = solar_os_terminal_get_status_bar,
    .status_bar_preference_visible = solar_os_terminal_status_bar_preference_visible,
    .set_status_bar_preference = terminal_set_status_bar_preference,
    .status_bar_visible = solar_os_terminal_status_bar_visible,
    .set_status_bar_visible = terminal_set_status_bar_visible,
    .set_status_bar_visible_transient = terminal_set_status_bar_visible_transient,
    .set_footer = solar_os_terminal_set_footer,
    .set_black_is_one = solar_os_terminal_set_black_is_one,
    .palette_preference_inverted = solar_os_terminal_palette_preference_inverted,
    .set_palette_preference = terminal_set_palette_preference,
    .palette_inverted = solar_os_terminal_palette_inverted,
    .set_palette_inverted = terminal_set_palette_inverted,
    .set_palette_inverted_transient = terminal_set_palette_inverted_transient,
    .cursor_row = solar_os_terminal_cursor_row,
    .cursor_col = solar_os_terminal_cursor_col,
    .rows = solar_os_terminal_rows,
    .cols = solar_os_terminal_cols,
    .set_cursor = solar_os_terminal_set_cursor,
    .set_cursor_visible = solar_os_terminal_set_cursor_visible,
    .cursor_visible = solar_os_terminal_cursor_visible,
    .get_cursor_preference = solar_os_terminal_get_cursor_preference,
    .set_cursor_preference = terminal_set_cursor_preference,
    .cursor_shape_name = solar_os_terminal_cursor_shape_name,
    .parse_cursor_shape = solar_os_terminal_parse_cursor_shape,
    .clear_primitives = solar_os_terminal_clear_primitives,
    .add_vrule = terminal_add_vrule,
    .clear_line_from = solar_os_terminal_clear_line_from,
    .page_up = solar_os_terminal_page_up,
    .page_down = solar_os_terminal_page_down,
    .scroll_to_live = solar_os_terminal_scroll_to_live,
    .is_scrolled_back = solar_os_terminal_is_scrolled_back,
    .orientation = solar_os_terminal_orientation,
    .set_orientation = terminal_set_orientation,
    .set_orientation_transient = terminal_set_orientation_transient,
    .font = solar_os_terminal_font,
    .set_font = terminal_set_font,
    .set_font_transient = terminal_set_font_transient,
    .font_name = solar_os_terminal_font_name,
    .parse_font = solar_os_terminal_parse_font,
    .text_size = solar_os_terminal_text_size,
    .set_text_size = terminal_set_text_size,
    .set_text_size_transient = terminal_set_text_size_transient,
    .text_size_name = solar_os_terminal_text_size_name,
    .parse_text_size = solar_os_terminal_parse_text_size,
    .get_profile = solar_os_terminal_get_profile,
    .apply_profile_transient = terminal_apply_profile_transient,
    .needs_draw = solar_os_terminal_needs_draw,
    .invalidate_render = solar_os_terminal_invalidate_render,
    .draw = solar_os_terminal_draw,
    .columns_for_width = solar_os_terminal_columns_compute,
    .geometry_for_height = solar_os_terminal_geometry_compute,
    .profile_valid = solar_os_terminal_profile_is_valid,
    .load_saved_profile = solar_os_terminal_profile_load_preferences,
    .load_display_preferences = terminal_load_display_preferences,
    .save_preference = terminal_save_preference,
    .cursor_preference_for_panel = solar_os_terminal_get_cursor_preference_for_panel,
};
#endif

const void *solar_os_native_terminal_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_terminal_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
