#include "solar_os_tui_widgets.h"
#include "solar_os_native_client_ui.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_keys.h"
#include "solar_os_terminal.h"

#define SOLAR_OS_TUI_AUTO_FULLSCREEN_MAX_ROWS 10U

static esp_err_t tui_screen_set_fullscreen(solar_os_tui_t *tui, bool fullscreen)
{
    if (tui == NULL || !tui->screen_active) return ESP_ERR_INVALID_STATE;
    if (tui->fullscreen == fullscreen) return ESP_OK;

    if (tui->terminal != NULL) {
        const esp_err_t err = solar_os_terminal_set_status_bar_visible_transient(
            tui->terminal, fullscreen ? false : tui->saved_status_bar_visible);
        if (err != ESP_OK) return err;
        tui->status_bar_overridden = fullscreen;
    }
    tui->fullscreen = fullscreen;

    if (tui->diff_enabled) {
        (void)solar_os_tui_enable_diff(tui, false);
        (void)solar_os_tui_enable_diff(tui, true);
    }
    return ESP_OK;
}

esp_err_t solar_os_tui_screen_begin(solar_os_tui_t *tui, solar_os_context_t *ctx)
{
    esp_err_t err = solar_os_tui_begin(tui, ctx);
    if (err != ESP_OK) return err;
    tui->screen_active = true;
    tui->saved_status_bar_visible = tui->terminal != NULL &&
        solar_os_terminal_status_bar_visible(tui->terminal);
    const bool compact = solar_os_tui_screen_should_fullscreen(
        solar_os_tui_rows(tui));
    if (compact) {
        err = tui_screen_set_fullscreen(tui, true);
    }
    if (err == ESP_OK) {
        err = solar_os_tui_enable_diff(tui, true);
    }
    if (err == ESP_OK) {
        solar_os_tui_attach_session(tui);
    } else {
        solar_os_tui_end(tui);
    }
    return err;
}

bool solar_os_tui_screen_should_fullscreen(size_t rows)
{
    return rows > 0U && rows <= SOLAR_OS_TUI_AUTO_FULLSCREEN_MAX_ROWS;
}

bool solar_os_tui_screen_fullscreen(const solar_os_tui_t *tui)
{
    return tui != NULL && tui->screen_active && tui->fullscreen;
}

size_t solar_os_tui_screen_bottom_rows(const solar_os_tui_t *tui,
                                       size_t normal_rows)
{
    return solar_os_tui_screen_fullscreen(tui) ? 0U : normal_rows;
}

size_t solar_os_tui_screen_content_rows(const solar_os_tui_t *tui,
                                        size_t top_rows,
                                        size_t normal_bottom_rows)
{
    const size_t rows = solar_os_tui_rows(tui);
    const size_t bottom_rows = solar_os_tui_screen_bottom_rows(
        tui, normal_bottom_rows);
    const size_t reserved = top_rows + bottom_rows;
    return rows > reserved ? rows - reserved : 0U;
}

size_t solar_os_tui_screen_content_end(const solar_os_tui_t *tui,
                                       size_t normal_bottom_rows)
{
    const size_t rows = solar_os_tui_rows(tui);
    const size_t bottom_rows = solar_os_tui_screen_bottom_rows(
        tui, normal_bottom_rows);
    return rows > bottom_rows ? rows - bottom_rows : 0U;
}

solar_os_tui_screen_key_action_t solar_os_tui_screen_key(solar_os_tui_t *tui,
                                                         uint8_t key)
{
    if (tui == NULL || !tui->screen_active) return SOLAR_OS_TUI_SCREEN_KEY_NONE;
    if (key == SOLAR_OS_KEY_ALT_PREFIX) {
        tui->alt_prefix_pending = true;
        return SOLAR_OS_TUI_SCREEN_KEY_CONSUMED;
    }
    if (!tui->alt_prefix_pending) return SOLAR_OS_TUI_SCREEN_KEY_NONE;

    tui->alt_prefix_pending = false;
    if (key != SOLAR_OS_KEY_ENTER && key != '\r') {
        return SOLAR_OS_TUI_SCREEN_KEY_PASSTHROUGH;
    }
    return tui_screen_set_fullscreen(tui, !tui->fullscreen) == ESP_OK ?
        SOLAR_OS_TUI_SCREEN_KEY_TOGGLED : SOLAR_OS_TUI_SCREEN_KEY_CONSUMED;
}

bool solar_os_tui_screen_layout(const solar_os_tui_t *tui,
                                size_t tab_rows,
                                size_t status_rows,
                                size_t input_rows,
                                solar_os_tui_screen_layout_t *layout)
{
    if (tui == NULL || layout == NULL) return false;
    if (solar_os_tui_screen_fullscreen(tui)) {
        return solar_os_tui_layout_compute_fullscreen(solar_os_tui_rows(tui),
                                                      solar_os_tui_cols(tui),
                                                      tab_rows, input_rows, layout);
    }
    return solar_os_tui_layout_compute(solar_os_tui_rows(tui),
                                       solar_os_tui_cols(tui),
                                       tab_rows, status_rows, input_rows, layout);
}

bool solar_os_tui_layout_compute_fullscreen(size_t rows,
                                            size_t cols,
                                            size_t tab_rows,
                                            size_t input_rows,
                                            solar_os_tui_screen_layout_t *layout)
{
    if (layout == NULL) return false;
    const size_t fixed = 1U + tab_rows + input_rows;
    memset(layout, 0, sizeof(*layout));
    if (cols == 0U || rows <= fixed) return false;

    layout->title = (solar_os_tui_rect_t){.row = 0, .col = 0, .height = 1, .width = cols};
    layout->tabs = (solar_os_tui_rect_t){.row = 1, .col = 0, .height = tab_rows, .width = cols};
    layout->help = (solar_os_tui_rect_t){.row = rows, .col = 0, .height = 0, .width = cols};
    layout->input = (solar_os_tui_rect_t){.row = rows - input_rows,
                                         .col = 0, .height = input_rows, .width = cols};
    layout->status = (solar_os_tui_rect_t){.row = layout->input.row,
                                          .col = 0, .height = 0, .width = cols};
    const size_t body_row = 1U + tab_rows;
    layout->body = (solar_os_tui_rect_t){.row = body_row, .col = 0,
                                        .height = layout->input.row - body_row,
                                        .width = cols};
    return layout->body.height > 0U;
}

bool solar_os_tui_layout_compute(size_t rows,
                                 size_t cols,
                                 size_t tab_rows,
                                 size_t status_rows,
                                 size_t input_rows,
                                 solar_os_tui_screen_layout_t *layout)
{
    if (layout == NULL) return false;
    const size_t fixed = 2U + tab_rows + status_rows + input_rows;
    memset(layout, 0, sizeof(*layout));
    if (cols == 0 || rows <= fixed) return false;

    layout->title = (solar_os_tui_rect_t){.row = 0, .col = 0, .height = 1, .width = cols};
    layout->tabs = (solar_os_tui_rect_t){.row = 1, .col = 0, .height = tab_rows, .width = cols};
    layout->help = (solar_os_tui_rect_t){.row = rows - 1U, .col = 0, .height = 1, .width = cols};
    layout->input = (solar_os_tui_rect_t){.row = layout->help.row - input_rows,
                                         .col = 0, .height = input_rows, .width = cols};
    layout->status = (solar_os_tui_rect_t){.row = layout->input.row - status_rows,
                                          .col = 0, .height = status_rows, .width = cols};
    const size_t body_row = 1U + tab_rows;
    layout->body = (solar_os_tui_rect_t){.row = body_row, .col = 0,
                                        .height = layout->status.row - body_row,
                                        .width = cols};
    return layout->body.height > 0;
}

esp_err_t solar_os_tui_write_cell(solar_os_tui_t *tui,
                                  size_t row,
                                  size_t col,
                                  size_t width,
                                  const char *text,
                                  uint8_t attr)
{
    if (tui == NULL || text == NULL || width == 0 ||
        row >= solar_os_tui_rows(tui) || col >= solar_os_tui_cols(tui)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t available = solar_os_tui_cols(tui) - col;
    if (width > available) width = available;
    esp_err_t err = solar_os_tui_fill(tui, row, col, 1, width, ' ', attr);
    if (err != ESP_OK) return err;

    size_t cell = 0;
    while (*text != '\0' && cell < width) {
        uint32_t codepoint = 0;
        const size_t consumed = solar_os_ui_utf8_decode(text, &codepoint);
        if (consumed == 0) break;
        err = solar_os_tui_putch(tui, row, col + cell, codepoint, attr);
        if (err != ESP_OK) return err;
        text += consumed;
        cell++;
    }
    return ESP_OK;
}

esp_err_t solar_os_tui_draw_title(solar_os_tui_t *tui,
                                  const char *title,
                                  const char *detail)
{
    if (tui == NULL || title == NULL) return ESP_ERR_INVALID_ARG;
    const size_t cols = solar_os_tui_cols(tui);
    esp_err_t err = solar_os_tui_write_cell(tui, 0, 0, cols, title,
        SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    if (err != ESP_OK || detail == NULL || detail[0] == '\0') return err;
    size_t detail_width = solar_os_ui_utf8_width(detail);
    if (detail_width > cols) detail_width = cols;
    return solar_os_tui_write_cell(tui, 0, cols - detail_width, detail_width, detail,
                                   SOLAR_OS_TUI_ATTR_INVERSE);
}

esp_err_t solar_os_tui_draw_help(solar_os_tui_t *tui, const char *text)
{
    if (tui == NULL || text == NULL || solar_os_tui_rows(tui) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_tui_screen_fullscreen(tui)) return ESP_OK;
    return solar_os_tui_write_cell(tui, solar_os_tui_rows(tui) - 1U, 0,
                                   solar_os_tui_cols(tui), text,
                                   SOLAR_OS_TUI_ATTR_INVERSE);
}

esp_err_t solar_os_tui_draw_footer(solar_os_tui_t *tui,
                                   const char *status,
                                   const char *help)
{
    if (tui == NULL || solar_os_tui_rows(tui) == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_tui_screen_fullscreen(tui)) {
        if (status == NULL || status[0] == '\0') return ESP_OK;
        return solar_os_tui_write_cell(tui, solar_os_tui_rows(tui) - 1U, 0U,
                                       solar_os_tui_cols(tui), status,
                                       SOLAR_OS_TUI_ATTR_INVERSE);
    }
    const char *text = status != NULL && status[0] != '\0' ? status : help;
    return text != NULL ? solar_os_tui_draw_help(tui, text) : ESP_OK;
}

esp_err_t solar_os_tui_draw_tab(solar_os_tui_t *tui,
                                size_t row,
                                size_t col,
                                size_t width,
                                const char *text,
                                bool selected)
{
    return solar_os_tui_write_cell(tui, row, col, width, text,
        selected ? (SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD)
                 : SOLAR_OS_TUI_ATTR_BOLD);
}

esp_err_t solar_os_tui_draw_too_small(solar_os_tui_t *tui, const char *name)
{
    char message[64];
    snprintf(message, sizeof(message), "%s: terminal too small",
             name != NULL && name[0] != '\0' ? name : "tui");
    solar_os_tui_clear(tui);
    return solar_os_tui_write_cell(tui, 0, 0, solar_os_tui_cols(tui), message,
                                   SOLAR_OS_TUI_ATTR_BOLD);
}

void solar_os_tui_viewport_reconcile(solar_os_tui_viewport_t *viewport,
                                     size_t item_count,
                                     size_t visible_rows)
{
    solar_os_ui_viewport_reconcile(viewport, item_count, visible_rows);
}

bool solar_os_tui_viewport_key(solar_os_tui_viewport_t *viewport,
                               uint8_t key,
                               size_t item_count,
                               size_t visible_rows,
                               bool wrap)
{
    return solar_os_ui_viewport_key(viewport, key, item_count, visible_rows, wrap);
}

solar_os_tui_input_action_t solar_os_tui_input_key(char *text, size_t capacity,
    solar_os_tui_input_state_t *state, uint32_t key, size_t visible_cells)
{
    return solar_os_ui_input_key(text, capacity, state, key, visible_cells);
}

solar_os_tui_input_action_t solar_os_tui_input_text(char *text, size_t capacity,
    solar_os_tui_input_state_t *state, const char *input, size_t length, size_t visible_cells)
{ return solar_os_ui_input_text(text, capacity, state, input, length, visible_cells); }

esp_err_t solar_os_tui_draw_input(solar_os_tui_t *tui,
                                  size_t row,
                                  size_t col,
                                  size_t width,
                                  const char *label,
                                  const char *text,
                                  solar_os_tui_input_state_t *state,
                                  uint8_t attr)
{
    return solar_os_tui_draw_input_ex(tui,
                                      row,
                                      col,
                                      width,
                                      label,
                                      text,
                                      state,
                                      attr,
                                      false);
}

esp_err_t solar_os_tui_draw_input_ex(solar_os_tui_t *tui,
                                     size_t row,
                                     size_t col,
                                     size_t width,
                                     const char *label,
                                     const char *text,
                                     solar_os_tui_input_state_t *state,
                                     uint8_t attr,
                                     bool masked)
{
    if (tui == NULL || label == NULL || text == NULL || state == NULL || width == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t label_cells = solar_os_ui_utf8_width(label);
    if (label_cells >= width) label_cells = width - 1U;
    const size_t text_cells = width - label_cells;
    solar_os_ui_input_reconcile(text, state, text_cells);
    esp_err_t err = solar_os_tui_write_cell(tui, row, col, width, "", attr);
    if (err != ESP_OK) return err;
    if (label_cells > 0) {
        err = solar_os_tui_write_cell(tui, row, col, label_cells, label, attr);
        if (err != ESP_OK) return err;
    }
    const char *display_text = text + state->view;
    char masked_text[SOLAR_OS_TERMINAL_MAX_COLS + 1U];
    if (masked) {
        size_t masked_len = 0;
        for (size_t pos = state->view;
             text[pos] != '\0' && masked_len < text_cells &&
                 masked_len < sizeof(masked_text) - 1U;
             masked_len++) {
            masked_text[masked_len] = '*';
            pos = solar_os_ui_utf8_next(text, pos);
        }
        masked_text[masked_len] = '\0';
        display_text = masked_text;
    }
    err = solar_os_tui_write_cell(tui, row, col + label_cells, text_cells,
                                  display_text, attr);
    if (err != ESP_OK) return err;
    size_t cursor_cells = 0;
    for (size_t pos = state->view; pos < state->cursor; cursor_cells++) {
        pos = solar_os_ui_utf8_next(text, pos);
    }
    if (cursor_cells >= text_cells) cursor_cells = text_cells - 1U;
    err = solar_os_tui_move(tui, row, col + label_cells + cursor_cells);
    if (err == ESP_OK) err = solar_os_tui_set_cursor_visible(tui, true);
    return err;
}

esp_err_t solar_os_tui_draw_form(solar_os_tui_t *tui, const solar_os_tui_form_t *form)
{
    if (!tui || !form || !form->viewport || (form->count && !form->row) ||
        (form->edit_state && (!form->edit_label || !form->edit_text))) return ESP_ERR_INVALID_ARG;
    const solar_os_tui_rect_t *bounds = &form->bounds;
    size_t rows = solar_os_tui_rows(tui), cols = solar_os_tui_cols(tui);
    if (!bounds->width || !bounds->height || bounds->row >= rows || bounds->col >= cols ||
        bounds->height > rows - bounds->row || bounds->width > cols - bounds->col) return ESP_ERR_INVALID_ARG;
    solar_os_tui_viewport_reconcile(form->viewport, form->count, bounds->height);
    esp_err_t error = solar_os_tui_set_cursor_visible(tui, false);
    if (error != ESP_OK) return error;
    char line[SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U];
    for (size_t row = 0; row < bounds->height; row++) {
        size_t index = form->viewport->top + row;
        line[0] = 0;
        if (index < form->count) {
            form->row(index, line, sizeof(line), form->user);
            line[sizeof(line) - 1U] = 0;
        } else if (!form->count && !row && form->empty_text) snprintf(line, sizeof(line), "%s", form->empty_text);
        error = solar_os_tui_write_cell(tui, bounds->row + row, bounds->col, bounds->width,
            line, index < form->count && index == form->viewport->cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
        if (error != ESP_OK) return error;
    }
    if (form->edit_state && form->count) {
        return solar_os_tui_draw_input_ex(tui,
            bounds->row + form->viewport->cursor - form->viewport->top,
            bounds->col, bounds->width, form->edit_label, form->edit_text,
            form->edit_state, SOLAR_OS_TUI_ATTR_INVERSE, form->masked);
    }
    return ESP_OK;
}
