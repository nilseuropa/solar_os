#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_io.h"

static char output[32768];
static size_t output_len, terminal_col, terminal_row, terminal_width;

static void capture(const char *text, size_t len)
{
    assert(output_len + len < sizeof(output));
    memcpy(output + output_len, text, len);
    output_len += len;
    output[output_len] = '\0';
}

bool solar_os_port_handle_valid(const solar_os_port_handle_t *port)
{
    (void)port;
    return true;
}

esp_err_t solar_os_port_write(const solar_os_port_handle_t *port,
                              const uint8_t *data, size_t len, size_t *written)
{
    (void)port;
    capture((const char *)data, len);
    *written = len;
    return ESP_OK;
}

size_t solar_os_terminal_cols(const solar_os_terminal_t *term)
{
    (void)term;
    return terminal_width;
}

size_t solar_os_terminal_cursor_col(const solar_os_terminal_t *term)
{
    (void)term;
    return terminal_col;
}

size_t solar_os_terminal_cursor_row(const solar_os_terminal_t *term)
{
    (void)term;
    return terminal_row;
}

void solar_os_terminal_set_bold(solar_os_terminal_t *term, bool bold)
{
    (void)term;
    (void)bold;
}

void solar_os_terminal_put_char(solar_os_terminal_t *term, char ch)
{
    (void)term;
    capture(&ch, 1U);
    if (ch == '\n') {
        terminal_col = 0U;
        terminal_row++;
    } else {
        /* Model delayed wrap on the display terminal. */
        if (terminal_col >= terminal_width) {
            terminal_row++;
            terminal_col = 0U;
        }
        terminal_col++;
    }
}

static void reset(solar_os_shell_io_t *io, size_t width, bool display)
{
    *io = (solar_os_shell_io_t){
        .kind = display ? SOLAR_OS_SHELL_IO_KIND_TERMINAL : SOLAR_OS_SHELL_IO_KIND_PORT,
        .terminal_profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI,
        .terminal = (solar_os_terminal_t *)(uintptr_t)1U,
        .cols = width,
        .rows = 100,
    };
    terminal_width = width;
    terminal_col = terminal_row = output_len = 0U;
    output[0] = '\0';
}

static void check_rows(size_t width, size_t expected)
{
    size_t col = 0U, rows = 0U;
    for (size_t i = 0U; i < output_len; i++) {
        if (output[i] == '\x1b') {
            assert(output[++i] == '[');
            while (output[i] != 'm') i++;
        } else if (output[i] == '\n') {
            assert(col < width);
            col = 0U;
            rows++;
        } else if (output[i] != '\r') {
            col++;
        }
    }
    assert(rows == expected && col == 0U);
}

int main(void)
{
    solar_os_shell_io_t io;
    const size_t widths[] = {1, 2, 8, 20, 26, 32, 40, 80, 120, 240};
    char long_name[600];
    memset(long_name, 'x', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    for (size_t display = 0U; display < 2U; display++) {
        for (size_t i = 0U; i < sizeof(widths) / sizeof(widths[0]); i++) {
            reset(&io, widths[i], display != 0U);
            assert(solar_os_shell_io_table_writeln(&io,
                "NAME         STATE    STACK KIND        EVT  TICKS RES") == ESP_OK);
            assert(solar_os_shell_io_table_printf_bold(&io,
                "%-12s %-8s %5u %-11s %4s %5u %3u\n",
                "telnetd", "running", 6144U, "background", "tick", 123U, 1U) == ESP_OK);
            assert(!io.bold);
            assert(solar_os_shell_io_table_printf(&io,
                "Internal: total %s free %s low %s max %s\n",
                "512 KiB", "64 KiB", "32 KiB", "24 KiB") == ESP_OK);
            assert(solar_os_shell_io_table_write(&io, "00: ") == ESP_OK);
            for (unsigned n = 0; n < 16U; n++) {
                assert(solar_os_shell_io_table_printf(&io, "%02x ", n) == ESP_OK);
            }
            assert(solar_os_shell_io_table_put_char(&io, '\n') == ESP_OK);
            assert(solar_os_shell_io_table_printf(&io, "%8u %s\n", 123U, long_name) == ESP_OK);
            assert(solar_os_shell_io_table_write_bold(&io, "next\trow\r\n") == ESP_OK);
            check_rows(widths[i], 6U);
            assert(solar_os_shell_io_cursor_col(&io) == 0U);
            assert(solar_os_shell_io_cursor_row(&io) == 6U);
        }
    }
    reset(&io, 5U, false);
    solar_os_shell_io_table_write(&io, "ab\xe2\x82\xac");
    solar_os_shell_io_table_writeln(&io, "suffix");
    assert(strcmp(output, "ab  \r\n") == 0);
    reset(&io, 8U, false);
    solar_os_shell_io_table_writeln(&io, "\xe2\x82\xac");
    assert(strcmp(output, "\xe2\x82\xac\r\n") == 0);
    reset(&io, 80U, true);
    terminal_width = 10U; /* Live display geometry overrides cached dimensions. */
    solar_os_shell_io_table_writeln(&io, "0123456789abcdef");
    assert(strcmp(output, "012345678\n") == 0);
    reset(&io, 10U, false);
    solar_os_shell_io_table_write(&io, "012345");
    solar_os_shell_io_set_dimensions(&io, 8U, 100U);
    solar_os_shell_io_table_writeln(&io, "6789");
    assert(strcmp(output, "0123456\r\n") == 0);
    reset(&io, 8U, false);
    solar_os_shell_io_write(&io, "ordinary output still wraps");
    assert(strcmp(output, "ordinary output still wraps") == 0);
    puts("shell table tests passed");
    return 0;
}
