#include "solar_os_tui_widgets.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_keys.h"
#include "solar_os_terminal.h"

static size_t tui_widget_decode(const char *text, uint32_t *codepoint)
{
    const unsigned char *p = (const unsigned char *)text;
    if (p == NULL || codepoint == NULL || p[0] == '\0') return 0;
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1;
    }
    if ((p[0] & 0xe0U) == 0xc0U && p[1] != '\0' &&
        (p[1] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x1fU) << 6) | (uint32_t)(p[1] & 0x3fU);
        return 2;
    }
    if ((p[0] & 0xf0U) == 0xe0U && p[1] != '\0' && p[2] != '\0' &&
        (p[1] & 0xc0U) == 0x80U &&
        (p[2] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x0fU) << 12) |
            ((uint32_t)(p[1] & 0x3fU) << 6) | (uint32_t)(p[2] & 0x3fU);
        return 3;
    }
    if ((p[0] & 0xf8U) == 0xf0U && p[1] != '\0' && p[2] != '\0' &&
        p[3] != '\0' && (p[1] & 0xc0U) == 0x80U &&
        (p[2] & 0xc0U) == 0x80U && (p[3] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x07U) << 18) |
            ((uint32_t)(p[1] & 0x3fU) << 12) |
            ((uint32_t)(p[2] & 0x3fU) << 6) | (uint32_t)(p[3] & 0x3fU);
        return 4;
    }
    *codepoint = '?';
    return 1;
}

static size_t tui_widget_encode(uint32_t codepoint, char out[4])
{
    if (codepoint <= 0x7fU) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7ffU) {
        out[0] = (char)(0xc0U | (codepoint >> 6));
        out[1] = (char)(0x80U | (codepoint & 0x3fU));
        return 2;
    }
    if (codepoint <= 0xffffU) {
        out[0] = (char)(0xe0U | (codepoint >> 12));
        out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        out[2] = (char)(0x80U | (codepoint & 0x3fU));
        return 3;
    }
    if (codepoint <= 0x10ffffU) {
        out[0] = (char)(0xf0U | (codepoint >> 18));
        out[1] = (char)(0x80U | ((codepoint >> 12) & 0x3fU));
        out[2] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        out[3] = (char)(0x80U | (codepoint & 0x3fU));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static size_t tui_widget_width(const char *text)
{
    size_t width = 0;
    while (text != NULL && *text != '\0') {
        uint32_t codepoint = 0;
        const size_t consumed = tui_widget_decode(text, &codepoint);
        if (consumed == 0) break;
        text += consumed;
        width++;
    }
    return width;
}

static size_t tui_widget_previous(const char *text, size_t offset)
{
    if (text == NULL || offset == 0) return 0;
    offset--;
    while (offset > 0 && (((unsigned char)text[offset] & 0xc0U) == 0x80U)) offset--;
    return offset;
}

static size_t tui_widget_next(const char *text, size_t offset)
{
    if (text == NULL || text[offset] == '\0') return offset;
    uint32_t codepoint = 0;
    const size_t consumed = tui_widget_decode(text + offset, &codepoint);
    return offset + (consumed > 0 ? consumed : 1U);
}

esp_err_t solar_os_tui_screen_begin(solar_os_tui_t *tui, solar_os_context_t *ctx)
{
    esp_err_t err = solar_os_tui_begin(tui, ctx);
    if (err == ESP_OK) {
        err = solar_os_tui_enable_diff(tui, true);
        if (err != ESP_OK) solar_os_tui_end(tui);
    }
    return err;
}

bool solar_os_tui_screen_layout(const solar_os_tui_t *tui,
                                size_t tab_rows,
                                size_t status_rows,
                                size_t input_rows,
                                solar_os_tui_screen_layout_t *layout)
{
    if (tui == NULL || layout == NULL) return false;
    return solar_os_tui_layout_compute(solar_os_tui_rows(tui),
                                       solar_os_tui_cols(tui),
                                       tab_rows, status_rows, input_rows, layout);
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
        const size_t consumed = tui_widget_decode(text, &codepoint);
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
    size_t detail_width = tui_widget_width(detail);
    if (detail_width > cols) detail_width = cols;
    return solar_os_tui_write_cell(tui, 0, cols - detail_width, detail_width, detail,
                                   SOLAR_OS_TUI_ATTR_INVERSE);
}

esp_err_t solar_os_tui_draw_help(solar_os_tui_t *tui, const char *text)
{
    if (tui == NULL || text == NULL || solar_os_tui_rows(tui) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_tui_write_cell(tui, solar_os_tui_rows(tui) - 1U, 0,
                                   solar_os_tui_cols(tui), text,
                                   SOLAR_OS_TUI_ATTR_INVERSE);
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
    if (viewport == NULL) return;
    if (item_count == 0 || visible_rows == 0) {
        viewport->cursor = 0;
        viewport->top = 0;
        return;
    }
    if (viewport->cursor >= item_count) viewport->cursor = item_count - 1U;
    if (viewport->top > viewport->cursor) viewport->top = viewport->cursor;
    if (viewport->cursor >= viewport->top + visible_rows) {
        viewport->top = viewport->cursor - visible_rows + 1U;
    }
    const size_t max_top = item_count > visible_rows ? item_count - visible_rows : 0;
    if (viewport->top > max_top) viewport->top = max_top;
}

bool solar_os_tui_viewport_key(solar_os_tui_viewport_t *viewport,
                               uint8_t key,
                               size_t item_count,
                               size_t visible_rows,
                               bool wrap)
{
    if (viewport == NULL || item_count == 0 || visible_rows == 0) return false;
    solar_os_tui_viewport_reconcile(viewport, item_count, visible_rows);
    const size_t before = viewport->cursor;
    switch (key) {
    case SOLAR_OS_KEY_UP:
        viewport->cursor = before > 0 ? before - 1U : (wrap ? item_count - 1U : 0);
        break;
    case SOLAR_OS_KEY_DOWN:
        viewport->cursor = before + 1U < item_count ? before + 1U : (wrap ? 0 : before);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        viewport->cursor = before > visible_rows ? before - visible_rows : 0;
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        viewport->cursor = before + visible_rows < item_count ?
            before + visible_rows : item_count - 1U;
        break;
    case SOLAR_OS_KEY_HOME:
        viewport->cursor = 0;
        break;
    case SOLAR_OS_KEY_END:
        viewport->cursor = item_count - 1U;
        break;
    default:
        return false;
    }
    solar_os_tui_viewport_reconcile(viewport, item_count, visible_rows);
    return viewport->cursor != before;
}

static void tui_widget_input_visible(const char *text,
                                     solar_os_tui_input_state_t *state,
                                     size_t visible_cells)
{
    const size_t len = strlen(text);
    if (state->cursor > len) state->cursor = len;
    if (state->view > state->cursor) state->view = state->cursor;
    while (state->view > 0) {
        size_t cells = 0;
        for (size_t pos = state->view; pos < state->cursor; cells++) pos = tui_widget_next(text, pos);
        if (cells < visible_cells) state->view = tui_widget_previous(text, state->view);
        else break;
    }
    for (;;) {
        size_t cells = 0;
        for (size_t pos = state->view; pos < state->cursor; cells++) pos = tui_widget_next(text, pos);
        if (cells < visible_cells || state->view >= state->cursor) break;
        state->view = tui_widget_next(text, state->view);
    }
}

solar_os_tui_input_action_t solar_os_tui_input_key(
    char *text,
    size_t capacity,
    solar_os_tui_input_state_t *state,
    uint32_t key,
    size_t visible_cells)
{
    if (text == NULL || capacity == 0 || state == NULL) return SOLAR_OS_TUI_INPUT_NONE;
    const size_t len = strnlen(text, capacity);
    if (len >= capacity) return SOLAR_OS_TUI_INPUT_NONE;
    if (state->cursor > len) state->cursor = len;
    solar_os_tui_input_action_t action = SOLAR_OS_TUI_INPUT_NONE;
    if (key == SOLAR_OS_KEY_ENTER) return SOLAR_OS_TUI_INPUT_SUBMIT;
    if (key == SOLAR_OS_KEY_ESCAPE) return SOLAR_OS_TUI_INPUT_CANCEL;
    if (key == SOLAR_OS_KEY_LEFT) state->cursor = tui_widget_previous(text, state->cursor);
    else if (key == SOLAR_OS_KEY_RIGHT) state->cursor = tui_widget_next(text, state->cursor);
    else if (key == SOLAR_OS_KEY_CTRL_LEFT) {
        while (state->cursor > 0 && isspace((unsigned char)text[state->cursor - 1U])) {
            state->cursor = tui_widget_previous(text, state->cursor);
        }
        while (state->cursor > 0 &&
               !isspace((unsigned char)text[state->cursor - 1U])) {
            state->cursor = tui_widget_previous(text, state->cursor);
        }
    } else if (key == SOLAR_OS_KEY_CTRL_RIGHT) {
        while (state->cursor < len &&
               !isspace((unsigned char)text[state->cursor])) {
            state->cursor = tui_widget_next(text, state->cursor);
        }
        while (state->cursor < len && isspace((unsigned char)text[state->cursor])) {
            state->cursor = tui_widget_next(text, state->cursor);
        }
    } else if (key == SOLAR_OS_KEY_HOME) state->cursor = 0;
    else if (key == SOLAR_OS_KEY_END) state->cursor = len;
    else if (key == '\b' || key == 0x7fU) {
        const size_t previous = tui_widget_previous(text, state->cursor);
        if (previous != state->cursor) {
            memmove(text + previous, text + state->cursor, len - state->cursor + 1U);
            state->cursor = previous;
            action = SOLAR_OS_TUI_INPUT_CHANGED;
        }
    } else if (key == SOLAR_OS_KEY_DELETE) {
        const size_t next = tui_widget_next(text, state->cursor);
        if (next != state->cursor) {
            memmove(text + state->cursor, text + next, len - next + 1U);
            action = SOLAR_OS_TUI_INPUT_CHANGED;
        }
    } else if (key >= 0x20U && key != 0x7fU && key < SOLAR_OS_KEY_UP) {
        char bytes[4];
        const size_t count = tui_widget_encode(key, bytes);
        if (len + count < capacity) {
            memmove(text + state->cursor + count, text + state->cursor,
                    len - state->cursor + 1U);
            memcpy(text + state->cursor, bytes, count);
            state->cursor += count;
            action = SOLAR_OS_TUI_INPUT_CHANGED;
        }
    }
    tui_widget_input_visible(text, state, visible_cells > 0 ? visible_cells : 1U);
    return action;
}

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
    size_t label_cells = tui_widget_width(label);
    if (label_cells >= width) label_cells = width - 1U;
    const size_t text_cells = width - label_cells;
    tui_widget_input_visible(text, state, text_cells);
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
            pos = tui_widget_next(text, pos);
        }
        masked_text[masked_len] = '\0';
        display_text = masked_text;
    }
    err = solar_os_tui_write_cell(tui, row, col + label_cells, text_cells,
                                  display_text, attr);
    if (err != ESP_OK) return err;
    size_t cursor_cells = 0;
    for (size_t pos = state->view; pos < state->cursor; cursor_cells++) {
        pos = tui_widget_next(text, pos);
    }
    if (cursor_cells >= text_cells) cursor_cells = text_cells - 1U;
    err = solar_os_tui_move(tui, row, col + label_cells + cursor_cells);
    if (err == ESP_OK) err = solar_os_tui_set_cursor_visible(tui, true);
    return err;
}
