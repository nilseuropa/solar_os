#include "solar_os_plot.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_time.h"

#define PLOT_MAX_SERIES 6U
#define PLOT_MAX_LIVE_STREAMS 6U
#define PLOT_MAX_COLS 32U
#define PLOT_CELL_MAX 72U
#define PLOT_LINE_MAX 512U
#define PLOT_SERIES_NAME_MAX 40U
#define PLOT_MESSAGE_MAX 96U
#define PLOT_DEFAULT_CAPACITY 2048U
#define PLOT_DEFAULT_RATE_MS 1000U
#define PLOT_MIN_RATE_MS 1U
#define PLOT_MAX_RATE_MS 86400000U
#define PLOT_DEFAULT_WINDOW_MS 60000U
#define PLOT_MIN_WINDOW_MS 1000U
#define PLOT_MAX_WINDOW_MS 86400000U

typedef enum {
    PLOT_MODE_CSV,
    PLOT_MODE_LIVE,
} plot_mode_t;

typedef struct {
    char name[PLOT_SERIES_NAME_MAX];
    size_t csv_col;
    solar_os_gfx_color_t color;
} plot_series_t;

typedef struct {
    solar_os_stream_handle_t handle;
    solar_os_stream_info_t info;
    size_t first_series;
    size_t series_count;
    size_t value_col[PLOT_MAX_SERIES];
} plot_live_stream_t;

typedef struct {
    bool running;
    bool paused;
    bool follow;
    bool autoscale;
    plot_mode_t mode;
    char title[SOLAR_OS_STORAGE_PATH_MAX];
    char message[PLOT_MESSAGE_MAX];
    plot_series_t series[PLOT_MAX_SERIES];
    size_t series_count;
    size_t active_series;
    plot_live_stream_t live[PLOT_MAX_LIVE_STREAMS];
    size_t live_count;
    uint32_t rate_ms;
    uint32_t live_window_ms;
    uint32_t next_sample_ms;
    uint64_t *x;
    float *y;
    size_t capacity;
    size_t start;
    size_t count;
    size_t view_start;
    size_t visible_count;
    char line[PLOT_LINE_MAX];
    char cells[PLOT_MAX_COLS][PLOT_CELL_MAX];
} plot_state_t;

static const char *TAG = "solar_os_plot";
static plot_state_t plot;

static void plot_set_message(const char *message)
{
    strlcpy(plot.message, message != NULL ? message : "", sizeof(plot.message));
}

static void plot_print_usage(solar_os_context_t *ctx, const char *reason)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);

    if (io == NULL) {
        return;
    }

    if (reason != NULL && reason[0] != '\0') {
        solar_os_shell_io_printf(io, "plot: %s\n", reason);
    }
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  plot <scalar-stream...> [--rate ms]");
    solar_os_shell_io_writeln(io, "  plot -f <file.csv> [column...]");
    solar_os_shell_io_writeln(io, "examples:");
    solar_os_shell_io_writeln(io, "  plot temperature humidity --rate 1000");
    solar_os_shell_io_writeln(io, "  plot -f /logs/env.csv temperature humidity");
    solar_os_shell_io_flush(io);
    solar_os_context_request_terminal_preserve(ctx);
}

static void *plot_alloc(size_t bytes)
{
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void plot_free_buffers(void)
{
    if (plot.x != NULL) {
        heap_caps_free(plot.x);
        plot.x = NULL;
    }
    if (plot.y != NULL) {
        heap_caps_free(plot.y);
        plot.y = NULL;
    }
    plot.capacity = 0;
    plot.start = 0;
    plot.count = 0;
}

static bool plot_alloc_buffers(size_t capacity)
{
    plot_free_buffers();
    plot.x = plot_alloc(capacity * sizeof(plot.x[0]));
    plot.y = plot_alloc(capacity * PLOT_MAX_SERIES * sizeof(plot.y[0]));
    if (plot.x == NULL || plot.y == NULL) {
        plot_free_buffers();
        return false;
    }
    plot.capacity = capacity;
    return true;
}

static size_t plot_physical_index(size_t logical_index)
{
    return (plot.start + logical_index) % plot.capacity;
}

static float *plot_y_slot(size_t physical_index)
{
    return &plot.y[physical_index * PLOT_MAX_SERIES];
}

static void plot_append_sample(uint64_t x, const float values[PLOT_MAX_SERIES])
{
    if (plot.capacity == 0 || values == NULL) {
        return;
    }

    size_t physical = 0;
    if (plot.count < plot.capacity) {
        physical = (plot.start + plot.count) % plot.capacity;
        plot.count++;
    } else {
        physical = plot.start;
        plot.start = (plot.start + 1U) % plot.capacity;
        if (plot.view_start > 0) {
            plot.view_start--;
        }
    }

    plot.x[physical] = x;
    float *slot = plot_y_slot(physical);
    for (size_t i = 0; i < PLOT_MAX_SERIES; i++) {
        slot[i] = values[i];
    }

    if (plot.visible_count == 0 || plot.visible_count > plot.count) {
        plot.visible_count = plot.count;
    }
    if (plot.follow && plot.visible_count < plot.count) {
        plot.view_start = plot.count - plot.visible_count;
    }
}

static void plot_apply_live_window(void)
{
    if (plot.mode != PLOT_MODE_LIVE || !plot.follow || plot.count == 0) {
        return;
    }

    const size_t latest_index = plot_physical_index(plot.count - 1U);
    const uint64_t latest_x = plot.x[latest_index];
    const uint64_t min_x = latest_x > plot.live_window_ms ?
        latest_x - plot.live_window_ms :
        0U;

    size_t start = 0;
    while (start + 1U < plot.count) {
        const size_t physical = plot_physical_index(start);
        if (plot.x[physical] >= min_x) {
            break;
        }
        start++;
    }

    plot.view_start = start;
    plot.visible_count = plot.count - start;
    if (plot.visible_count == 0) {
        plot.view_start = plot.count - 1U;
        plot.visible_count = 1U;
    }
}

static bool plot_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool plot_parse_float(const char *text, float *value)
{
    if (text == NULL || value == NULL) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (*text == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const float parsed = strtof(text, &end);
    if (errno != 0 || end == text || !isfinite(parsed)) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static bool plot_parse_u64_cell(const char *text, uint64_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static void plot_discard_line_remainder(FILE *file, const char *line)
{
    if (file == NULL || line == NULL) {
        return;
    }
    const size_t len = strlen(line);
    if (len > 0 && line[len - 1U] == '\n') {
        return;
    }
    int ch = 0;
    do {
        ch = fgetc(file);
    } while (ch != EOF && ch != '\n');
}

static bool plot_read_line(FILE *file, char *line, size_t line_len)
{
    if (file == NULL || line == NULL || line_len == 0) {
        return false;
    }
    if (fgets(line, (int)line_len, file) == NULL) {
        line[0] = '\0';
        return false;
    }
    plot_discard_line_remainder(file, line);
    return true;
}

static bool plot_csv_parse_line(const char *line,
                                char cells[PLOT_MAX_COLS][PLOT_CELL_MAX],
                                size_t *cell_count)
{
    if (line == NULL || cells == NULL || cell_count == NULL) {
        return false;
    }

    memset(cells, 0, PLOT_MAX_COLS * PLOT_CELL_MAX);
    size_t col = 0;
    size_t pos = 0;
    bool quoted = false;
    bool quote_closed = false;

    for (const char *p = line; *p != '\0'; p++) {
        const char ch = *p;
        if (!quoted && (ch == '\r' || ch == '\n')) {
            break;
        }
        if (col >= PLOT_MAX_COLS) {
            break;
        }
        if (quoted) {
            if (ch == '"') {
                if (p[1] == '"') {
                    if (pos + 1U < PLOT_CELL_MAX) {
                        cells[col][pos++] = '"';
                    }
                    p++;
                } else {
                    quoted = false;
                    quote_closed = true;
                }
            } else if (pos + 1U < PLOT_CELL_MAX) {
                cells[col][pos++] = ch;
            }
            continue;
        }
        if (ch == ',') {
            cells[col][pos] = '\0';
            col++;
            pos = 0;
            quote_closed = false;
            continue;
        }
        if (ch == '"' && pos == 0 && !quote_closed) {
            quoted = true;
            continue;
        }
        if (quote_closed && ch != ' ' && ch != '\t') {
            return false;
        }
        if (!quote_closed && pos + 1U < PLOT_CELL_MAX) {
            cells[col][pos++] = ch;
        }
    }

    if (quoted) {
        return false;
    }
    if (col < PLOT_MAX_COLS) {
        cells[col][pos] = '\0';
        col++;
    }
    *cell_count = col;
    return true;
}

static bool plot_header_is_meta(const char *name)
{
    return strcmp(name, "time_ms") == 0 ||
        strcmp(name, "uptime_ms") == 0 ||
        strcmp(name, "stream") == 0;
}

static bool plot_column_matches_filter(const char *name, const char *filter)
{
    if (name == NULL || filter == NULL || filter[0] == '\0') {
        return false;
    }
    if (strcmp(name, filter) == 0) {
        return true;
    }
    const size_t filter_len = strlen(filter);
    return strncmp(name, filter, filter_len) == 0 &&
        (name[filter_len] == '_' || name[filter_len] == '\0');
}

static bool plot_column_selected(const char *name, int filter_count, const char **filters)
{
    if (plot_header_is_meta(name)) {
        return false;
    }
    if (filter_count == 0) {
        return true;
    }
    for (int i = 0; i < filter_count; i++) {
        if (plot_column_matches_filter(name, filters[i])) {
            return true;
        }
    }
    return false;
}

static void plot_add_series(const char *name, size_t csv_col)
{
    if (plot.series_count >= PLOT_MAX_SERIES) {
        return;
    }

    static const solar_os_gfx_color_t colors[] = {
        SOLAR_OS_GFX_COLOR_BLACK,
        SOLAR_OS_GFX_COLOR_DARK,
        SOLAR_OS_GFX_COLOR_BLACK,
        SOLAR_OS_GFX_COLOR_DARK,
        SOLAR_OS_GFX_COLOR_BLACK,
        SOLAR_OS_GFX_COLOR_DARK,
    };
    plot_series_t *series = &plot.series[plot.series_count];
    strlcpy(series->name, name != NULL && name[0] ? name : "value", sizeof(series->name));
    series->csv_col = csv_col;
    series->color = colors[plot.series_count % (sizeof(colors) / sizeof(colors[0]))];
    plot.series_count++;
}

static esp_err_t plot_load_csv(const char *path, int filter_count, const char **filters)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return ESP_FAIL;
    }

    plot.series_count = 0;
    plot.count = 0;
    plot.start = 0;
    plot.view_start = 0;
    plot.visible_count = 0;

    if (!plot_read_line(file, plot.line, sizeof(plot.line))) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    char headers[PLOT_MAX_COLS][PLOT_CELL_MAX];
    size_t col_count = 0;
    if (!plot_csv_parse_line(plot.line, headers, &col_count)) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t x_col = SIZE_MAX;
    for (size_t i = 0; i < col_count; i++) {
        if (strcmp(headers[i], "time_ms") == 0) {
            x_col = i;
            break;
        }
        if (x_col == SIZE_MAX && strcmp(headers[i], "uptime_ms") == 0) {
            x_col = i;
        }
    }

    for (size_t i = 0; i < col_count && plot.series_count < PLOT_MAX_SERIES; i++) {
        if (plot_column_selected(headers[i], filter_count, filters)) {
            plot_add_series(headers[i], i);
        }
    }

    if (plot.series_count == 0) {
        fclose(file);
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t row_index = 0;
    while (plot_read_line(file, plot.line, sizeof(plot.line))) {
        size_t cells = 0;
        if (!plot_csv_parse_line(plot.line, plot.cells, &cells)) {
            continue;
        }

        float values[PLOT_MAX_SERIES];
        for (size_t i = 0; i < PLOT_MAX_SERIES; i++) {
            values[i] = NAN;
        }
        for (size_t i = 0; i < plot.series_count; i++) {
            const size_t col = plot.series[i].csv_col;
            if (col < cells) {
                (void)plot_parse_float(plot.cells[col], &values[i]);
            }
        }

        uint64_t x = row_index;
        if (x_col != SIZE_MAX && x_col < cells) {
            (void)plot_parse_u64_cell(plot.cells[x_col], &x);
        }
        plot_append_sample(x, values);
        row_index++;
    }

    fclose(file);
    plot.visible_count = plot.count;
    plot.view_start = 0;
    plot.follow = false;
    snprintf(plot.message, sizeof(plot.message), "%u samples", (unsigned)plot.count);
    return plot.count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool plot_stream_is_scalar(const char *id, solar_os_stream_info_t *info)
{
    solar_os_stream_info_t local_info;
    if (solar_os_stream_get_info(id, &local_info) != ESP_OK ||
        local_info.type != SOLAR_OS_STREAM_TYPE_SCALAR) {
        return false;
    }
    if (info != NULL) {
        *info = local_info;
    }
    return true;
}

static bool plot_live_primary_column(const char *id,
                                     char header_cells[PLOT_MAX_COLS][PLOT_CELL_MAX],
                                     size_t col_count,
                                     size_t *column)
{
    const char *preferred = "value";

    if (strcmp(id, "battery") == 0) {
        preferred = "voltage_v";
    } else if (strncmp(id, "adc", 3) == 0) {
        preferred = "mv";
    } else if (strncmp(id, "mic", 3) == 0) {
        preferred = "peak_percent";
    } else if (strcmp(id, "temperature") == 0) {
        preferred = "temperature_c";
    } else if (strcmp(id, "humidity") == 0) {
        preferred = "humidity_percent";
    }

    for (size_t col = 0; col < col_count; col++) {
        if (strcmp(header_cells[col], preferred) == 0) {
            *column = col;
            return true;
        }
    }

    for (size_t col = 0; col < col_count; col++) {
        if (!plot_header_is_meta(header_cells[col])) {
            *column = col;
            return true;
        }
    }

    return false;
}

static esp_err_t plot_add_live_stream(const char *id, const solar_os_stream_info_t *info)
{
    if (plot.live_count >= PLOT_MAX_LIVE_STREAMS || info == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (plot.series_count >= PLOT_MAX_SERIES) {
        return ESP_ERR_NO_MEM;
    }

    plot_live_stream_t *live = &plot.live[plot.live_count];
    memset(live, 0, sizeof(*live));
    live->handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    live->info = *info;
    live->first_series = plot.series_count;

    esp_err_t err = solar_os_stream_open(id, "plot", &live->handle);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "stream open failed: %s: %s", id, esp_err_to_name(err));
        return err;
    }

    char header[SOLAR_OS_STREAM_CSV_HEADER_MAX];
    err = solar_os_stream_csv_header(info, header, sizeof(header));
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "stream header failed: %s: %s", id, esp_err_to_name(err));
        solar_os_stream_close(&live->handle);
        return err;
    }

    char header_cells[PLOT_MAX_COLS][PLOT_CELL_MAX];
    size_t col_count = 0;
    if (!plot_csv_parse_line(header, header_cells, &col_count)) {
        SOLAR_OS_LOGW(TAG, "stream header parse failed: %s", id);
        solar_os_stream_close(&live->handle);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t value_col = 0;
    if (!plot_live_primary_column(id, header_cells, col_count, &value_col)) {
        solar_os_stream_close(&live->handle);
        return ESP_ERR_NOT_FOUND;
    }

    live->value_col[0] = value_col;
    live->series_count = 1;
    plot_add_series(id, value_col);

    plot.live_count++;
    return ESP_OK;
}

static void plot_close_live_streams(void)
{
    for (size_t i = 0; i < plot.live_count; i++) {
        solar_os_stream_close(&plot.live[i].handle);
    }
    plot.live_count = 0;
}

static esp_err_t plot_start_live(int stream_count, const char **streams, uint32_t rate_ms)
{
    plot.mode = PLOT_MODE_LIVE;
    plot.series_count = 0;
    plot.count = 0;
    plot.start = 0;
    plot.view_start = 0;
    plot.visible_count = 0;
    plot.follow = true;
    plot.paused = false;
    plot.rate_ms = rate_ms;
    plot.live_window_ms = PLOT_DEFAULT_WINDOW_MS;
    plot.next_sample_ms = 0;
    plot_close_live_streams();

    for (int i = 0; i < stream_count; i++) {
        solar_os_stream_info_t info;
        if (!plot_stream_is_scalar(streams[i], &info)) {
            SOLAR_OS_LOGW(TAG, "not a scalar stream: %s", streams[i]);
            return ESP_ERR_INVALID_ARG;
        }
        const esp_err_t err = plot_add_live_stream(streams[i], &info);
        if (err != ESP_OK) {
            return err;
        }
    }

    strlcpy(plot.title, "live", sizeof(plot.title));
    plot_set_message("live");
    return plot.series_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool plot_parse_args(solar_os_context_t *ctx,
                            char *csv_path,
                            size_t csv_path_len,
                            bool *csv_mode,
                            int *csv_filter_count,
                            const char **csv_filters,
                            int *live_count,
                            const char **live_streams,
                            uint32_t *rate_ms)
{
    const int argc = solar_os_context_argc(ctx);
    csv_path[0] = '\0';
    *csv_mode = false;
    *csv_filter_count = 0;
    *live_count = 0;
    *rate_ms = PLOT_DEFAULT_RATE_MS;

    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (arg == NULL) {
            continue;
        }
        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--file") == 0) {
            if (*csv_mode || *live_count > 0 || ++i >= argc) {
                return false;
            }
            const char *path_arg = solar_os_context_argv(ctx, i);
            if (path_arg == NULL ||
                solar_os_storage_resolve_path(path_arg, csv_path, csv_path_len) != ESP_OK) {
                return false;
            }
            *csv_mode = true;
            continue;
        }
        if (strcmp(arg, "--rate") == 0 || strcmp(arg, "--rate-ms") == 0) {
            if (++i >= argc ||
                !plot_parse_u32(solar_os_context_argv(ctx, i),
                                PLOT_MIN_RATE_MS,
                                PLOT_MAX_RATE_MS,
                                rate_ms)) {
                return false;
            }
            continue;
        }
        if (arg[0] == '-') {
            return false;
        }
        if (*csv_mode) {
            if (*csv_filter_count >= SOLAR_OS_APP_ARG_MAX - 1) {
                return false;
            }
            csv_filters[*csv_filter_count] = arg;
            (*csv_filter_count)++;
        } else {
            if (*live_count >= SOLAR_OS_APP_ARG_MAX - 1 || !plot_stream_is_scalar(arg, NULL)) {
                return false;
            }
            live_streams[*live_count] = arg;
            (*live_count)++;
        }
    }

    return *csv_mode ? csv_path[0] != '\0' : *live_count > 0;
}

static void plot_update_view_after_zoom(size_t new_visible)
{
    if (plot.count == 0) {
        plot.visible_count = 0;
        plot.view_start = 0;
        return;
    }
    if (new_visible < 8U) {
        new_visible = plot.count < 8U ? plot.count : 8U;
    }
    if (new_visible > plot.count) {
        new_visible = plot.count;
    }

    const size_t center = plot.view_start + (plot.visible_count / 2U);
    plot.visible_count = new_visible;
    if (plot.follow) {
        plot.view_start = plot.count > plot.visible_count ? plot.count - plot.visible_count : 0;
    } else if (center > plot.visible_count / 2U) {
        plot.view_start = center - (plot.visible_count / 2U);
    } else {
        plot.view_start = 0;
    }
    if (plot.view_start + plot.visible_count > plot.count) {
        plot.view_start = plot.count - plot.visible_count;
    }
}

static void plot_reset_view(void)
{
    if (plot.mode == PLOT_MODE_LIVE) {
        plot.live_window_ms = PLOT_DEFAULT_WINDOW_MS;
        plot.follow = true;
        plot_apply_live_window();
    } else {
        plot.visible_count = plot.count;
        plot.view_start = 0;
        plot.follow = false;
    }
    plot.autoscale = true;
}

static void plot_adjust_live_window(bool zoom_in)
{
    if (zoom_in) {
        uint32_t next = plot.live_window_ms / 2U;
        if (next < PLOT_MIN_WINDOW_MS) {
            next = PLOT_MIN_WINDOW_MS;
        }
        plot.live_window_ms = next;
    } else {
        uint32_t next = plot.live_window_ms > PLOT_MAX_WINDOW_MS / 2U ?
            PLOT_MAX_WINDOW_MS :
            plot.live_window_ms * 2U;
        if (next > PLOT_MAX_WINDOW_MS) {
            next = PLOT_MAX_WINDOW_MS;
        }
        plot.live_window_ms = next;
    }

    plot.follow = true;
    plot_apply_live_window();
}

static void plot_visible_range(float *min_value, float *max_value)
{
    float min_v = INFINITY;
    float max_v = -INFINITY;

    const size_t end = plot.view_start + plot.visible_count;
    for (size_t i = plot.view_start; i < end && i < plot.count; i++) {
        const size_t physical = plot_physical_index(i);
        const float *slot = plot_y_slot(physical);
        for (size_t s = 0; s < plot.series_count; s++) {
            const float value = slot[s];
            if (!isfinite(value)) {
                continue;
            }
            if (value < min_v) {
                min_v = value;
            }
            if (value > max_v) {
                max_v = value;
            }
        }
    }

    if (!isfinite(min_v) || !isfinite(max_v)) {
        min_v = 0.0f;
        max_v = 1.0f;
    } else if (fabsf(max_v - min_v) < 0.001f) {
        min_v -= 1.0f;
        max_v += 1.0f;
    }
    *min_value = min_v;
    *max_value = max_v;
}

static void plot_visible_x_range(uint64_t *min_x, uint64_t *max_x)
{
    if (plot.count == 0 || plot.visible_count == 0) {
        *min_x = 0;
        *max_x = 1;
        return;
    }

    const size_t first = plot_physical_index(plot.view_start);
    const size_t last = plot_physical_index(plot.view_start + plot.visible_count - 1U);
    uint64_t start_x = plot.x[first];
    uint64_t end_x = plot.x[last];
    if (end_x <= start_x) {
        end_x = start_x + 1U;
    }
    *min_x = start_x;
    *max_x = end_x;
}

static int plot_map_y(float value, float min_v, float max_v, int top, int height)
{
    if (!isfinite(value) || height <= 1) {
        return top + height - 1;
    }
    float t = (value - min_v) / (max_v - min_v);
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return top + height - 1 - (int)((float)(height - 1) * t);
}

static void plot_draw_series(solar_os_gfx_t *gfx,
                             size_t series_index,
                             int left,
                             int top,
                             int width,
                             int height,
                             float min_v,
                             float max_v,
                             uint64_t min_x,
                             uint64_t max_x)
{
    if (plot.visible_count < 2 || series_index >= plot.series_count) {
        return;
    }

    solar_os_gfx_set_color(gfx, plot.series[series_index].color);
    bool have_prev = false;
    int prev_x = 0;
    int prev_y = 0;
    const uint64_t span_x = max_x > min_x ? max_x - min_x : 1U;
    const size_t end = plot.view_start + plot.visible_count;
    for (size_t i = plot.view_start; i < end && i < plot.count; i++) {
        const size_t physical = plot_physical_index(i);
        const float value = plot_y_slot(physical)[series_index];
        if (!isfinite(value)) {
            have_prev = false;
            continue;
        }

        const uint64_t sample_x = plot.x[physical] > min_x ? plot.x[physical] - min_x : 0U;
        const int x = left + (int)((sample_x * (uint64_t)(width - 1)) / span_x);
        const int y = plot_map_y(value, min_v, max_v, top, height);
        if (have_prev) {
            solar_os_gfx_line(gfx, prev_x, prev_y, x, y);
        }
        if (series_index == plot.active_series) {
            solar_os_gfx_fill_rect(gfx, x - 1, y - 1, 3, 3);
        }
        prev_x = x;
        prev_y = y;
        have_prev = true;
    }
}

static void plot_draw_grid(solar_os_gfx_t *gfx, int left, int top, int width, int height)
{
    const int divisions = 4;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DOTTED);
    for (int i = 1; i < divisions; i++) {
        const int y = top + ((height - 1) * i) / divisions;
        solar_os_gfx_line(gfx, left + 1, y, left + width - 1, y);
    }
    for (int i = 1; i < divisions; i++) {
        const int x = left + ((width - 1) * i) / divisions;
        solar_os_gfx_line(gfx, x, top, x, top + height - 2);
    }
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
}

static void plot_format_float(char *buffer, size_t buffer_len, float value)
{
    if (fabsf(value) >= 1000.0f || fabsf(value) < 0.01f) {
        snprintf(buffer, buffer_len, "%.2g", value);
    } else {
        snprintf(buffer, buffer_len, "%.2f", value);
    }
}

static void plot_format_window(char *buffer, size_t buffer_len, uint32_t window_ms)
{
    if (window_ms < 60000U) {
        snprintf(buffer, buffer_len, "%us", (unsigned)((window_ms + 999U) / 1000U));
    } else if (window_ms < 3600000U) {
        snprintf(buffer, buffer_len, "%um", (unsigned)((window_ms + 59999U) / 60000U));
    } else {
        snprintf(buffer, buffer_len, "%uh", (unsigned)((window_ms + 3599999U) / 3600000U));
    }
}

static void plot_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);
    const int left = 42;
    const int top = 22;
    const int right_pad = 4;
    const int bottom_pad = 22;
    const int plot_w = screen_w - left - right_pad;
    const int plot_h = screen_h - top - bottom_pad;

    plot_apply_live_window();

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    char window[16] = "";
    if (plot.mode == PLOT_MODE_LIVE) {
        plot_format_window(window, sizeof(window), plot.live_window_ms);
    }
    char title[96];
    if (plot.mode == PLOT_MODE_LIVE) {
        snprintf(title,
                 sizeof(title),
                 "live %s %s%u",
                 window,
                 plot.paused ? "paused " : "",
                 (unsigned)plot.count);
    } else {
        snprintf(title,
                 sizeof(title),
                 "%s %s%u",
                 plot.title,
                 plot.paused ? "paused " : "",
                 (unsigned)plot.count);
    }
    solar_os_gfx_text(gfx, 2, 10, title);

    if (plot.count == 0 || plot.series_count == 0 || plot_w <= 8 || plot_h <= 8) {
        solar_os_gfx_text(gfx, 2, screen_h / 2, plot.message[0] ? plot.message : "no data");
        solar_os_gfx_present(gfx);
        return;
    }

    float min_v = 0.0f;
    float max_v = 1.0f;
    plot_visible_range(&min_v, &max_v);
    uint64_t min_x = 0;
    uint64_t max_x = 1;
    plot_visible_x_range(&min_x, &max_x);

    char label[24];
    plot_format_float(label, sizeof(label), max_v);
    solar_os_gfx_text(gfx, 2, top + 8, label);
    plot_format_float(label, sizeof(label), min_v);
    solar_os_gfx_text(gfx, 2, top + plot_h - 2, label);

    plot_draw_grid(gfx, left, top, plot_w, plot_h);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, left, top, left, top + plot_h - 1);
    solar_os_gfx_line(gfx, left, top + plot_h - 1, left + plot_w - 1, top + plot_h - 1);

    for (size_t s = 0; s < plot.series_count; s++) {
        plot_draw_series(gfx, s, left, top, plot_w, plot_h, min_v, max_v, min_x, max_x);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const char *active = plot.active_series < plot.series_count ?
        plot.series[plot.active_series].name :
        "";
    char footer[96];
    snprintf(footer,
             sizeof(footer),
             "%s  %u-%u/%u",
             active,
             (unsigned)(plot.view_start + 1U),
             (unsigned)(plot.view_start + plot.visible_count),
             (unsigned)plot.count);
    solar_os_gfx_text(gfx, 2, screen_h - 3, footer);
    if (plot.message[0]) {
        solar_os_gfx_text(gfx, screen_w / 2, 10, plot.message);
    }

    solar_os_gfx_present(gfx);
}

static void plot_live_sample(void)
{
    float values[PLOT_MAX_SERIES];
    for (size_t i = 0; i < PLOT_MAX_SERIES; i++) {
        values[i] = NAN;
    }

    for (size_t i = 0; i < plot.live_count; i++) {
        plot_live_stream_t *live = &plot.live[i];
        solar_os_stream_csv_record_t record;
        const solar_os_stream_read_options_t options = {
            .window_ms = live->info.id[0] == 'm' ? 100U : 0U,
            .timeout_ms = 0U,
        };
        const esp_err_t err = solar_os_stream_read_csv(&live->handle, &options, &record);
        if (err != ESP_OK || !record.has_data) {
            continue;
        }

        size_t cells = 0;
        if (!plot_csv_parse_line(record.line, plot.cells, &cells)) {
            continue;
        }
        for (size_t s = 0; s < live->series_count; s++) {
            const size_t series_index = live->first_series + s;
            const size_t col = live->value_col[s];
            if (series_index < PLOT_MAX_SERIES && col < cells) {
                (void)plot_parse_float(plot.cells[col], &values[series_index]);
            }
        }
    }

    plot_append_sample(solar_os_time_uptime_ms(), values);
}

static esp_err_t plot_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&plot, 0, sizeof(plot));
    plot.autoscale = true;
    plot.rate_ms = PLOT_DEFAULT_RATE_MS;
    if (!plot_alloc_buffers(PLOT_DEFAULT_CAPACITY)) {
        return ESP_ERR_NO_MEM;
    }

    char csv_path[SOLAR_OS_STORAGE_PATH_MAX] = {0};
    bool csv_mode = false;
    const char *csv_filters[SOLAR_OS_APP_ARG_MAX] = {0};
    int csv_filter_count = 0;
    const char *live_streams[SOLAR_OS_APP_ARG_MAX] = {0};
    int live_count = 0;
    uint32_t rate_ms = PLOT_DEFAULT_RATE_MS;
    if (!plot_parse_args(ctx,
                         csv_path,
                         sizeof(csv_path),
                         &csv_mode,
                         &csv_filter_count,
                         csv_filters,
                         &live_count,
                         live_streams,
                         &rate_ms)) {
        plot_print_usage(ctx, "invalid arguments");
        plot_free_buffers();
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    if (!csv_mode) {
        err = plot_start_live(live_count, live_streams, rate_ms);
    } else {
        plot.mode = PLOT_MODE_CSV;
        strlcpy(plot.title, csv_path, sizeof(plot.title));
        err = plot_load_csv(csv_path, csv_filter_count, csv_filters);
    }
    if (err != ESP_OK) {
        solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
        if (io != NULL) {
            solar_os_shell_io_printf(io, "plot: start failed: %s\n", esp_err_to_name(err));
            solar_os_shell_io_flush(io);
            solar_os_context_request_terminal_preserve(ctx);
        }
        plot_close_live_streams();
        plot_free_buffers();
        return err;
    }

    solar_os_context_set_graphics_active(ctx, true);
    plot_render(ctx);
    plot.running = true;
    return ESP_OK;
}

static void plot_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    plot.running = false;
    plot_close_live_streams();
    plot_free_buffers();
    solar_os_context_set_graphics_active(ctx, false);
}

static void plot_pan_left(void)
{
    plot.follow = false;
    const size_t step = plot.visible_count > 20U ? plot.visible_count / 5U : 1U;
    plot.view_start = plot.view_start > step ? plot.view_start - step : 0;
}

static void plot_pan_right(void)
{
    const size_t step = plot.visible_count > 20U ? plot.visible_count / 5U : 1U;
    if (plot.view_start + plot.visible_count + step >= plot.count) {
        plot.view_start = plot.count > plot.visible_count ? plot.count - plot.visible_count : 0;
        plot.follow = plot.mode == PLOT_MODE_LIVE;
    } else {
        plot.view_start += step;
        plot.follow = false;
    }
}

static bool plot_handle_char(solar_os_context_t *ctx, char ch)
{
    const uint8_t key = (uint8_t)ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_LEFT:
        plot_pan_left();
        break;
    case SOLAR_OS_KEY_RIGHT:
        plot_pan_right();
        break;
    case SOLAR_OS_KEY_UP:
        if (plot.active_series > 0) {
            plot.active_series--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (plot.active_series + 1U < plot.series_count) {
            plot.active_series++;
        }
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        if (plot.mode == PLOT_MODE_LIVE) {
            plot_adjust_live_window(true);
        } else {
            plot_update_view_after_zoom(plot.visible_count / 2U);
            plot.follow = false;
        }
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        if (plot.mode == PLOT_MODE_LIVE) {
            plot_adjust_live_window(false);
        } else {
            plot_update_view_after_zoom(plot.visible_count * 2U);
        }
        break;
    default:
        if (ch == '+' || ch == '=') {
            if (plot.mode == PLOT_MODE_LIVE) {
                plot_adjust_live_window(true);
            } else {
                plot_update_view_after_zoom(plot.visible_count / 2U);
                plot.follow = false;
            }
        } else if (ch == '-') {
            if (plot.mode == PLOT_MODE_LIVE) {
                plot_adjust_live_window(false);
            } else {
                plot_update_view_after_zoom(plot.visible_count * 2U);
            }
        } else if (ch == 'a' || ch == 'A' || ch == 'r' || ch == 'R') {
            plot_reset_view();
        } else if (ch == ' ' && plot.mode == PLOT_MODE_LIVE) {
            plot.paused = !plot.paused;
            plot.follow = !plot.paused;
        } else {
            return false;
        }
        break;
    }

    plot_render(ctx);
    return true;
}

static bool plot_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return plot_handle_char(ctx, event->data.ch);
    }

    if (event->type == SOLAR_OS_EVENT_TICK &&
        plot.mode == PLOT_MODE_LIVE &&
        !plot.paused) {
        if (plot.next_sample_ms == 0 ||
            (int32_t)(event->data.tick_ms - plot.next_sample_ms) >= 0) {
            plot.next_sample_ms = event->data.tick_ms + plot.rate_ms;
            plot_live_sample();
            plot_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        plot_render(ctx);
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_plot_app = {
    .name = "plot",
    .summary = "plot DAQ CSV files or scalar streams",
    .start = plot_start,
    .stop = plot_stop,
    .event = plot_event,
};
