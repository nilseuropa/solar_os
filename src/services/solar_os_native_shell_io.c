#include "solar_os_native_shell_io.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os.h"
#include "solar_os_shell_io.h"
#include "solar_os_native_shell_io_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
static solar_os_shell_io_t *shell_io_current(void)
{
    return solar_os_application_descriptor() ?
        solar_os_context_shell_io(solar_os_application_context()) : NULL;
}
static int shell_io_write(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_write(io, text)); }
static int shell_io_write_len(solar_os_shell_io_t *io, const char *text, size_t len)
{ return solar_os_native_result(solar_os_shell_io_write_len(io, text, len)); }
static int shell_io_write_raw(solar_os_shell_io_t *io, const char *data, size_t len)
{ return solar_os_native_result(solar_os_shell_io_write_raw(io, data, len)); }
static int shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_writeln(io, text)); }
static int shell_io_printf(solar_os_shell_io_t *io, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    const int result = solar_os_native_result(solar_os_shell_io_vprintf(io, fmt, args));
    va_end(args); return result;
}
static int shell_io_vprintf(solar_os_shell_io_t *io, const char *fmt, va_list args)
{ return solar_os_native_result(solar_os_shell_io_vprintf(io, fmt, args)); }
static int shell_io_table_write(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_table_write(io, text)); }
static int shell_io_table_writeln(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_table_writeln(io, text)); }
static int shell_io_table_printf(solar_os_shell_io_t *io, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    const int result = solar_os_native_result(solar_os_shell_io_table_vprintf(io, fmt, args));
    va_end(args); return result;
}
static int shell_io_table_printf_bold(solar_os_shell_io_t *io, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    const int result = solar_os_native_result(solar_os_shell_io_table_vprintf_bold(io, fmt, args));
    va_end(args); return result;
}
static int shell_io_table_write_bold(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_table_write_bold(io, text)); }
static int shell_io_table_put_char(solar_os_shell_io_t *io, char ch)
{ return solar_os_native_result(solar_os_shell_io_table_put_char(io, ch)); }
static int shell_io_set_bold(solar_os_shell_io_t *io, bool enabled)
{ return solar_os_native_result(solar_os_shell_io_set_bold(io, enabled)); }
static int shell_io_set_italic(solar_os_shell_io_t *io, bool enabled)
{ return solar_os_native_result(solar_os_shell_io_set_italic(io, enabled)); }
static int shell_io_set_underline(solar_os_shell_io_t *io, bool enabled)
{ return solar_os_native_result(solar_os_shell_io_set_underline(io, enabled)); }
static int shell_io_set_inverse(solar_os_shell_io_t *io, bool enabled)
{ return solar_os_native_result(solar_os_shell_io_set_inverse(io, enabled)); }
static int shell_io_write_bold(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_write_bold(io, text)); }
static int shell_io_printf_bold(solar_os_shell_io_t *io, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    const int result = solar_os_native_result(solar_os_shell_io_vprintf_bold(io, fmt, args));
    va_end(args); return result;
}
static int shell_io_clear(solar_os_shell_io_t *io)
{ return solar_os_native_result(solar_os_shell_io_clear(io)); }
static int shell_io_newline(solar_os_shell_io_t *io)
{ return solar_os_native_result(solar_os_shell_io_newline(io)); }
static int shell_io_put_char(solar_os_shell_io_t *io, char ch)
{ return solar_os_native_result(solar_os_shell_io_put_char(io, ch)); }
static int shell_io_put_utf8_byte(solar_os_shell_io_t *io, uint8_t byte)
{ return solar_os_native_result(solar_os_shell_io_put_utf8_byte(io, byte)); }
static int shell_io_set_cursor(solar_os_shell_io_t *io, size_t row, size_t col)
{ return solar_os_native_result(solar_os_shell_io_set_cursor(io, row, col)); }
static int shell_io_set_cursor_visible(solar_os_shell_io_t *io, bool visible)
{ return solar_os_native_result(solar_os_shell_io_set_cursor_visible(io, visible)); }
static int shell_io_clear_line_from(solar_os_shell_io_t *io, size_t row, size_t col)
{ return solar_os_native_result(solar_os_shell_io_clear_line_from(io, row, col)); }
static int shell_io_redraw_line(solar_os_shell_io_t *io, size_t row, size_t col, const char *text, size_t text_len, size_t cursor_offset)
{ return solar_os_native_result(solar_os_shell_io_redraw_line(io, row, col, text, text_len, cursor_offset)); }
static int shell_io_set_footer(solar_os_shell_io_t *io, const char *text)
{ return solar_os_native_result(solar_os_shell_io_set_footer(io, text)); }
static int shell_io_clear_footer(solar_os_shell_io_t *io)
{ return solar_os_native_result(solar_os_shell_io_clear_footer(io)); }
static int shell_io_flush(solar_os_shell_io_t *io)
{ return solar_os_native_result(solar_os_shell_io_flush(io)); }
static int shell_io_vprintf_bold(solar_os_shell_io_t *io, const char *fmt, va_list args)
{ return solar_os_native_result(solar_os_shell_io_vprintf_bold(io, fmt, args)); }
static int shell_io_table_vprintf(solar_os_shell_io_t *io, const char *fmt, va_list args)
{ return solar_os_native_result(solar_os_shell_io_table_vprintf(io, fmt, args)); }
static int shell_io_table_vprintf_bold(solar_os_shell_io_t *io, const char *fmt, va_list args)
{ return solar_os_native_result(solar_os_shell_io_table_vprintf_bold(io, fmt, args)); }

static int shell_io_poll_input(const solar_os_shell_io_t *io, uint8_t *buffer,
                               size_t capacity, size_t *count)
{
    if (io == NULL || io != shell_io_current() ||
        solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        if (count != NULL) {
            *count = 0;
        }
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    return solar_os_native_result(solar_os_shell_io_poll_input(io, buffer, capacity, count));
}

static const solar_os_native_shell_io_api_v1_t shell_io_api = {
    .abi_version = SOLAR_OS_NATIVE_SHELL_IO_ABI, .struct_size = sizeof(shell_io_api),
    .current = shell_io_current,
    .set_dimensions = solar_os_shell_io_set_dimensions,
    .kind = solar_os_shell_io_kind,
    .terminal = solar_os_shell_io_terminal,
    .terminal_profile_name = solar_os_shell_terminal_profile_name,
    .parse_terminal_profile = solar_os_shell_parse_terminal_profile,
    .set_terminal_profile = solar_os_shell_io_set_terminal_profile,
    .terminal_profile = solar_os_shell_io_terminal_profile,
    .charset_name = solar_os_shell_charset_name,
    .parse_charset = solar_os_shell_parse_charset,
    .set_charset = solar_os_shell_io_set_charset,
    .charset = solar_os_shell_io_charset,
    .is_cursor_addressable = solar_os_shell_io_is_cursor_addressable,
    .app_exit_key = solar_os_shell_io_app_exit_key,
    .write = shell_io_write,
    .write_len = shell_io_write_len,
    .write_raw = shell_io_write_raw,
    .writeln = shell_io_writeln,
    .printf = shell_io_printf,
    .vprintf = shell_io_vprintf,
    .table_write = shell_io_table_write,
    .table_writeln = shell_io_table_writeln,
    .table_printf = shell_io_table_printf,
    .table_printf_bold = shell_io_table_printf_bold,
    .table_write_bold = shell_io_table_write_bold,
    .table_put_char = shell_io_table_put_char,
    .set_bold = shell_io_set_bold,
    .set_italic = shell_io_set_italic,
    .set_underline = shell_io_set_underline,
    .set_inverse = shell_io_set_inverse,
    .write_bold = shell_io_write_bold,
    .printf_bold = shell_io_printf_bold,
    .clear = shell_io_clear,
    .newline = shell_io_newline,
    .put_char = shell_io_put_char,
    .put_utf8_byte = shell_io_put_utf8_byte,
    .cols = solar_os_shell_io_cols,
    .rows = solar_os_shell_io_rows,
    .cursor_row = solar_os_shell_io_cursor_row,
    .cursor_col = solar_os_shell_io_cursor_col,
    .set_cursor = shell_io_set_cursor,
    .set_cursor_visible = shell_io_set_cursor_visible,
    .cursor_visible = solar_os_shell_io_cursor_visible,
    .screen_generation = solar_os_shell_io_screen_generation,
    .clear_line_from = shell_io_clear_line_from,
    .redraw_line = shell_io_redraw_line,
    .set_footer = shell_io_set_footer,
    .clear_footer = shell_io_clear_footer,
    .flush = shell_io_flush,
    .vprintf_bold = shell_io_vprintf_bold,
    .table_vprintf = shell_io_table_vprintf,
    .table_vprintf_bold = shell_io_table_vprintf_bold,
    .poll_input = shell_io_poll_input,
};
#endif

const void *solar_os_native_shell_io_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_shell_io_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
