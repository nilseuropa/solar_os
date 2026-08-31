#include "solar_os_calc.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_expr.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"

#define CALC_ROW_MAX 8U
#define CALC_SOURCE_MAX 128U
#define CALC_HISTORY_MAX 8U
#define CALC_PATH_MAX SOLAR_OS_STORAGE_PATH_MAX
#define CALC_EVAL_DEPTH_MAX 12U
#define CALC_DEFAULT_FILE "calc.txt"

typedef enum {
    CALC_ROW_EMPTY,
    CALC_ROW_VALUE,
    CALC_ROW_VARIABLE,
    CALC_ROW_FUNCTION,
    CALC_ROW_GRAPH,
} calc_row_kind_t;

typedef enum {
    CALC_MODE_TEXT,
    CALC_MODE_GRAPHICS,
} calc_mode_t;

typedef enum {
    CALC_FOCUS_ROWS,
    CALC_FOCUS_GRAPH,
} calc_focus_t;

typedef struct {
    char source[CALC_SOURCE_MAX];
    char name[SOLAR_OS_EXPR_NAME_MAX];
    calc_row_kind_t kind;
    solar_os_expr_program_t program;
    solar_os_expr_error_t error;
    bool compiled;
    bool visible;
    bool visiting;
    double result;
    bool has_result;
} calc_row_t;

typedef struct {
    struct calc_state *state;
    bool has_x;
    double x;
    unsigned depth;
} calc_eval_frame_t;

typedef struct calc_state {
    calc_mode_t mode;
    calc_focus_t focus;
    bool suspended;
    calc_row_t rows[CALC_ROW_MAX];
    size_t row_count;
    size_t active_row;
    size_t cursor;
    char input[CALC_SOURCE_MAX];
    size_t input_len;
    size_t input_cursor;
    size_t input_row;
    size_t input_col;
    char history[CALC_HISTORY_MAX][CALC_SOURCE_MAX];
    size_t history_count;
    size_t history_index;
    double center_x;
    double center_y;
    double units_x;
    double units_y;
    bool trace;
    double trace_x;
    char message[80];
    solar_os_shell_io_t fallback_io;
} calc_state_t;

static calc_state_t *calc;
#define calc_fallback_io (calc->fallback_io)

static void calc_format(double value, char *buffer, size_t buffer_len)
{
    if (fabs(value) < 5e-15) {
        value = 0.0;
    }
    snprintf(buffer, buffer_len, "%.12g", value);
}

static char *calc_trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static bool calc_valid_name(const char *text)
{
    if (text == NULL || strlen(text) >= SOLAR_OS_EXPR_NAME_MAX ||
        !(isalpha((unsigned char)*text) || *text == '_')) {
        return false;
    }
    for (text++; *text != '\0'; text++) {
        if (!(isalnum((unsigned char)*text) || *text == '_')) {
            return false;
        }
    }
    return true;
}

static void calc_parse_row(calc_row_t *row)
{
    row->compiled = false;
    row->has_result = false;
    row->kind = CALC_ROW_EMPTY;
    row->name[0] = '\0';
    memset(&row->error, 0, sizeof(row->error));

    char work[CALC_SOURCE_MAX];
    snprintf(work, sizeof(work), "%s", row->source);
    char *source = calc_trim(work);
    if (*source == '\0') {
        return;
    }

    char *expression = source;
    char *equals = strchr(source, '=');
    if (equals != NULL && strchr(equals + 1, '=') == NULL) {
        *equals = '\0';
        char *lhs = calc_trim(source);
        expression = calc_trim(equals + 1);
        for (char *p = lhs; *p != '\0'; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        const size_t lhs_len = strlen(lhs);
        if (lhs_len > 3U && lhs[lhs_len - 1U] == ')' &&
            lhs[lhs_len - 2U] == 'x' && lhs[lhs_len - 3U] == '(') {
            lhs[lhs_len - 3U] = '\0';
            lhs = calc_trim(lhs);
            if (calc_valid_name(lhs) && strcmp(lhs, "y") != 0) {
                row->kind = CALC_ROW_FUNCTION;
                snprintf(row->name, sizeof(row->name), "%s", lhs);
            }
        } else if (calc_valid_name(lhs)) {
            snprintf(row->name, sizeof(row->name), "%s", lhs);
            row->kind =
                strcmp(lhs, "y") == 0 ? CALC_ROW_GRAPH : CALC_ROW_VARIABLE;
        }
        if (row->kind == CALC_ROW_VARIABLE &&
            (strcmp(row->name, "x") == 0 || strcmp(row->name, "pi") == 0 ||
             strcmp(row->name, "e") == 0)) {
            row->kind = CALC_ROW_EMPTY;
        }
        if (row->kind == CALC_ROW_EMPTY) {
            snprintf(row->error.message, sizeof(row->error.message),
                     "invalid definition");
            return;
        }
    }

    if (solar_os_expr_compile(expression, &row->program, &row->error) !=
        ESP_OK) {
        return;
    }
    row->compiled = true;
    if (row->kind == CALC_ROW_EMPTY) {
        row->kind = row->program.uses_x ? CALC_ROW_GRAPH : CALC_ROW_VALUE;
    } else if (row->kind == CALC_ROW_VARIABLE && row->program.uses_x) {
        row->compiled = false;
        snprintf(row->error.message, sizeof(row->error.message),
                 "only functions and y may use x");
    }
}

static bool calc_eval_row(calc_eval_frame_t *frame, size_t index,
                          double *value);

static bool calc_lookup_variable(void *user, const char *name, double *value)
{
    calc_eval_frame_t *frame = user;
    for (size_t i = frame->state->row_count; i > 0U; i--) {
        calc_row_t *row = &frame->state->rows[i - 1U];
        if (row->kind == CALC_ROW_VARIABLE && strcmp(row->name, name) == 0) {
            if (calc_eval_row(frame, i - 1U, value)) {
                return true;
            }
        }
    }
    return false;
}

static bool calc_lookup_function(void *user, const char *name,
                                 const double *args, size_t argc, double *value)
{
    calc_eval_frame_t *frame = user;
    if (argc != 1U || frame->depth >= CALC_EVAL_DEPTH_MAX) {
        return false;
    }
    for (size_t i = frame->state->row_count; i > 0U; i--) {
        calc_row_t *row = &frame->state->rows[i - 1U];
        if (row->kind == CALC_ROW_FUNCTION && strcmp(row->name, name) == 0) {
            calc_eval_frame_t child = {
                .state = frame->state,
                .has_x = true,
                .x = args[0],
                .depth = frame->depth + 1U,
            };
            if (calc_eval_row(&child, i - 1U, value)) {
                return true;
            }
        }
    }
    return false;
}

static bool calc_eval_row(calc_eval_frame_t *frame, size_t index, double *value)
{
    if (index >= frame->state->row_count ||
        frame->depth >= CALC_EVAL_DEPTH_MAX) {
        return false;
    }
    calc_row_t *row = &frame->state->rows[index];
    if (!row->compiled || row->visiting) {
        return false;
    }
    row->visiting = true;
    solar_os_expr_context_t context = {
        .user = frame,
        .variable = calc_lookup_variable,
        .function = calc_lookup_function,
        .has_x = frame->has_x,
        .x = frame->x,
    };
    solar_os_expr_error_t error = {0};
    const esp_err_t err =
        solar_os_expr_evaluate(&row->program, &context, value, &error);
    row->visiting = false;
    if (err != ESP_OK && row->error.message[0] == '\0') {
        row->error = error;
    }
    return err == ESP_OK;
}

static void calc_recompute(void)
{
    for (size_t i = 0; i < calc->row_count; i++) {
        calc->rows[i].has_result = false;
        calc->rows[i].visiting = false;
        if (!calc->rows[i].compiled) {
            continue;
        }
        calc->rows[i].error.message[0] = '\0';
        if (calc->rows[i].kind == CALC_ROW_VALUE ||
            calc->rows[i].kind == CALC_ROW_VARIABLE) {
            calc_eval_frame_t frame = {.state = calc};
            calc->rows[i].has_result =
                calc_eval_row(&frame, i, &calc->rows[i].result);
        }
    }
}

static void calc_compile_all(void)
{
    for (size_t i = 0; i < calc->row_count; i++) {
        calc_parse_row(&calc->rows[i]);
    }
    calc_recompute();
}

static void calc_set_message(const char *message)
{
    snprintf(calc->message, sizeof(calc->message), "%s",
             message != NULL ? message : "");
}

static solar_os_shell_io_t *calc_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL ||
        solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&calc_fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &calc_fallback_io);
        io = &calc_fallback_io;
    }
    return io;
}

static void calc_text_prompt(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = calc_io(ctx);
    solar_os_shell_io_write(io, "> ");
    calc->input_row = solar_os_shell_io_cursor_row(io);
    calc->input_col = solar_os_shell_io_cursor_col(io);
    solar_os_shell_io_flush(io);
}

static void calc_text_render_input(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = calc_io(ctx);
    solar_os_shell_io_clear_line_from(io, calc->input_row, calc->input_col);
    solar_os_shell_io_set_cursor(io, calc->input_row, calc->input_col);
    solar_os_shell_io_write(io, calc->input);
    solar_os_shell_io_set_cursor(io, calc->input_row,
                                 calc->input_col + calc->input_cursor);
    solar_os_shell_io_flush(io);
}

static size_t calc_text_max_input_len(solar_os_context_t *ctx)
{
    const size_t cols = solar_os_shell_io_cols(calc_io(ctx));
    const size_t visible = cols > calc->input_col ? cols - calc->input_col : 0U;
    const size_t buffer = sizeof(calc->input) - 1U;
    return visible > 0U && visible < buffer ? visible : buffer;
}

static void calc_history_add(const char *line)
{
    if (line == NULL || *line == '\0')
        return;
    if (calc->history_count == CALC_HISTORY_MAX) {
        memmove(calc->history, calc->history + 1,
                sizeof(calc->history[0]) * (CALC_HISTORY_MAX - 1U));
        calc->history_count--;
    }
    snprintf(calc->history[calc->history_count++], sizeof(calc->history[0]),
             "%s", line);
    calc->history_index = calc->history_count;
}

static void calc_add_row(const char *source)
{
    if (calc->row_count == 1U && calc->rows[0].source[0] == '\0') {
        calc_row_t *row = &calc->rows[0];
        snprintf(row->source, sizeof(row->source), "%s", source);
        row->visible = true;
        calc_compile_all();
        return;
    }
    if (calc->row_count == CALC_ROW_MAX) {
        memmove(calc->rows, calc->rows + 1,
                sizeof(calc->rows[0]) * (CALC_ROW_MAX - 1U));
        calc->row_count--;
    }
    calc_row_t *row = &calc->rows[calc->row_count++];
    memset(row, 0, sizeof(*row));
    snprintf(row->source, sizeof(row->source), "%s", source);
    row->visible = true;
    calc_compile_all();
}

static bool calc_path(solar_os_context_t *ctx, const char *arg, char *path,
                      size_t path_len)
{
    const char *name = arg != NULL && *arg != '\0' ? arg : CALC_DEFAULT_FILE;
    return solar_os_shell_resolve_path(ctx, name, path, path_len) == ESP_OK;
}

static bool calc_save(solar_os_context_t *ctx, const char *arg)
{
    char path[CALC_PATH_MAX];
    if (!calc_path(ctx, arg, path, sizeof(path)))
        return false;
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return false;
    bool ok = true;
    for (size_t i = 0; i < calc->row_count; i++) {
        if (fprintf(file, "%s\n", calc->rows[i].source) < 0) {
            ok = false;
            break;
        }
    }
    if (fclose(file) != 0)
        ok = false;
    if (ok)
        snprintf(calc->message, sizeof(calc->message), "saved %s",
                 arg != NULL ? arg : CALC_DEFAULT_FILE);
    return ok;
}

static bool calc_load(solar_os_context_t *ctx, const char *arg)
{
    char path[CALC_PATH_MAX];
    if (!calc_path(ctx, arg, path, sizeof(path)))
        return false;
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    memset(calc->rows, 0, sizeof(calc->rows));
    calc->row_count = 0U;
    char line[CALC_SOURCE_MAX + 4U];
    while (calc->row_count < CALC_ROW_MAX &&
           fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        calc_row_t *row = &calc->rows[calc->row_count++];
        snprintf(row->source, sizeof(row->source), "%s", line);
        row->visible = true;
    }
    fclose(file);
    if (calc->row_count == 0U)
        calc->row_count = 1U;
    calc->active_row = 0U;
    calc->cursor = strlen(calc->rows[0].source);
    calc_compile_all();
    snprintf(calc->message, sizeof(calc->message), "loaded %s",
             arg != NULL ? arg : CALC_DEFAULT_FILE);
    return true;
}

static void calc_print_rows(solar_os_shell_io_t *io)
{
    for (size_t i = 0; i < calc->row_count; i++) {
        calc_row_t *row = &calc->rows[i];
        char result[32] = "";
        if (row->has_result)
            calc_format(row->result, result, sizeof(result));
        solar_os_shell_io_printf(io, "%u: %s%s%s\n", (unsigned)(i + 1U),
                                 row->source, row->has_result ? " = " : "",
                                 result);
    }
}

static void calc_text_help(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(
        io, "scientific expressions use radians; constants: pi, e");
    solar_os_shell_io_writeln(io, "definitions: a=2, f(x)=sin(x), y=f(x)");
    solar_os_shell_io_writeln(io, "commands: :list :vars :del n :clear :save "
                                  "[file] :load [file] :help :quit");
}

static void calc_text_submit(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = calc_io(ctx);
    solar_os_shell_io_newline(io);
    char line[CALC_SOURCE_MAX];
    snprintf(line, sizeof(line), "%s", calc->input);
    char *command = calc_trim(line);
    calc_history_add(command);
    calc->input[0] = '\0';
    calc->input_len = calc->input_cursor = 0U;

    if (*command == ':') {
        char *arg = strchr(command, ' ');
        if (arg != NULL)
            *arg++ = '\0';
        arg = arg != NULL ? calc_trim(arg) : NULL;
        if (strcmp(command, ":quit") == 0 || strcmp(command, ":q") == 0) {
            solar_os_context_finish(ctx, 0, NULL);
            return;
        } else if (strcmp(command, ":help") == 0) {
            calc_text_help(io);
        } else if (strcmp(command, ":list") == 0 ||
                   strcmp(command, ":vars") == 0) {
            calc_print_rows(io);
        } else if (strcmp(command, ":clear") == 0) {
            memset(calc->rows, 0, sizeof(calc->rows));
            calc->row_count = 0U;
        } else if (strcmp(command, ":del") == 0 && arg != NULL) {
            const long number = strtol(arg, NULL, 10);
            if (number >= 1 && (size_t)number <= calc->row_count) {
                const size_t index = (size_t)number - 1U;
                memmove(&calc->rows[index], &calc->rows[index + 1U],
                        sizeof(calc->rows[0]) * (calc->row_count - index - 1U));
                calc->row_count--;
                calc_compile_all();
            } else
                solar_os_shell_io_writeln(io, "calc: invalid row");
        } else if (strcmp(command, ":save") == 0) {
            solar_os_shell_io_writeln(
                io, calc_save(ctx, arg) ? calc->message : "calc: save failed");
        } else if (strcmp(command, ":load") == 0) {
            solar_os_shell_io_writeln(
                io, calc_load(ctx, arg) ? calc->message : "calc: load failed");
        } else {
            solar_os_shell_io_writeln(io, "calc: unknown command; use :help");
        }
    } else if (*command != '\0') {
        calc_add_row(command);
        calc_row_t *row = &calc->rows[calc->row_count - 1U];
        if (!row->compiled ||
            (!row->has_result && row->kind != CALC_ROW_FUNCTION &&
             row->kind != CALC_ROW_GRAPH)) {
            solar_os_shell_io_printf(io, "error: %s\n",
                                     row->error.message[0]
                                         ? row->error.message
                                         : "evaluation failed");
        } else if (row->has_result) {
            char result[32];
            calc_format(row->result, result, sizeof(result));
            solar_os_shell_io_writeln(io, result);
        } else if (row->kind == CALC_ROW_GRAPH) {
            solar_os_shell_io_writeln(io, "plot hidden in text mode");
        } else {
            solar_os_shell_io_writeln(io, "defined");
        }
    }
    solar_os_shell_io_flush(io);
    calc_text_prompt(ctx);
}

static int calc_map_x(double x, int left, int width)
{
    return left + width / 2 + (int)lround((x - calc->center_x) / calc->units_x);
}

static int calc_map_y(double y, int top, int height)
{
    return top + height / 2 - (int)lround((y - calc->center_y) / calc->units_y);
}

static size_t calc_selected_graph_row(void)
{
    if (calc->active_row < calc->row_count &&
        calc->rows[calc->active_row].kind == CALC_ROW_GRAPH) {
        return calc->active_row;
    }
    for (size_t i = 0; i < calc->row_count; i++) {
        if (calc->rows[i].kind == CALC_ROW_GRAPH) {
            return i;
        }
    }
    return CALC_ROW_MAX;
}

static bool calc_trace_value(double *value)
{
    const size_t row = calc_selected_graph_row();
    if (row >= calc->row_count) {
        return false;
    }
    calc_eval_frame_t frame = {
        .state = calc,
        .has_x = true,
        .x = calc->trace_x,
    };
    return calc_eval_row(&frame, row, value);
}

static void calc_draw_graph(solar_os_gfx_t *gfx, int left, int top, int width,
                            int height)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    for (int x = left + width / 2; x < left + width; x += 40)
        solar_os_gfx_line(gfx, x, top, x, top + height - 1);
    for (int x = left + width / 2 - 40; x >= left; x -= 40)
        solar_os_gfx_line(gfx, x, top, x, top + height - 1);
    for (int y = top + height / 2; y < top + height; y += 40)
        solar_os_gfx_line(gfx, left, y, left + width - 1, y);
    for (int y = top + height / 2 - 40; y >= top; y -= 40)
        solar_os_gfx_line(gfx, left, y, left + width - 1, y);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    const int axis_x = calc_map_x(0.0, left, width);
    const int axis_y = calc_map_y(0.0, top, height);
    if (axis_x >= left && axis_x < left + width)
        solar_os_gfx_line(gfx, axis_x, top, axis_x, top + height - 1);
    if (axis_y >= top && axis_y < top + height)
        solar_os_gfx_line(gfx, left, axis_y, left + width - 1, axis_y);

    for (size_t row_index = 0; row_index < calc->row_count; row_index++) {
        calc_row_t *row = &calc->rows[row_index];
        if (row->kind != CALC_ROW_GRAPH || !row->compiled || !row->visible)
            continue;
        solar_os_gfx_set_color(gfx, row_index == calc->active_row
                                        ? SOLAR_OS_GFX_COLOR_BLACK
                                        : SOLAR_OS_GFX_COLOR_DARK);
        solar_os_gfx_set_line_style(gfx, row_index == calc->active_row
                                             ? SOLAR_OS_GFX_LINE_SOLID
                                             : SOLAR_OS_GFX_LINE_DASHED);
        bool have_previous = false;
        int previous_y = 0;
        for (int px = 0; px < width; px++) {
            const double x = calc->center_x +
                             ((double)px - (double)width / 2.0) * calc->units_x;
            calc_eval_frame_t frame = {.state = calc, .has_x = true, .x = x};
            double y = 0.0;
            if (!calc_eval_row(&frame, row_index, &y)) {
                have_previous = false;
                continue;
            }
            const int py = calc_map_y(y, top, height);
            if (have_previous && py > top - height && py < top + height * 2 &&
                abs(py - previous_y) < height) {
                solar_os_gfx_line(gfx, left + px - 1, previous_y, left + px,
                                  py);
            }
            previous_y = py;
            have_previous = py > top - height && py < top + height * 2;
        }
    }
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);

    double trace_y = 0.0;
    if (calc->trace && calc_trace_value(&trace_y)) {
        const int px = calc_map_x(calc->trace_x, left, width);
        const int py = calc_map_y(trace_y, top, height);
        if (px >= left && px < left + width && py >= top && py < top + height) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DOTTED);
            solar_os_gfx_line(gfx, px, top, px, top + height - 1);
            solar_os_gfx_line(gfx, left, py, left + width - 1, py);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_circle(gfx, px, py, 3);
        }
    }
}

static void calc_graphics_render(solar_os_context_t *ctx)
{
    if (calc == NULL || calc->suspended)
        return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL)
        return;
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const int footer = height >= 120 ? 18 : 12;
    const int panel = width >= 380 ? 142 : (width >= 240 ? 110 : width / 2);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (height >= 120)
        solar_os_gfx_text(gfx, 5, 14, "calc");
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_gfx_line(gfx, panel, 0, panel, height - footer);
    solar_os_gfx_line(gfx, 0, height - footer, width - 1, height - footer);

    const int row_height = height >= 120 ? 25 : 15;
    for (size_t i = 0;
         i < calc->row_count && 20 + (int)i * row_height < height - footer;
         i++) {
        const int y = (height >= 120 ? 27 : 12) + (int)i * row_height;
        if (i == calc->active_row) {
            const int row_top = y >= 13 ? y - 13 : 0;
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_rect(gfx, 1, row_top, panel - 2, row_height - 1);
        }
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        char shown[24];
        int source_chars = panel / 7 - 4;
        if (source_chars < 1)
            source_chars = 1;
        if (source_chars > 17)
            source_chars = 17;
        size_t source_start = 0U;
        if (i == calc->active_row && calc->cursor >= (size_t)source_chars) {
            source_start = calc->cursor - (size_t)source_chars + 1U;
        }
        snprintf(shown, sizeof(shown), "%u %c %.*s", (unsigned)(i + 1U),
                 calc->rows[i].visible ? '*' : ' ', source_chars,
                 calc->rows[i].source + source_start);
        solar_os_gfx_text(gfx, 4, y, shown);
        if (i == calc->active_row && calc->focus == CALC_FOCUS_ROWS) {
            const int cursor_x =
                4 + (4 + (int)(calc->cursor - source_start)) * 7;
            if (cursor_x < panel) {
                solar_os_gfx_line(gfx, cursor_x, y - 11, cursor_x, y + 2);
            }
        }
        char result[22];
        if (height >= 120 && calc->rows[i].has_result) {
            calc_format(calc->rows[i].result, result, sizeof(result));
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 23, y + 10, result);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        } else if (height >= 120 && calc->rows[i].error.message[0] != '\0') {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, 23, y + 10, calc->rows[i].error.message);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        }
    }
    calc_draw_graph(gfx, panel + 1, 0, width - panel - 1, height - footer - 1);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char trace_text[64];
    const char *footer_text;
    if (calc->message[0] != '\0') {
        footer_text = calc->message;
    } else if (calc->trace) {
        double trace_y = 0.0;
        if (calc_trace_value(&trace_y)) {
            char x[24];
            char y[24];
            calc_format(calc->trace_x, x, sizeof(x));
            calc_format(trace_y, y, sizeof(y));
            snprintf(trace_text, sizeof(trace_text), "trace x=%s y=%s", x, y);
        } else {
            snprintf(trace_text, sizeof(trace_text), "trace: no value");
        }
        footer_text = trace_text;
    } else {
        footer_text = calc->focus == CALC_FOCUS_ROWS
                          ? "type edit  arrows row  Tab graph"
                          : "arrows pan  +/- zoom  t trace";
    }
    solar_os_gfx_text(gfx, 4, height - 5, footer_text);
    solar_os_gfx_present(gfx);
}

static void calc_graphics_edit(char ch)
{
    calc_row_t *row = &calc->rows[calc->active_row];
    const size_t len = strlen(row->source);
    if (ch == '\b') {
        if (calc->cursor > 0U) {
            memmove(&row->source[calc->cursor - 1U], &row->source[calc->cursor],
                    len - calc->cursor + 1U);
            calc->cursor--;
        }
    } else if ((uint8_t)ch == SOLAR_OS_KEY_DELETE) {
        if (calc->cursor < len)
            memmove(&row->source[calc->cursor], &row->source[calc->cursor + 1U],
                    len - calc->cursor);
    } else if (isprint((unsigned char)ch) && len + 1U < sizeof(row->source)) {
        memmove(&row->source[calc->cursor + 1U], &row->source[calc->cursor],
                len - calc->cursor + 1U);
        row->source[calc->cursor++] = ch;
    }
    calc_parse_row(row);
    calc_recompute();
}

static void calc_graphics_event(solar_os_context_t *ctx, uint8_t ch)
{
    calc_set_message(NULL);
    if (ch == 0x13U) {
        if (!calc_save(ctx, NULL)) {
            calc_set_message("save failed");
        }
    } else if (ch == 0x0fU) {
        if (!calc_load(ctx, NULL)) {
            calc_set_message("load failed");
        }
    } else if (ch == '\t') {
        calc->focus =
            calc->focus == CALC_FOCUS_ROWS ? CALC_FOCUS_GRAPH : CALC_FOCUS_ROWS;
    } else if (calc->focus == CALC_FOCUS_GRAPH) {
        const double pan_x = calc->units_x * 30.0;
        const double pan_y = calc->units_y * 30.0;
        if (ch == 't' || ch == 'T') {
            calc->trace = !calc->trace;
            calc->trace_x = calc->center_x;
        } else if (calc->trace && ch == SOLAR_OS_KEY_LEFT) {
            calc->trace_x -= calc->units_x * 5.0;
        } else if (calc->trace && ch == SOLAR_OS_KEY_RIGHT) {
            calc->trace_x += calc->units_x * 5.0;
        } else if (calc->trace &&
                   (ch == SOLAR_OS_KEY_UP || ch == SOLAR_OS_KEY_DOWN)) {
            size_t candidate = calc->active_row;
            for (size_t attempts = 0; attempts < calc->row_count; attempts++) {
                if (ch == SOLAR_OS_KEY_UP) {
                    candidate =
                        candidate == 0U ? calc->row_count - 1U : candidate - 1U;
                } else {
                    candidate = (candidate + 1U) % calc->row_count;
                }
                if (calc->rows[candidate].kind == CALC_ROW_GRAPH) {
                    calc->active_row = candidate;
                    break;
                }
            }
        } else if (ch == SOLAR_OS_KEY_LEFT)
            calc->center_x -= pan_x;
        else if (ch == SOLAR_OS_KEY_RIGHT)
            calc->center_x += pan_x;
        else if (ch == SOLAR_OS_KEY_UP)
            calc->center_y += pan_y;
        else if (ch == SOLAR_OS_KEY_DOWN)
            calc->center_y -= pan_y;
        else if (ch == '+' || ch == '=') {
            calc->units_x *= 0.75;
            calc->units_y *= 0.75;
        } else if (ch == '-') {
            calc->units_x *= 1.333333;
            calc->units_y *= 1.333333;
        } else if (ch == SOLAR_OS_KEY_HOME || ch == '0') {
            calc->center_x = calc->center_y = 0.0;
            calc->units_x = calc->units_y = 0.05;
        }
    } else {
        calc_row_t *row = &calc->rows[calc->active_row];
        if (ch == SOLAR_OS_KEY_UP && calc->active_row > 0U) {
            calc->active_row--;
            calc->cursor = strlen(calc->rows[calc->active_row].source);
        } else if (ch == SOLAR_OS_KEY_DOWN) {
            if (calc->active_row + 1U >= calc->row_count &&
                calc->row_count < CALC_ROW_MAX) {
                calc->rows[calc->row_count].visible = true;
                calc->row_count++;
            }
            if (calc->active_row + 1U < calc->row_count)
                calc->active_row++;
            calc->cursor = strlen(calc->rows[calc->active_row].source);
        } else if (ch == SOLAR_OS_KEY_LEFT && calc->cursor > 0U)
            calc->cursor--;
        else if (ch == SOLAR_OS_KEY_RIGHT && calc->cursor < strlen(row->source))
            calc->cursor++;
        else if (ch == SOLAR_OS_KEY_HOME)
            calc->cursor = 0U;
        else if (ch == SOLAR_OS_KEY_END)
            calc->cursor = strlen(row->source);
        else if (ch == '\r' || ch == '\n') {
            if (calc->active_row + 1U >= calc->row_count &&
                calc->row_count < CALC_ROW_MAX) {
                calc->rows[calc->row_count].visible = true;
                calc->row_count++;
            }
            if (calc->active_row + 1U < calc->row_count)
                calc->active_row++;
            calc->cursor = strlen(calc->rows[calc->active_row].source);
        } else if (ch == SOLAR_OS_KEY_PAGE_UP) {
            row->visible = !row->visible;
        } else
            calc_graphics_edit((char)ch);
    }
    calc_graphics_render(ctx);
}

static void calc_print_usage(solar_os_context_t *ctx, const char *reason)
{
    solar_os_shell_io_t *io = calc_io(ctx);
    if (reason != NULL)
        solar_os_shell_io_printf(io, "calc: %s\n", reason);
    solar_os_shell_io_writeln(io, "usage: calc [--tui]");
    solar_os_shell_io_writeln(io, "       calc -e <expression>");
    solar_os_shell_io_flush(io);
}

static esp_err_t calc_start(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc > 1 &&
        !(argc == 2 && strcmp(solar_os_context_argv(ctx, 1), "--tui") == 0)) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
    }
    calc = solar_os_memory_calloc(
        1, sizeof(*calc), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "calc.state");
    if (calc == NULL)
        return ESP_ERR_NO_MEM;
    calc->row_count = 1U;
    calc->rows[0].visible = true;
    calc->units_x = calc->units_y = 0.05;

    bool force_tui = false;
    bool one_shot = false;
    char expression[CALC_SOURCE_MAX] = "";
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "--tui") == 0)
            force_tui = true;
        else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0)
            one_shot = true;
        else if (one_shot) {
            if (expression[0] != '\0')
                strlcat(expression, " ", sizeof(expression));
            strlcat(expression, arg, sizeof(expression));
        } else {
            calc_print_usage(ctx, "unknown option");
            solar_os_context_finish(ctx, 2, NULL);
            return ESP_OK;
        }
    }
    if (one_shot && expression[0] == '\0') {
        calc_print_usage(ctx, "-e needs an expression");
        solar_os_context_finish(ctx, 2, NULL);
        return ESP_OK;
    }

    if (one_shot) {
        calc_add_row(expression);
        calc_row_t *row = &calc->rows[calc->row_count - 1U];
        solar_os_shell_io_t *io = calc_io(ctx);
        if (row->has_result) {
            char result[32];
            calc_format(row->result, result, sizeof(result));
            solar_os_shell_io_writeln(io, result);
            solar_os_context_finish(ctx, 0, NULL);
        } else {
            char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
            snprintf(message,
                     sizeof(message),
                     "calc: %s",
                     row->error.message[0] ?
                         row->error.message : "not a scalar expression");
            solar_os_shell_io_printf(io, "calc: %s\n",
                                     row->error.message[0]
                                         ? row->error.message
                                         : "not a scalar expression");
            solar_os_context_finish(ctx, 1, NULL);
        }
        solar_os_shell_io_flush(io);
        calc->mode = CALC_MODE_TEXT;
        return ESP_OK;
    }

    solar_os_shell_io_t *launch_io = solar_os_context_shell_io(ctx);
    const bool port_shell =
        launch_io != NULL &&
        solar_os_shell_io_kind(launch_io) == SOLAR_OS_SHELL_IO_KIND_PORT;
    if (!force_tui && !port_shell && solar_os_context_gfx(ctx) != NULL) {
        calc->mode = CALC_MODE_GRAPHICS;
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_GUI);
        solar_os_context_set_graphics_active(ctx, true);
        calc_graphics_render(ctx);
    } else {
        calc->mode = CALC_MODE_TEXT;
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_TUI);
        solar_os_shell_io_t *io = calc_io(ctx);
        solar_os_shell_io_writeln(
            io, "SolarOS calculator (radians). :help for commands.");
        calc_text_prompt(ctx);
    }
    return ESP_OK;
}

static void calc_stop(solar_os_context_t *ctx)
{
    if (calc != NULL && calc->mode == CALC_MODE_GRAPHICS)
        solar_os_context_set_graphics_active(ctx, false);
    solar_os_memory_free(calc);
    calc = NULL;
}

static void calc_suspend(solar_os_context_t *ctx)
{
    if (calc == NULL || calc->mode != CALC_MODE_GRAPHICS)
        return;
    calc->suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void calc_resume(solar_os_context_t *ctx)
{
    if (calc == NULL)
        return;
    if (calc->mode == CALC_MODE_TEXT) {
        solar_os_shell_io_writeln(calc_io(ctx), "SolarOS calculator resumed.");
        calc_text_prompt(ctx);
        if (calc->input_len > 0U)
            calc_text_render_input(ctx);
        return;
    }
    calc->suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    calc_graphics_render(ctx);
}

static bool calc_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (calc == NULL || event == NULL)
        return false;
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        calc_resume(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR)
        return false;
    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
        if (calc->mode == CALC_MODE_TEXT && calc->input_len > 0U &&
            ch == SOLAR_OS_KEY_ESCAPE) {
            calc->input[0] = '\0';
            calc->input_len = calc->input_cursor = 0U;
            calc_text_render_input(ctx);
        } else {
            solar_os_context_finish(ctx, 0, NULL);
        }
        return true;
    }
    if (calc->mode == CALC_MODE_GRAPHICS) {
        calc_graphics_event(ctx, ch);
        return true;
    }

    if (ch == '\r' || ch == '\n')
        calc_text_submit(ctx);
    else if (ch == SOLAR_OS_KEY_LEFT && calc->input_cursor > 0U) {
        calc->input_cursor--;
        calc_text_render_input(ctx);
    } else if (ch == SOLAR_OS_KEY_RIGHT &&
               calc->input_cursor < calc->input_len) {
        calc->input_cursor++;
        calc_text_render_input(ctx);
    } else if (ch == SOLAR_OS_KEY_HOME) {
        calc->input_cursor = 0U;
        calc_text_render_input(ctx);
    } else if (ch == SOLAR_OS_KEY_END) {
        calc->input_cursor = calc->input_len;
        calc_text_render_input(ctx);
    } else if (ch == SOLAR_OS_KEY_UP || ch == SOLAR_OS_KEY_DOWN) {
        if (ch == SOLAR_OS_KEY_UP && calc->history_index > 0U)
            calc->history_index--;
        if (ch == SOLAR_OS_KEY_DOWN &&
            calc->history_index < calc->history_count)
            calc->history_index++;
        const char *line = calc->history_index < calc->history_count
                               ? calc->history[calc->history_index]
                               : "";
        snprintf(calc->input, sizeof(calc->input), "%s", line);
        calc->input_len = calc->input_cursor = strlen(calc->input);
        calc_text_render_input(ctx);
    } else if (ch == '\b' && calc->input_cursor > 0U) {
        memmove(&calc->input[calc->input_cursor - 1U],
                &calc->input[calc->input_cursor],
                calc->input_len - calc->input_cursor + 1U);
        calc->input_cursor--;
        calc->input_len--;
        calc_text_render_input(ctx);
    } else if (ch == SOLAR_OS_KEY_DELETE &&
               calc->input_cursor < calc->input_len) {
        memmove(&calc->input[calc->input_cursor],
                &calc->input[calc->input_cursor + 1U],
                calc->input_len - calc->input_cursor);
        calc->input_len--;
        calc_text_render_input(ctx);
    } else if (isprint(ch) && calc->input_len < calc_text_max_input_len(ctx)) {
        memmove(&calc->input[calc->input_cursor + 1U],
                &calc->input[calc->input_cursor],
                calc->input_len - calc->input_cursor + 1U);
        calc->input[calc->input_cursor++] = (char)ch;
        calc->input_len++;
        calc_text_render_input(ctx);
    }
    return true;
}

static void calc_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "calc");
}

const solar_os_app_t solar_os_calc_app = {
    .name = "calc",
    .summary = "scientific calculator and function plotter",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = calc_start,
    .suspend = calc_suspend,
    .resume = calc_resume,
    .stop = calc_stop,
    .event = calc_event,
    .title = calc_title,
    .tick_deadline_ms = 30U,
};
