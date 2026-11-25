#pragma once
#include <stdarg.h>
#include "solar_os_native_service_abi.h"
#include "solar_os_shell_io_types_v1.h"

#define SOLAR_OS_NATIVE_SHELL_IO_SERVICE "shell_io"
#define SOLAR_OS_NATIVE_SHELL_IO_ABI 1U

/* current borrows the active application callback's output channel, or NULL
 * outside that task's scope. The core owns the channel, terminal and port.
 * Cache the service table, but use borrowed channels only in their owning
 * callback. Text and formatting buffers are consumed before return. No
 * adapter buffer or per-call allocation is added. Terminal profiles apply
 * to the channel; they do not persist global display preferences. */
typedef struct {
    uint32_t abi_version, struct_size;
    solar_os_shell_io_t *(*current)(void);
    void (*set_dimensions)(solar_os_shell_io_t *io, uint16_t cols, uint16_t rows);
    solar_os_shell_io_kind_t (*kind)(const solar_os_shell_io_t *io);
    solar_os_terminal_t * (*terminal)(solar_os_shell_io_t *io);
    const char * (*terminal_profile_name)(solar_os_shell_terminal_profile_t profile);
    bool (*parse_terminal_profile)(const char *name, solar_os_shell_terminal_profile_t *profile);
    void (*set_terminal_profile)(solar_os_shell_io_t *io, solar_os_shell_terminal_profile_t profile);
    solar_os_shell_terminal_profile_t (*terminal_profile)(const solar_os_shell_io_t *io);
    const char * (*charset_name)(solar_os_shell_charset_t charset);
    bool (*parse_charset)(const char *name, solar_os_shell_charset_t *charset);
    void (*set_charset)(solar_os_shell_io_t *io, solar_os_shell_charset_t charset);
    solar_os_shell_charset_t (*charset)(const solar_os_shell_io_t *io);
    bool (*is_cursor_addressable)(const solar_os_shell_io_t *io);
    const char * (*app_exit_key)(const solar_os_shell_io_t *io);
    int (*write)(solar_os_shell_io_t *io, const char *text);
    int (*write_len)(solar_os_shell_io_t *io, const char *text, size_t len);
    int (*write_raw)(solar_os_shell_io_t *io, const char *data, size_t len);
    int (*writeln)(solar_os_shell_io_t *io, const char *text);
    int (*printf)(solar_os_shell_io_t *io, const char *fmt, ...);
    int (*vprintf)(solar_os_shell_io_t *io, const char *fmt, va_list args);
    int (*table_write)(solar_os_shell_io_t *io, const char *text);
    int (*table_writeln)(solar_os_shell_io_t *io, const char *text);
    int (*table_printf)(solar_os_shell_io_t *io, const char *fmt, ...);
    int (*table_printf_bold)(solar_os_shell_io_t *io, const char *fmt, ...);
    int (*table_write_bold)(solar_os_shell_io_t *io, const char *text);
    int (*table_put_char)(solar_os_shell_io_t *io, char ch);
    int (*set_bold)(solar_os_shell_io_t *io, bool enabled);
    int (*set_italic)(solar_os_shell_io_t *io, bool enabled);
    int (*set_underline)(solar_os_shell_io_t *io, bool enabled);
    int (*set_inverse)(solar_os_shell_io_t *io, bool enabled);
    int (*write_bold)(solar_os_shell_io_t *io, const char *text);
    int (*printf_bold)(solar_os_shell_io_t *io, const char *fmt, ...);
    int (*clear)(solar_os_shell_io_t *io);
    int (*newline)(solar_os_shell_io_t *io);
    int (*put_char)(solar_os_shell_io_t *io, char ch);
    int (*put_utf8_byte)(solar_os_shell_io_t *io, uint8_t byte);
    uint16_t (*cols)(const solar_os_shell_io_t *io);
    uint16_t (*rows)(const solar_os_shell_io_t *io);
    size_t (*cursor_row)(const solar_os_shell_io_t *io);
    size_t (*cursor_col)(const solar_os_shell_io_t *io);
    int (*set_cursor)(solar_os_shell_io_t *io, size_t row, size_t col);
    int (*set_cursor_visible)(solar_os_shell_io_t *io, bool visible);
    bool (*cursor_visible)(const solar_os_shell_io_t *io);
    uint32_t (*screen_generation)(const solar_os_shell_io_t *io);
    int (*clear_line_from)(solar_os_shell_io_t *io, size_t row, size_t col);
    int (*redraw_line)(solar_os_shell_io_t *io, size_t row, size_t col, const char *text, size_t text_len, size_t cursor_offset);
    int (*set_footer)(solar_os_shell_io_t *io, const char *text);
    int (*clear_footer)(solar_os_shell_io_t *io);
    int (*flush)(solar_os_shell_io_t *io);
    int (*vprintf_bold)(solar_os_shell_io_t *io, const char *fmt, va_list args);
    int (*table_vprintf)(solar_os_shell_io_t *io, const char *fmt, va_list args);
    int (*table_vprintf_bold)(solar_os_shell_io_t *io, const char *fmt, va_list args);
} solar_os_native_shell_io_api_v1_t;
