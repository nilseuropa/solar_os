#include <assert.h>
#include <stdio.h>

#include "solar_os_terminal.h"
#include "solar_os_terminal_geometry.h"

static void test_columns(void)
{
    static const int cell_widths[] = {5, 6, 7, 8, 9, 10};
    for (size_t i = 0; i < sizeof(cell_widths) / sizeof(cell_widths[0]); i++) {
        const int cell_width = cell_widths[i];
        const size_t cols = solar_os_terminal_columns_compute(
            792, 4, cell_width, SOLAR_OS_TERMINAL_MAX_COLS);
        assert(cols == (size_t)(784 / cell_width));
        assert(cols * (size_t)cell_width <= 784U);
        assert(784U - cols * (size_t)cell_width < (size_t)cell_width);
    }

    assert(solar_os_terminal_columns_compute(
               792, 4, 5, SOLAR_OS_TERMINAL_MAX_COLS) == 156U);
    assert(solar_os_terminal_columns_compute(792, 4, 0, 160) == 0U);
    assert(solar_os_terminal_columns_compute(4, 4, 5, 160) == 1U);
    assert(solar_os_terminal_columns_compute(792, -1, 5, 160) == 0U);
    assert(solar_os_terminal_columns_compute(792, 4, 5, 0) == 0U);
}

static void expect_geometry(int height, int status, int footer, int line, int ascent)
{
    solar_os_terminal_geometry_t geometry;
    assert(solar_os_terminal_geometry_compute(height, status, footer, line, ascent,
                                              64, &geometry));
    assert(geometry.rows >= 1);
    assert(geometry.grid_top == status);
    assert(geometry.baseline_offset == geometry.grid_top + ascent);
    assert(geometry.grid_top + (int)geometry.rows * line <= geometry.content_bottom);
    const int bottom_gap = geometry.content_bottom -
                           (geometry.grid_top + (int)geometry.rows * line);
    assert(bottom_gap >= 0);
    assert(bottom_gap < line);
}
int main(void)
{
    test_columns();
    const int heights[] = {200, 240, 288, 300};
    const int lines[] = {10, 12, 14, 16, 18, 20};
    for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
        for (size_t l = 0; l < sizeof(lines) / sizeof(lines[0]); l++) {
            expect_geometry(heights[h], 16, 0, lines[l], lines[l] - 2);
            expect_geometry(heights[h], 16, lines[l], lines[l], lines[l] - 2);
            expect_geometry(heights[h], 0, 0, lines[l], lines[l] - 2);
        }
    }
    puts("terminal_geometry_test: ok");
    return 0;
}
