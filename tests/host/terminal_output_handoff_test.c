#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_terminal_internal.h"

static void set_line(solar_os_terminal_cell_t *line, const char *text)
{
    size_t index = 0U;
    for (; text[index] != '\0'; index++) {
        line[index] = (unsigned char)text[index];
    }
    line[index] = 0U;
}

static void expect_line(const solar_os_terminal_cell_t *line,
                        const char *expected)
{
    size_t index = 0U;
    for (; expected[index] != '\0'; index++) {
        assert(line[index] == (unsigned char)expected[index]);
    }
    assert(line[index] == 0U);
}

int main(void)
{
    solar_os_terminal_cell_t history[2][SOLAR_OS_TERMINAL_MAX_COLS + 1] = {0};
    solar_os_terminal_t source = {
        .rows = 6U,
        .cols = 16U,
        .scrollback = history,
        .scrollback_capacity = 2U,
        .scrollback_count = 2U,
    };
    solar_os_terminal_t destination = {
        .rows = 8U,
        .cols = 16U,
    };
    set_line(history[0], "older");
    set_line(history[1], "old");
    set_line(source.lines[0], "first");
    set_line(source.lines[2], "second   ");
    set_line(destination.lines[0], "shell");
    destination.cursor_col = 5U;

    assert(solar_os_terminal_append_text(&destination, &source));
    expect_line(destination.lines[0], "shell");
    expect_line(destination.lines[1], "older");
    expect_line(destination.lines[2], "old");
    expect_line(destination.lines[3], "first");
    expect_line(destination.lines[4], "");
    expect_line(destination.lines[5], "second");
    assert(destination.cursor_row == 6U);
    assert(destination.cursor_col == 0U);

    solar_os_terminal_t empty = {
        .rows = 2U,
        .cols = 16U,
    };
    assert(!solar_os_terminal_append_text(&destination, &empty));
    puts("terminal output handoff tests passed");
    return 0;
}
