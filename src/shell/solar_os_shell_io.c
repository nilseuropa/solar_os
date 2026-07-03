#include "solar_os_shell_io.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_terminal.h"

#define SHELL_IO_DEFAULT_COLS 80
#define SHELL_IO_DEFAULT_ROWS 24

static uint16_t shell_io_nonzero_or_default(uint16_t value, uint16_t fallback)
{
    return value != 0 ? value : fallback;
}

static bool shell_io_terminal_profile_is_valid(solar_os_shell_terminal_profile_t profile)
{
    switch (profile) {
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO:
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB:
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI:
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100:
        return true;
    default:
        return false;
    }
}

static bool shell_io_port_supports_ansi_controls(const solar_os_shell_io_t *io)
{
    return io != NULL &&
        io->kind == SOLAR_OS_SHELL_IO_KIND_PORT &&
        (io->terminal_profile == SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI ||
         io->terminal_profile == SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100);
}

static void shell_io_track_newline(solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return;
    }

    io->cursor_col = 0;
    if (io->rows == 0 || io->cursor_row + 1 < io->rows) {
        io->cursor_row++;
    }
}

static void shell_io_track_char(solar_os_shell_io_t *io, char ch)
{
    if (io == NULL) {
        return;
    }

    switch (ch) {
    case '\r':
        io->cursor_col = 0;
        break;
    case '\n':
        shell_io_track_newline(io);
        break;
    case '\b':
        if (io->cursor_col > 0) {
            io->cursor_col--;
        }
        break;
    case '\t': {
        const size_t next_col = (io->cursor_col + 4U) & ~(size_t)3U;
        io->cursor_col = next_col;
        break;
    }
    default:
        if ((unsigned char)ch >= 0x20) {
            io->cursor_col++;
        }
        break;
    }

    if (io->cols != 0 && io->cursor_col >= io->cols) {
        io->cursor_col = 0;
        if (io->rows == 0 || io->cursor_row + 1 < io->rows) {
            io->cursor_row++;
        }
    }
}

static esp_err_t shell_io_port_write_bytes(solar_os_shell_io_t *io, const char *data, size_t len)
{
    if (io == NULL || io->kind != SOLAR_OS_SHELL_IO_KIND_PORT ||
        !solar_os_port_handle_valid(&io->port)) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = 0;
    const esp_err_t err =
        solar_os_port_write(&io->port, (const uint8_t *)data, len, &written);
    if (err != ESP_OK) {
        return err;
    }
    return written == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t shell_io_port_write_text(solar_os_shell_io_t *io, const char *text, size_t len)
{
    size_t start = 0;

    if (io == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; i++) {
        if (text[i] != '\n') {
            continue;
        }

        if (i > start) {
            esp_err_t err = shell_io_port_write_bytes(io, &text[start], i - start);
            if (err != ESP_OK) {
                return err;
            }
            for (size_t j = start; j < i; j++) {
                shell_io_track_char(io, text[j]);
            }
        }

        if (i == 0 || text[i - 1] != '\r') {
            esp_err_t err = shell_io_port_write_bytes(io, "\r\n", 2);
            if (err != ESP_OK) {
                return err;
            }
            shell_io_track_char(io, '\n');
        } else {
            esp_err_t err = shell_io_port_write_bytes(io, "\n", 1);
            if (err != ESP_OK) {
                return err;
            }
            shell_io_track_char(io, '\n');
        }
        start = i + 1;
    }

    if (start < len) {
        esp_err_t err = shell_io_port_write_bytes(io, &text[start], len - start);
        if (err != ESP_OK) {
            return err;
        }
        for (size_t i = start; i < len; i++) {
            shell_io_track_char(io, text[i]);
        }
    }
    return ESP_OK;
}

void solar_os_shell_io_init_terminal(solar_os_shell_io_t *io, solar_os_terminal_t *terminal)
{
    if (io == NULL) {
        return;
    }

    memset(io, 0, sizeof(*io));
    io->kind = terminal != NULL ? SOLAR_OS_SHELL_IO_KIND_TERMINAL : SOLAR_OS_SHELL_IO_KIND_NONE;
    io->terminal_profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100;
    io->terminal = terminal;
    io->port = (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
    io->cols = terminal != NULL ? (uint16_t)solar_os_terminal_cols(terminal) : 0;
    io->rows = terminal != NULL ? (uint16_t)solar_os_terminal_rows(terminal) : 0;
    io->cursor_row = terminal != NULL ? solar_os_terminal_cursor_row(terminal) : 0;
    io->cursor_col = terminal != NULL ? solar_os_terminal_cursor_col(terminal) : 0;
    io->bold = terminal != NULL ? solar_os_terminal_bold(terminal) : false;
    io->italic = terminal != NULL ? solar_os_terminal_italic(terminal) : false;
    io->underline = terminal != NULL ? solar_os_terminal_underline(terminal) : false;
    io->inverse = terminal != NULL ? solar_os_terminal_inverse(terminal) : false;
    io->cursor_visible = terminal != NULL ? solar_os_terminal_cursor_visible(terminal) : true;
}

void solar_os_shell_io_init_port(solar_os_shell_io_t *io,
                                 const solar_os_port_handle_t *port,
                                 uint16_t cols,
                                 uint16_t rows)
{
    if (io == NULL) {
        return;
    }

    memset(io, 0, sizeof(*io));
    io->kind = port != NULL && solar_os_port_handle_valid(port) ?
        SOLAR_OS_SHELL_IO_KIND_PORT :
        SOLAR_OS_SHELL_IO_KIND_NONE;
    io->terminal_profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100;
    io->terminal = NULL;
    io->port = port != NULL ? *port : (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;
    io->cols = shell_io_nonzero_or_default(cols, SHELL_IO_DEFAULT_COLS);
    io->rows = shell_io_nonzero_or_default(rows, SHELL_IO_DEFAULT_ROWS);
    io->cursor_visible = true;
}

void solar_os_shell_io_set_dimensions(solar_os_shell_io_t *io, uint16_t cols, uint16_t rows)
{
    if (io == NULL || io->kind != SOLAR_OS_SHELL_IO_KIND_PORT) {
        return;
    }

    io->cols = shell_io_nonzero_or_default(cols, SHELL_IO_DEFAULT_COLS);
    io->rows = shell_io_nonzero_or_default(rows, SHELL_IO_DEFAULT_ROWS);
    if (io->cols > 0 && io->cursor_col >= io->cols) {
        io->cursor_col = io->cols - 1U;
    }
    if (io->rows > 0 && io->cursor_row >= io->rows) {
        io->cursor_row = io->rows - 1U;
    }
}

solar_os_shell_io_kind_t solar_os_shell_io_kind(const solar_os_shell_io_t *io)
{
    return io != NULL ? io->kind : SOLAR_OS_SHELL_IO_KIND_NONE;
}

solar_os_terminal_t *solar_os_shell_io_terminal(solar_os_shell_io_t *io)
{
    return io != NULL && io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL ? io->terminal : NULL;
}

const char *solar_os_shell_terminal_profile_name(solar_os_shell_terminal_profile_t profile)
{
    switch (profile) {
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO:
        return "auto";
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB:
        return "dumb";
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI:
        return "ansi";
    case SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100:
        return "vt100";
    default:
        return "unknown";
    }
}

bool solar_os_shell_parse_terminal_profile(const char *name,
                                           solar_os_shell_terminal_profile_t *profile)
{
    if (name == NULL || profile == NULL) {
        return false;
    }
    if (strcmp(name, "auto") == 0) {
        *profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO;
        return true;
    }
    if (strcmp(name, "dumb") == 0) {
        *profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB;
        return true;
    }
    if (strcmp(name, "ansi") == 0) {
        *profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI;
        return true;
    }
    if (strcmp(name, "vt100") == 0) {
        *profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100;
        return true;
    }
    return false;
}

void solar_os_shell_io_set_terminal_profile(solar_os_shell_io_t *io,
                                            solar_os_shell_terminal_profile_t profile)
{
    if (io == NULL || !shell_io_terminal_profile_is_valid(profile)) {
        return;
    }
    io->terminal_profile = profile;
}

solar_os_shell_terminal_profile_t solar_os_shell_io_terminal_profile(const solar_os_shell_io_t *io)
{
    return io != NULL ? io->terminal_profile : SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB;
}

bool solar_os_shell_io_is_cursor_addressable(const solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return false;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        return true;
    }
    return shell_io_port_supports_ansi_controls(io);
}

const char *solar_os_shell_io_app_exit_key(const solar_os_shell_io_t *io)
{
    return io != NULL && io->kind == SOLAR_OS_SHELL_IO_KIND_PORT ? "Ctrl+]" : "CTRL+ALT+DEL";
}

esp_err_t solar_os_shell_io_write_len(solar_os_shell_io_t *io, const char *text, size_t len)
{
    if (io == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        for (size_t i = 0; i < len; i++) {
            solar_os_terminal_put_char(io->terminal, text[i]);
        }
        io->cursor_row = solar_os_terminal_cursor_row(io->terminal);
        io->cursor_col = solar_os_terminal_cursor_col(io->terminal);
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        return shell_io_port_write_text(io, text, len);
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_write(solar_os_shell_io_t *io, const char *text)
{
    return solar_os_shell_io_write_len(io, text, text != NULL ? strlen(text) : 0);
}

esp_err_t solar_os_shell_io_write_raw(solar_os_shell_io_t *io, const char *data, size_t len)
{
    if (io == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        for (size_t i = 0; i < len; i++) {
            solar_os_terminal_put_char(io->terminal, data[i]);
        }
        io->cursor_row = solar_os_terminal_cursor_row(io->terminal);
        io->cursor_col = solar_os_terminal_cursor_col(io->terminal);
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        return shell_io_port_write_bytes(io, data, len);
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{
    esp_err_t err = solar_os_shell_io_write(io, text);
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_shell_io_newline(io);
}

esp_err_t solar_os_shell_io_vprintf(solar_os_shell_io_t *io, const char *fmt, va_list args)
{
    if (io == NULL || fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char buffer[192];
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);
    if (needed < 0) {
        return ESP_FAIL;
    }
    if ((size_t)needed < sizeof(buffer)) {
        return solar_os_shell_io_write(io, buffer);
    }

    buffer[sizeof(buffer) - 1] = '\0';
    return solar_os_shell_io_write(io, buffer);
}

esp_err_t solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    const esp_err_t err = solar_os_shell_io_vprintf(io, fmt, args);
    va_end(args);
    return err;
}

esp_err_t solar_os_shell_io_set_bold(solar_os_shell_io_t *io, bool enabled)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_set_bold(io->terminal, enabled);
        io->bold = enabled;
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            io->bold = enabled;
            return ESP_OK;
        }
        const char *seq = enabled ? "\x1b[1m" : "\x1b[22m";
        const esp_err_t err = shell_io_port_write_bytes(io, seq, strlen(seq));
        if (err == ESP_OK) {
            io->bold = enabled;
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_set_italic(solar_os_shell_io_t *io, bool enabled)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_set_italic(io->terminal, enabled);
        io->italic = enabled;
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            io->italic = enabled;
            return ESP_OK;
        }
        const char *seq = enabled ? "\x1b[3m" : "\x1b[23m";
        const esp_err_t err = shell_io_port_write_bytes(io, seq, strlen(seq));
        if (err == ESP_OK) {
            io->italic = enabled;
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_set_underline(solar_os_shell_io_t *io, bool enabled)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_set_underline(io->terminal, enabled);
        io->underline = enabled;
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            io->underline = enabled;
            return ESP_OK;
        }
        const char *seq = enabled ? "\x1b[4m" : "\x1b[24m";
        const esp_err_t err = shell_io_port_write_bytes(io, seq, strlen(seq));
        if (err == ESP_OK) {
            io->underline = enabled;
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_set_inverse(solar_os_shell_io_t *io, bool enabled)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_set_inverse(io->terminal, enabled);
        io->inverse = enabled;
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            io->inverse = enabled;
            return ESP_OK;
        }
        const char *seq = enabled ? "\x1b[7m" : "\x1b[27m";
        const esp_err_t err = shell_io_port_write_bytes(io, seq, strlen(seq));
        if (err == ESP_OK) {
            io->inverse = enabled;
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_write_bold(solar_os_shell_io_t *io, const char *text)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_write_bold(io->terminal, text);
        io->cursor_row = solar_os_terminal_cursor_row(io->terminal);
        io->cursor_col = solar_os_terminal_cursor_col(io->terminal);
        return ESP_OK;
    }

    const bool previous = io->bold;
    esp_err_t err = solar_os_shell_io_set_bold(io, true);
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_shell_io_write(io, text);
    const esp_err_t restore_err = solar_os_shell_io_set_bold(io, previous);
    return err != ESP_OK ? err : restore_err;
}

esp_err_t solar_os_shell_io_printf_bold(solar_os_shell_io_t *io, const char *fmt, ...)
{
    if (io == NULL || fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char buffer[192];
    va_list args;
    va_start(args, fmt);
    const int needed = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (needed < 0) {
        return ESP_FAIL;
    }

    buffer[sizeof(buffer) - 1] = '\0';
    return solar_os_shell_io_write_bold(io, buffer);
}

esp_err_t solar_os_shell_io_clear(solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        io->cursor_row = 0;
        io->cursor_col = 0;
        solar_os_terminal_clear(io->terminal);
        return ESP_OK;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            return ESP_OK;
        }
        io->cursor_row = 0;
        io->cursor_col = 0;
        return shell_io_port_write_bytes(io, "\x1b[2J\x1b[H", strlen("\x1b[2J\x1b[H"));
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_newline(solar_os_shell_io_t *io)
{
    return solar_os_shell_io_put_char(io, '\n');
}

esp_err_t solar_os_shell_io_put_char(solar_os_shell_io_t *io, char ch)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_put_char(io->terminal, ch);
        io->cursor_row = solar_os_terminal_cursor_row(io->terminal);
        io->cursor_col = solar_os_terminal_cursor_col(io->terminal);
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (ch == '\n') {
            const esp_err_t err = shell_io_port_write_bytes(io, "\r\n", 2);
            if (err == ESP_OK) {
                shell_io_track_char(io, ch);
            }
            return err;
        }

        const esp_err_t err = shell_io_port_write_bytes(io, &ch, 1);
        if (err == ESP_OK) {
            shell_io_track_char(io, ch);
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_put_utf8_byte(solar_os_shell_io_t *io, uint8_t byte)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_put_utf8_byte(io->terminal, byte);
        io->cursor_row = solar_os_terminal_cursor_row(io->terminal);
        io->cursor_col = solar_os_terminal_cursor_col(io->terminal);
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        const char ch = (char)byte;
        const esp_err_t err = shell_io_port_write_bytes(io, &ch, 1);
        if (err == ESP_OK) {
            shell_io_track_char(io, ch);
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

uint16_t solar_os_shell_io_cols(const solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return 0;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        return (uint16_t)solar_os_terminal_cols(io->terminal);
    }
    return io->cols;
}

uint16_t solar_os_shell_io_rows(const solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return 0;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        return (uint16_t)solar_os_terminal_rows(io->terminal);
    }
    return io->rows;
}

size_t solar_os_shell_io_cursor_row(const solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return 0;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        return solar_os_terminal_cursor_row(io->terminal);
    }
    return io->cursor_row;
}

size_t solar_os_shell_io_cursor_col(const solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return 0;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        return solar_os_terminal_cursor_col(io->terminal);
    }
    return io->cursor_col;
}

esp_err_t solar_os_shell_io_set_cursor(solar_os_shell_io_t *io, size_t row, size_t col)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_set_cursor(io->terminal, row, col);
        io->cursor_row = solar_os_terminal_cursor_row(io->terminal);
        io->cursor_col = solar_os_terminal_cursor_col(io->terminal);
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        char seq[32];
        snprintf(seq, sizeof(seq), "\x1b[%u;%uH", (unsigned)(row + 1), (unsigned)(col + 1));
        const esp_err_t err = shell_io_port_write_bytes(io, seq, strlen(seq));
        if (err == ESP_OK) {
            io->cursor_row = row;
            io->cursor_col = col;
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_set_cursor_visible(solar_os_shell_io_t *io, bool visible)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_set_cursor_visible(io->terminal, visible);
        io->cursor_visible = visible;
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (!shell_io_port_supports_ansi_controls(io)) {
            io->cursor_visible = visible;
            return ESP_OK;
        }
        const esp_err_t err =
            shell_io_port_write_bytes(io, visible ? "\x1b[?25h" : "\x1b[?25l", strlen("\x1b[?25h"));
        if (err == ESP_OK) {
            io->cursor_visible = visible;
        }
        return err;
    }

    return ESP_ERR_INVALID_STATE;
}

bool solar_os_shell_io_cursor_visible(const solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return false;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        return solar_os_terminal_cursor_visible(io->terminal);
    }
    return io->cursor_visible;
}

esp_err_t solar_os_shell_io_clear_line_from(solar_os_shell_io_t *io, size_t row, size_t col)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_clear_line_from(io->terminal, row, col);
        return ESP_OK;
    }

    if (io->kind == SOLAR_OS_SHELL_IO_KIND_PORT) {
        esp_err_t err = solar_os_shell_io_set_cursor(io, row, col);
        if (err != ESP_OK) {
            return err;
        }
        return shell_io_port_write_bytes(io, "\x1b[K", strlen("\x1b[K"));
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_shell_io_flush(solar_os_shell_io_t *io)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (io->kind == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        solar_os_terminal_draw(io->terminal);
    }
    return io->kind == SOLAR_OS_SHELL_IO_KIND_NONE ? ESP_ERR_INVALID_STATE : ESP_OK;
}
