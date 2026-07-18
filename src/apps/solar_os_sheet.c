#include "solar_os_sheet.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_tui.h"

#define SHEET_MAX_COLS 32U
#define SHEET_CELL_MAX 72U
#define SHEET_LINE_MAX 1024U
#define SHEET_MESSAGE_MAX 96U
#define SHEET_FORMULA_LEN 96U
#define SHEET_ROWNUM_WIDTH 6U
#define SHEET_CELL_WIDTH 14U
#define SHEET_OFFSET_INITIAL 256U

typedef enum {
    SHEET_INPUT_NORMAL,
    SHEET_INPUT_FORMULA,
} sheet_input_mode_t;

typedef enum {
    SHEET_FORMULA_SUM,
    SHEET_FORMULA_AVG,
    SHEET_FORMULA_MIN,
    SHEET_FORMULA_MAX,
    SHEET_FORMULA_COUNT,
    SHEET_FORMULA_DELTA,
    SHEET_FORMULA_RATE,
} sheet_formula_t;

typedef struct {
    bool running;
    bool error_only;
    sheet_input_mode_t input_mode;
    solar_os_tui_t tui;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char headers[SHEET_MAX_COLS][SHEET_CELL_MAX];
    size_t col_count;
    long *row_offsets;
    size_t row_count;
    size_t row_capacity;
    size_t cursor_row;
    size_t cursor_col;
    size_t row_offset;
    size_t col_offset;
    char message[SHEET_MESSAGE_MAX];
    char formula[SHEET_FORMULA_LEN];
    size_t formula_len;
    char line[SHEET_LINE_MAX];
    char cells[SHEET_MAX_COLS][SHEET_CELL_MAX];
} sheet_state_t;

static sheet_state_t *sheet_state;
#define sheet (*sheet_state)

static sheet_state_t *sheet_alloc_state(void)
{
    return solar_os_memory_calloc(1,
                                  sizeof(sheet_state_t),
                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                  "sheet.state");
}

static bool sheet_is_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static void sheet_set_message(const char *message)
{
    strlcpy(sheet.message, message != NULL ? message : "", sizeof(sheet.message));
}

static void *sheet_realloc(void *ptr, size_t bytes)
{
    return solar_os_memory_realloc(ptr,
                                   bytes,
                                   SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                   "sheet.rows");
}

static bool sheet_ensure_offset_capacity(size_t needed)
{
    if (needed <= sheet.row_capacity) {
        return true;
    }

    size_t next_capacity = sheet.row_capacity > 0 ? sheet.row_capacity : SHEET_OFFSET_INITIAL;
    while (next_capacity < needed) {
        next_capacity *= 2U;
    }

    long *next = sheet_realloc(sheet.row_offsets, next_capacity * sizeof(sheet.row_offsets[0]));
    if (next == NULL) {
        return false;
    }

    sheet.row_offsets = next;
    sheet.row_capacity = next_capacity;
    return true;
}

static void sheet_free_offsets(void)
{
    if (sheet.row_offsets != NULL) {
        solar_os_memory_free(sheet.row_offsets);
        sheet.row_offsets = NULL;
    }
    sheet.row_count = 0;
    sheet.row_capacity = 0;
}

static void sheet_discard_line_remainder(FILE *file, const char *line)
{
    if (file == NULL || line == NULL) {
        return;
    }

    const size_t len = strlen(line);
    if (len > 0 && line[len - 1U] == '\n') {
        return;
    }
    if (feof(file)) {
        return;
    }

    int ch = 0;
    do {
        ch = fgetc(file);
    } while (ch != EOF && ch != '\n');
}

static bool sheet_read_line(FILE *file, char *line, size_t line_len)
{
    if (file == NULL || line == NULL || line_len == 0) {
        return false;
    }

    if (fgets(line, (int)line_len, file) == NULL) {
        line[0] = '\0';
        return false;
    }
    sheet_discard_line_remainder(file, line);
    return true;
}

static bool sheet_csv_parse_line(const char *line,
                                 char cells[SHEET_MAX_COLS][SHEET_CELL_MAX],
                                 size_t *cell_count)
{
    if (line == NULL || cells == NULL || cell_count == NULL) {
        return false;
    }

    memset(cells, 0, SHEET_MAX_COLS * SHEET_CELL_MAX);
    size_t col = 0;
    size_t pos = 0;
    bool quoted = false;
    bool quote_closed = false;

    for (const char *p = line; *p != '\0'; p++) {
        const char ch = *p;
        if (!quoted && (ch == '\r' || ch == '\n')) {
            break;
        }

        if (col >= SHEET_MAX_COLS) {
            break;
        }

        if (quoted) {
            if (ch == '"') {
                if (p[1] == '"') {
                    if (pos + 1U < SHEET_CELL_MAX) {
                        cells[col][pos++] = '"';
                    }
                    p++;
                } else {
                    quoted = false;
                    quote_closed = true;
                }
            } else if (pos + 1U < SHEET_CELL_MAX) {
                cells[col][pos++] = ch;
            }
            continue;
        }

        if (ch == ',' && (!quoted || quote_closed)) {
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
        if (!quote_closed && pos + 1U < SHEET_CELL_MAX) {
            cells[col][pos++] = ch;
        }
    }

    if (col < SHEET_MAX_COLS) {
        cells[col][pos] = '\0';
        col++;
    }
    *cell_count = col;
    return true;
}

static bool sheet_read_row(size_t row, char cells[SHEET_MAX_COLS][SHEET_CELL_MAX], size_t *cell_count)
{
    if (row >= sheet.row_count || cells == NULL || cell_count == NULL) {
        return false;
    }

    FILE *file = fopen(sheet.path, "r");
    if (file == NULL) {
        return false;
    }

    bool ok = fseek(file, sheet.row_offsets[row], SEEK_SET) == 0 &&
        sheet_read_line(file, sheet.line, sizeof(sheet.line)) &&
        sheet_csv_parse_line(sheet.line, cells, cell_count);
    fclose(file);
    return ok;
}

static bool sheet_line_has_content(const char *line)
{
    if (line == NULL) {
        return false;
    }
    for (const char *p = line; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            break;
        }
        if (!isspace((unsigned char)*p)) {
            return true;
        }
    }
    return false;
}

static esp_err_t sheet_index_file(void)
{
    struct stat st;
    if (stat(sheet.path, &st) != 0 || !S_ISREG(st.st_mode)) {
        sheet_set_message("not a file");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(sheet.path, "r");
    if (file == NULL) {
        char message[SHEET_MESSAGE_MAX];
        snprintf(message, sizeof(message), "open failed: %s", strerror(errno));
        sheet_set_message(message);
        return ESP_FAIL;
    }

    if (!sheet_read_line(file, sheet.line, sizeof(sheet.line)) ||
        !sheet_csv_parse_line(sheet.line, sheet.headers, &sheet.col_count) ||
        sheet.col_count == 0) {
        fclose(file);
        sheet_set_message("empty csv");
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (!feof(file)) {
        const long offset = ftell(file);
        if (offset < 0) {
            fclose(file);
            sheet_set_message("index failed");
            return ESP_FAIL;
        }
        if (!sheet_read_line(file, sheet.line, sizeof(sheet.line))) {
            break;
        }
        if (!sheet_line_has_content(sheet.line)) {
            continue;
        }
        if (!sheet_ensure_offset_capacity(sheet.row_count + 1U)) {
            fclose(file);
            sheet_set_message("out of memory");
            return ESP_ERR_NO_MEM;
        }
        sheet.row_offsets[sheet.row_count++] = offset;
    }

    fclose(file);
    return ESP_OK;
}

static size_t sheet_utf8_char_len(unsigned char ch)
{
    if (ch < 0x80U) {
        return 1;
    }
    if ((ch & 0xe0U) == 0xc0U) {
        return 2;
    }
    if ((ch & 0xf0U) == 0xe0U) {
        return 3;
    }
    if ((ch & 0xf8U) == 0xf0U) {
        return 4;
    }
    return 1;
}

static void sheet_clip_text(const char *text, size_t cells, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (text == NULL || cells == 0) {
        return;
    }

    size_t in = 0;
    size_t out_pos = 0;
    size_t used_cells = 0;
    while (text[in] != '\0' && used_cells < cells && out_pos + 1U < out_len) {
        size_t char_len = sheet_utf8_char_len((unsigned char)text[in]);
        if (char_len == 0 || out_pos + char_len >= out_len) {
            break;
        }
        for (size_t i = 1; i < char_len; i++) {
            if (((unsigned char)text[in + i] & 0xc0U) != 0x80U) {
                char_len = 1;
                break;
            }
        }
        if (char_len == 1 && !sheet_is_printable((uint8_t)text[in])) {
            out[out_pos++] = '.';
            in++;
        } else {
            memcpy(&out[out_pos], &text[in], char_len);
            out_pos += char_len;
            in += char_len;
        }
        used_cells++;
    }
    out[out_pos] = '\0';
}

static void sheet_write_cell(size_t row,
                             size_t col,
                             size_t width,
                             const char *text,
                             uint8_t attr)
{
    const size_t rows = solar_os_tui_rows(&sheet.tui);
    const size_t cols = solar_os_tui_cols(&sheet.tui);
    char clipped[SHEET_LINE_MAX];

    if (row >= rows || col >= cols || width == 0) {
        return;
    }
    if (col + width > cols) {
        width = cols - col;
    }
    if (width == 0) {
        return;
    }

    solar_os_tui_fill(&sheet.tui, row, col, 1, width, ' ', attr);
    sheet_clip_text(text, width, clipped, sizeof(clipped));
    if (clipped[0] != '\0') {
        solar_os_tui_addstr(&sheet.tui, row, col, clipped, attr);
    }
}

static void sheet_write_line(size_t row, const char *text, uint8_t attr)
{
    sheet_write_cell(row, 0, solar_os_tui_cols(&sheet.tui), text, attr);
}

static size_t sheet_body_rows(void)
{
    const size_t rows = solar_os_tui_rows(&sheet.tui);
    return rows > 3U ? rows - 3U : 1U;
}

static size_t sheet_visible_cols(void)
{
    const size_t cols = solar_os_tui_cols(&sheet.tui);
    if (cols <= SHEET_ROWNUM_WIDTH + 1U) {
        return 1;
    }
    size_t visible = (cols - SHEET_ROWNUM_WIDTH) / SHEET_CELL_WIDTH;
    if (visible == 0) {
        visible = 1;
    }
    return visible;
}

static void sheet_clamp_view(void)
{
    if (sheet.col_count == 0) {
        sheet.cursor_col = 0;
    } else if (sheet.cursor_col >= sheet.col_count) {
        sheet.cursor_col = sheet.col_count - 1U;
    }
    if (sheet.row_count == 0) {
        sheet.cursor_row = 0;
    } else if (sheet.cursor_row >= sheet.row_count) {
        sheet.cursor_row = sheet.row_count - 1U;
    }

    const size_t rows = sheet_body_rows();
    if (sheet.cursor_row < sheet.row_offset) {
        sheet.row_offset = sheet.cursor_row;
    } else if (sheet.cursor_row >= sheet.row_offset + rows) {
        sheet.row_offset = sheet.cursor_row - rows + 1U;
    }

    const size_t cols = sheet_visible_cols();
    if (sheet.cursor_col < sheet.col_offset) {
        sheet.col_offset = sheet.cursor_col;
    } else if (sheet.cursor_col >= sheet.col_offset + cols) {
        sheet.col_offset = sheet.cursor_col - cols + 1U;
    }
}

static const char *sheet_current_cell_text(void)
{
    size_t count = 0;
    if (!sheet_read_row(sheet.cursor_row, sheet.cells, &count) || sheet.cursor_col >= count) {
        return "";
    }
    return sheet.cells[sheet.cursor_col];
}

static void sheet_render_footer(void)
{
    char footer[SHEET_LINE_MAX];
    if (sheet.input_mode == SHEET_INPUT_FORMULA) {
        snprintf(footer, sizeof(footer), "=%s_", sheet.formula);
    } else if (sheet.message[0] != '\0') {
        strlcpy(footer, sheet.message, sizeof(footer));
    } else {
        const char *header =
            sheet.cursor_col < sheet.col_count ? sheet.headers[sheet.cursor_col] : "";
        snprintf(footer,
                 sizeof(footer),
                 "R%u/%u C%u/%u %s=%s",
                 (unsigned)(sheet.row_count == 0 ? 0U : sheet.cursor_row + 1U),
                 (unsigned)sheet.row_count,
                 (unsigned)(sheet.col_count == 0 ? 0U : sheet.cursor_col + 1U),
                 (unsigned)sheet.col_count,
                 header,
                 sheet_current_cell_text());
    }

    const size_t rows = solar_os_tui_rows(&sheet.tui);
    if (rows > 0) {
        sheet_write_line(rows - 1U, footer, SOLAR_OS_TUI_ATTR_INVERSE);
    }
}

static void sheet_render(solar_os_context_t *ctx)
{
    (void)ctx;

    const size_t rows = solar_os_tui_rows(&sheet.tui);
    const size_t cols = solar_os_tui_cols(&sheet.tui);

    solar_os_tui_clear(&sheet.tui);
    solar_os_tui_set_cursor_visible(&sheet.tui, false);
    if (sheet.error_only) {
        sheet_write_line(0, "sheet", SOLAR_OS_TUI_ATTR_INVERSE);
        if (sheet.message[0] != '\0') {
            sheet_write_line(2, sheet.message, SOLAR_OS_TUI_ATTR_NORMAL);
        }
        sheet_write_line(3, "usage: sheet <file.csv>", SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_refresh(&sheet.tui);
        return;
    }

    sheet_clamp_view();

    char header[SHEET_LINE_MAX];
    snprintf(header,
             sizeof(header),
             "sheet %s  rows=%u cols=%u",
             sheet.display_name,
             (unsigned)sheet.row_count,
             (unsigned)sheet.col_count);
    sheet_write_line(0, header, SOLAR_OS_TUI_ATTR_INVERSE);

    if (rows < 3U || cols == 0) {
        solar_os_tui_refresh(&sheet.tui);
        return;
    }

    sheet_write_cell(1, 0, SHEET_ROWNUM_WIDTH, "#", SOLAR_OS_TUI_ATTR_BOLD);
    const size_t visible_cols = sheet_visible_cols();
    for (size_t c = 0; c < visible_cols && sheet.col_offset + c < sheet.col_count; c++) {
        const size_t col = sheet.col_offset + c;
        sheet_write_cell(1,
                         SHEET_ROWNUM_WIDTH + (c * SHEET_CELL_WIDTH),
                         SHEET_CELL_WIDTH,
                         sheet.headers[col],
                         SOLAR_OS_TUI_ATTR_BOLD);
    }

    const size_t body_rows = sheet_body_rows();
    for (size_t r = 0; r < body_rows; r++) {
        const size_t row = sheet.row_offset + r;
        if (row >= sheet.row_count) {
            break;
        }

        size_t cell_count = 0;
        if (!sheet_read_row(row, sheet.cells, &cell_count)) {
            memset(sheet.cells, 0, sizeof(sheet.cells));
            cell_count = 0;
        }

        char row_number[SHEET_ROWNUM_WIDTH + 1U];
        snprintf(row_number, sizeof(row_number), "%u", (unsigned)(row + 1U));
        sheet_write_cell(2U + r,
                         0,
                         SHEET_ROWNUM_WIDTH,
                         row_number,
                         row == sheet.cursor_row ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);

        for (size_t c = 0; c < visible_cols && sheet.col_offset + c < sheet.col_count; c++) {
            const size_t col = sheet.col_offset + c;
            const bool active = row == sheet.cursor_row && col == sheet.cursor_col;
            sheet_write_cell(2U + r,
                             SHEET_ROWNUM_WIDTH + (c * SHEET_CELL_WIDTH),
                             SHEET_CELL_WIDTH,
                             col < cell_count ? sheet.cells[col] : "",
                             active ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
        }
    }

    sheet_render_footer();
    solar_os_tui_refresh(&sheet.tui);
}

static char *sheet_trim(char *text)
{
    if (text == NULL) {
        return NULL;
    }
    while (isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static bool sheet_name_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool sheet_parse_number(const char *text, double *value)
{
    if (text == NULL || value == NULL) {
        return false;
    }

    char copy[SHEET_CELL_MAX];
    strlcpy(copy, text, sizeof(copy));
    char *trimmed = sheet_trim(copy);
    if (trimmed == NULL || *trimmed == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const double parsed = strtod(trimmed, &end);
    if (errno != 0 || end == trimmed) {
        return false;
    }
    while (end != NULL && isspace((unsigned char)*end)) {
        end++;
    }
    if (end == NULL || *end != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

static int sheet_find_col(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < sheet.col_count; i++) {
        if (sheet_name_equals(name, sheet.headers[i])) {
            return (int)i;
        }
    }
    return -1;
}

static bool sheet_parse_formula(const char *formula,
                                sheet_formula_t *kind,
                                char *arg,
                                size_t arg_len)
{
    if (formula == NULL || kind == NULL || arg == NULL || arg_len == 0) {
        return false;
    }

    char copy[SHEET_FORMULA_LEN];
    strlcpy(copy, formula, sizeof(copy));
    char *expr = sheet_trim(copy);
    if (expr == NULL || expr[0] == '\0') {
        return false;
    }
    if (expr[0] == '=') {
        expr = sheet_trim(expr + 1);
    }

    char *open = strchr(expr, '(');
    char *close = open != NULL ? strrchr(open + 1, ')') : NULL;
    if (open == NULL || close == NULL || close < open) {
        return false;
    }
    *open = '\0';
    *close = '\0';

    char *fn = sheet_trim(expr);
    char *raw_arg = sheet_trim(open + 1);
    if (fn == NULL || raw_arg == NULL || fn[0] == '\0' || raw_arg[0] == '\0') {
        return false;
    }

    if (sheet_name_equals(fn, "SUM")) {
        *kind = SHEET_FORMULA_SUM;
    } else if (sheet_name_equals(fn, "AVG") || sheet_name_equals(fn, "AVERAGE")) {
        *kind = SHEET_FORMULA_AVG;
    } else if (sheet_name_equals(fn, "MIN")) {
        *kind = SHEET_FORMULA_MIN;
    } else if (sheet_name_equals(fn, "MAX")) {
        *kind = SHEET_FORMULA_MAX;
    } else if (sheet_name_equals(fn, "COUNT")) {
        *kind = SHEET_FORMULA_COUNT;
    } else if (sheet_name_equals(fn, "DELTA")) {
        *kind = SHEET_FORMULA_DELTA;
    } else if (sheet_name_equals(fn, "RATE")) {
        *kind = SHEET_FORMULA_RATE;
    } else {
        return false;
    }

    strlcpy(arg, raw_arg, arg_len);
    return true;
}

static void sheet_eval_count_all(void)
{
    char message[SHEET_MESSAGE_MAX];
    snprintf(message, sizeof(message), "COUNT(*)=%u", (unsigned)sheet.row_count);
    sheet_set_message(message);
}

static void sheet_eval_formula(void)
{
    sheet_formula_t kind;
    char arg[SHEET_CELL_MAX];
    if (!sheet_parse_formula(sheet.formula, &kind, arg, sizeof(arg))) {
        sheet_set_message("formula?");
        return;
    }

    if (kind == SHEET_FORMULA_COUNT && strcmp(sheet_trim(arg), "*") == 0) {
        sheet_eval_count_all();
        return;
    }

    const int col_index = sheet_find_col(arg);
    if (col_index < 0) {
        sheet_set_message("column?");
        return;
    }

    const int time_col = sheet_find_col("time_ms");
    double sum = 0.0;
    double min_value = 0.0;
    double max_value = 0.0;
    double first_value = 0.0;
    double last_value = 0.0;
    double first_time = 0.0;
    double last_time = 0.0;
    size_t numeric_count = 0;

    FILE *file = fopen(sheet.path, "r");
    if (file == NULL) {
        sheet_set_message("open failed");
        return;
    }

    for (size_t row = 0; row < sheet.row_count; row++) {
        if (fseek(file, sheet.row_offsets[row], SEEK_SET) != 0 ||
            !sheet_read_line(file, sheet.line, sizeof(sheet.line))) {
            continue;
        }

        size_t cell_count = 0;
        if (!sheet_csv_parse_line(sheet.line, sheet.cells, &cell_count) ||
            (size_t)col_index >= cell_count) {
            continue;
        }

        double value = 0.0;
        if (!sheet_parse_number(sheet.cells[col_index], &value)) {
            continue;
        }

        double time_value = 0.0;
        const bool have_time =
            time_col >= 0 &&
            (size_t)time_col < cell_count &&
            sheet_parse_number(sheet.cells[time_col], &time_value);

        if (numeric_count == 0) {
            min_value = value;
            max_value = value;
            first_value = value;
            first_time = have_time ? time_value : 0.0;
        }
        if (value < min_value) {
            min_value = value;
        }
        if (value > max_value) {
            max_value = value;
        }
        last_value = value;
        if (have_time) {
            last_time = time_value;
        }
        sum += value;
        numeric_count++;
    }
    fclose(file);

    char message[SHEET_MESSAGE_MAX];
    if (numeric_count == 0) {
        snprintf(message, sizeof(message), "%s: no numbers", sheet.headers[col_index]);
        sheet_set_message(message);
        return;
    }

    double result = 0.0;
    const char *name = "";
    switch (kind) {
    case SHEET_FORMULA_SUM:
        result = sum;
        name = "SUM";
        break;
    case SHEET_FORMULA_AVG:
        result = sum / (double)numeric_count;
        name = "AVG";
        break;
    case SHEET_FORMULA_MIN:
        result = min_value;
        name = "MIN";
        break;
    case SHEET_FORMULA_MAX:
        result = max_value;
        name = "MAX";
        break;
    case SHEET_FORMULA_COUNT:
        result = (double)numeric_count;
        name = "COUNT";
        break;
    case SHEET_FORMULA_DELTA:
        result = last_value - first_value;
        name = "DELTA";
        break;
    case SHEET_FORMULA_RATE:
        result = last_value - first_value;
        if (time_col >= 0 && last_time > first_time) {
            result /= (last_time - first_time) / 1000.0;
        } else if (numeric_count > 1) {
            result /= (double)(numeric_count - 1U);
        }
        name = "RATE";
        break;
    default:
        sheet_set_message("formula?");
        return;
    }

    snprintf(message,
             sizeof(message),
             "%s(%s)=%.6g n=%u",
             name,
             sheet.headers[col_index],
             result,
             (unsigned)numeric_count);
    sheet_set_message(message);
}

static void sheet_start_formula(uint8_t ch)
{
    sheet.input_mode = SHEET_INPUT_FORMULA;
    sheet.formula_len = 0;
    sheet.formula[0] = '\0';
    if (ch != '=' && ch != 'f' && ch != 'F') {
        return;
    }
    sheet_set_message("");
}

static bool sheet_handle_formula_input(solar_os_context_t *ctx, uint8_t ch)
{
    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        sheet.input_mode = SHEET_INPUT_NORMAL;
        sheet.formula_len = 0;
        sheet.formula[0] = '\0';
        sheet_set_message("");
        break;
    case '\r':
    case '\n':
        sheet.input_mode = SHEET_INPUT_NORMAL;
        sheet_eval_formula();
        break;
    case '\b':
    case 0x7f:
        if (sheet.formula_len > 0) {
            sheet.formula[--sheet.formula_len] = '\0';
        }
        break;
    default:
        if (sheet_is_printable(ch) && sheet.formula_len + 1U < sizeof(sheet.formula)) {
            sheet.formula[sheet.formula_len++] = (char)ch;
            sheet.formula[sheet.formula_len] = '\0';
        }
        break;
    }

    sheet_render(ctx);
    return true;
}

static void sheet_page_down(solar_os_context_t *ctx)
{
    (void)ctx;

    const size_t step = sheet_body_rows();
    if (sheet.row_count == 0) {
        return;
    }
    sheet.cursor_row = sheet.cursor_row + step < sheet.row_count ?
        sheet.cursor_row + step :
        sheet.row_count - 1U;
}

static void sheet_page_up(solar_os_context_t *ctx)
{
    (void)ctx;

    const size_t step = sheet_body_rows();
    sheet.cursor_row = sheet.cursor_row > step ? sheet.cursor_row - step : 0;
}

static esp_err_t sheet_start(solar_os_context_t *ctx)
{
    sheet_state = sheet_alloc_state();
    if (sheet_state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = solar_os_tui_begin(&sheet.tui, ctx);
    if (err != ESP_OK) {
        solar_os_memory_free(sheet_state);
        sheet_state = NULL;
        return err;
    }
    (void)solar_os_tui_enable_diff(&sheet.tui, true);

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        sheet.error_only = true;
        sheet_set_message("usage: sheet <file.csv>");
        sheet_render(ctx);
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    strlcpy(sheet.display_name, arg != NULL ? arg : "", sizeof(sheet.display_name));

    err = solar_os_storage_resolve_path(arg, sheet.path, sizeof(sheet.path));
    if (err != ESP_OK) {
        sheet.error_only = true;
        sheet_set_message(err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        sheet_render(ctx);
        return ESP_OK;
    }

    err = sheet_index_file();
    if (err != ESP_OK) {
        sheet.error_only = true;
        sheet_render(ctx);
        return ESP_OK;
    }

    sheet.running = true;
    sheet_render(ctx);
    return ESP_OK;
}

static void sheet_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (sheet_state != NULL) {
        solar_os_tui_set_cursor_visible(&sheet.tui, true);
        solar_os_tui_refresh(&sheet.tui);
        solar_os_tui_end(&sheet.tui);
    }
    sheet_free_offsets();
    solar_os_memory_free(sheet_state);
    sheet_state = NULL;
}

static bool sheet_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (sheet.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    if (sheet.input_mode == SHEET_INPUT_FORMULA) {
        return sheet_handle_formula_input(ctx, ch);
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case SOLAR_OS_KEY_LEFT:
        if (sheet.cursor_col > 0) {
            sheet.cursor_col--;
        }
        break;
    case SOLAR_OS_KEY_RIGHT:
        if (sheet.cursor_col + 1U < sheet.col_count) {
            sheet.cursor_col++;
        }
        break;
    case SOLAR_OS_KEY_UP:
        if (sheet.cursor_row > 0) {
            sheet.cursor_row--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (sheet.cursor_row + 1U < sheet.row_count) {
            sheet.cursor_row++;
        }
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        sheet_page_up(ctx);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        sheet_page_down(ctx);
        break;
    case SOLAR_OS_KEY_HOME:
        sheet.cursor_col = 0;
        break;
    case SOLAR_OS_KEY_END:
        if (sheet.col_count > 0) {
            sheet.cursor_col = sheet.col_count - 1U;
        }
        break;
    case '=':
    case 'f':
    case 'F':
        sheet_start_formula(ch);
        break;
    default:
        return true;
    }

    sheet_set_message("");
    sheet_render(ctx);
    return true;
}

const solar_os_app_t solar_os_sheet_app = {
    .name = "sheet",
    .summary = "CSV sheet viewer",
    .start = sheet_start,
    .stop = sheet_stop,
    .event = sheet_event,
};
