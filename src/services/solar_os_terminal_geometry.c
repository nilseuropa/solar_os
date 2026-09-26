#include "solar_os_terminal_geometry.h"

size_t solar_os_terminal_columns_compute(int display_width,
                                         int horizontal_margin,
                                         int cell_width,
                                         size_t max_cols)
{
    if (display_width <= 0 || horizontal_margin < 0 || cell_width <= 0 || max_cols == 0U) {
        return 0U;
    }

    const int content_width = display_width - (horizontal_margin * 2);
    size_t cols = content_width >= cell_width ?
        (size_t)(content_width / cell_width) : 1U;
    if (cols > max_cols) {
        cols = max_cols;
    }
    return cols;
}

bool solar_os_terminal_geometry_compute(int display_height,
                                        int status_bar_height,
                                        int footer_height,
                                        int line_height,
                                        int cell_ascent,
                                        size_t max_rows,
                                        solar_os_terminal_geometry_t *geometry)
{
    if (geometry == NULL || display_height <= 0 || status_bar_height < 0 ||
        footer_height < 0 || line_height <= 0 || cell_ascent <= 0 ||
        cell_ascent > line_height || max_rows == 0) {
        return false;
    }
    int content_bottom = display_height - footer_height;
    if (content_bottom < status_bar_height) content_bottom = status_bar_height;
    const int content_height = content_bottom - status_bar_height;
    size_t rows = content_height >= line_height ?
        (size_t)(content_height / line_height) : 1U;
    if (rows > max_rows) rows = max_rows;
    geometry->rows = rows;
    geometry->grid_top = status_bar_height;
    geometry->baseline_offset = geometry->grid_top + cell_ascent;
    geometry->content_bottom = content_bottom;
    return true;
}
