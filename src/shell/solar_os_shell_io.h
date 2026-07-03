#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_port.h"

typedef enum {
    SOLAR_OS_SHELL_IO_KIND_NONE,
    SOLAR_OS_SHELL_IO_KIND_TERMINAL,
    SOLAR_OS_SHELL_IO_KIND_PORT,
} solar_os_shell_io_kind_t;

typedef enum {
    SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO,
    SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB,
    SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI,
    SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100,
} solar_os_shell_terminal_profile_t;

struct solar_os_shell_io {
    solar_os_shell_io_kind_t kind;
    solar_os_shell_terminal_profile_t terminal_profile;
    solar_os_terminal_t *terminal;
    solar_os_port_handle_t port;
    uint16_t cols;
    uint16_t rows;
    size_t cursor_row;
    size_t cursor_col;
    bool bold;
    bool italic;
    bool underline;
    bool inverse;
    bool cursor_visible;
};

void solar_os_shell_io_init_terminal(solar_os_shell_io_t *io, solar_os_terminal_t *terminal);
void solar_os_shell_io_init_port(solar_os_shell_io_t *io,
                                 const solar_os_port_handle_t *port,
                                 uint16_t cols,
                                 uint16_t rows);
void solar_os_shell_io_set_dimensions(solar_os_shell_io_t *io, uint16_t cols, uint16_t rows);
solar_os_shell_io_kind_t solar_os_shell_io_kind(const solar_os_shell_io_t *io);
solar_os_terminal_t *solar_os_shell_io_terminal(solar_os_shell_io_t *io);
const char *solar_os_shell_terminal_profile_name(solar_os_shell_terminal_profile_t profile);
bool solar_os_shell_parse_terminal_profile(const char *name,
                                           solar_os_shell_terminal_profile_t *profile);
void solar_os_shell_io_set_terminal_profile(solar_os_shell_io_t *io,
                                            solar_os_shell_terminal_profile_t profile);
solar_os_shell_terminal_profile_t solar_os_shell_io_terminal_profile(const solar_os_shell_io_t *io);
bool solar_os_shell_io_is_cursor_addressable(const solar_os_shell_io_t *io);
const char *solar_os_shell_io_app_exit_key(const solar_os_shell_io_t *io);
esp_err_t solar_os_shell_io_write(solar_os_shell_io_t *io, const char *text);
esp_err_t solar_os_shell_io_write_len(solar_os_shell_io_t *io, const char *text, size_t len);
esp_err_t solar_os_shell_io_write_raw(solar_os_shell_io_t *io, const char *data, size_t len);
esp_err_t solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text);
esp_err_t solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *fmt, ...);
esp_err_t solar_os_shell_io_vprintf(solar_os_shell_io_t *io, const char *fmt, va_list args);
esp_err_t solar_os_shell_io_set_bold(solar_os_shell_io_t *io, bool enabled);
esp_err_t solar_os_shell_io_set_italic(solar_os_shell_io_t *io, bool enabled);
esp_err_t solar_os_shell_io_set_underline(solar_os_shell_io_t *io, bool enabled);
esp_err_t solar_os_shell_io_set_inverse(solar_os_shell_io_t *io, bool enabled);
esp_err_t solar_os_shell_io_write_bold(solar_os_shell_io_t *io, const char *text);
esp_err_t solar_os_shell_io_printf_bold(solar_os_shell_io_t *io, const char *fmt, ...);
esp_err_t solar_os_shell_io_clear(solar_os_shell_io_t *io);
esp_err_t solar_os_shell_io_newline(solar_os_shell_io_t *io);
esp_err_t solar_os_shell_io_put_char(solar_os_shell_io_t *io, char ch);
esp_err_t solar_os_shell_io_put_utf8_byte(solar_os_shell_io_t *io, uint8_t byte);
uint16_t solar_os_shell_io_cols(const solar_os_shell_io_t *io);
uint16_t solar_os_shell_io_rows(const solar_os_shell_io_t *io);
size_t solar_os_shell_io_cursor_row(const solar_os_shell_io_t *io);
size_t solar_os_shell_io_cursor_col(const solar_os_shell_io_t *io);
esp_err_t solar_os_shell_io_set_cursor(solar_os_shell_io_t *io, size_t row, size_t col);
esp_err_t solar_os_shell_io_set_cursor_visible(solar_os_shell_io_t *io, bool visible);
bool solar_os_shell_io_cursor_visible(const solar_os_shell_io_t *io);
esp_err_t solar_os_shell_io_clear_line_from(solar_os_shell_io_t *io, size_t row, size_t col);
esp_err_t solar_os_shell_io_flush(solar_os_shell_io_t *io);
