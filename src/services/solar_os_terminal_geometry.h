#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t rows;
    int grid_top;
    int baseline_offset;
    int content_bottom;
} solar_os_terminal_geometry_t;

size_t solar_os_terminal_columns_compute(int display_width,
                                         int horizontal_margin,
                                         int cell_width,
                                         size_t max_cols);

bool solar_os_terminal_geometry_compute(int display_height,
                                        int status_bar_height,
                                        int footer_height,
                                        int line_height,
                                        int cell_ascent,
                                        size_t max_rows,
                                        solar_os_terminal_geometry_t *geometry);
