#include "solar_os_tui.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_memory.h"
#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"

#define TUI_BOX_H 0x2500U
#define TUI_BOX_V 0x2502U
#define TUI_BOX_TL 0x250cU
#define TUI_BOX_TR 0x2510U
#define TUI_BOX_BL 0x2514U
#define TUI_BOX_BR 0x2518U
#define TUI_SHADOW_INVALID 0xffffffffU

static bool tui_valid(const solar_os_tui_t *tui)
{
    return tui != NULL && tui->io != NULL &&
        solar_os_shell_io_kind(tui->io) != SOLAR_OS_SHELL_IO_KIND_NONE;
}

static bool tui_diff_active(const solar_os_tui_t *tui)
{
    return tui_valid(tui) &&
        tui->diff_enabled &&
        tui->diff_ready &&
        solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_PORT &&
        solar_os_shell_io_is_cursor_addressable(tui->io);
}

static uint8_t tui_attr_from_io(const solar_os_shell_io_t *io)
{
    uint8_t attr = SOLAR_OS_TUI_ATTR_NORMAL;
    if (io != NULL && io->bold) {
        attr |= SOLAR_OS_TUI_ATTR_BOLD;
    }
    if (io != NULL && io->italic) {
        attr |= SOLAR_OS_TUI_ATTR_ITALIC;
    }
    if (io != NULL && io->underline) {
        attr |= SOLAR_OS_TUI_ATTR_UNDERLINE;
    }
    if (io != NULL && io->inverse) {
        attr |= SOLAR_OS_TUI_ATTR_INVERSE;
    }
    return attr;
}

static void tui_set_attr(solar_os_tui_t *tui, uint8_t attr)
{
    if (!tui_valid(tui)) {
        return;
    }
    if (tui_diff_active(tui)) {
        tui->draw_attr = attr;
        return;
    }

    (void)solar_os_shell_io_set_bold(tui->io, (attr & SOLAR_OS_TUI_ATTR_BOLD) != 0);
    (void)solar_os_shell_io_set_italic(tui->io, (attr & SOLAR_OS_TUI_ATTR_ITALIC) != 0);
    (void)solar_os_shell_io_set_underline(tui->io, (attr & SOLAR_OS_TUI_ATTR_UNDERLINE) != 0);
    (void)solar_os_shell_io_set_inverse(tui->io, (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0);
}

static void tui_restore_attr(solar_os_tui_t *tui,
                             bool bold,
                             bool italic,
                             bool underline,
                             bool inverse)
{
    if (!tui_valid(tui)) {
        return;
    }
    if (tui_diff_active(tui)) {
        tui->draw_attr = SOLAR_OS_TUI_ATTR_NORMAL |
            (bold ? SOLAR_OS_TUI_ATTR_BOLD : 0) |
            (italic ? SOLAR_OS_TUI_ATTR_ITALIC : 0) |
            (underline ? SOLAR_OS_TUI_ATTR_UNDERLINE : 0) |
            (inverse ? SOLAR_OS_TUI_ATTR_INVERSE : 0);
        return;
    }

    (void)solar_os_shell_io_set_bold(tui->io, bold);
    (void)solar_os_shell_io_set_italic(tui->io, italic);
    (void)solar_os_shell_io_set_underline(tui->io, underline);
    (void)solar_os_shell_io_set_inverse(tui->io, inverse);
}

static void tui_save_attr(const solar_os_tui_t *tui,
                          bool *bold,
                          bool *italic,
                          bool *underline,
                          bool *inverse)
{
    if (bold != NULL) {
        *bold = tui_diff_active(tui) ?
            (tui->draw_attr & SOLAR_OS_TUI_ATTR_BOLD) != 0 :
            (tui != NULL && tui->io != NULL && tui->io->bold);
    }
    if (italic != NULL) {
        *italic = tui_diff_active(tui) ?
            (tui->draw_attr & SOLAR_OS_TUI_ATTR_ITALIC) != 0 :
            (tui != NULL && tui->io != NULL && tui->io->italic);
    }
    if (underline != NULL) {
        *underline = tui_diff_active(tui) ?
            (tui->draw_attr & SOLAR_OS_TUI_ATTR_UNDERLINE) != 0 :
            (tui != NULL && tui->io != NULL && tui->io->underline);
    }
    if (inverse != NULL) {
        *inverse = tui_diff_active(tui) ?
            (tui->draw_attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0 :
            (tui != NULL && tui->io != NULL && tui->io->inverse);
    }
}

static size_t tui_rows(const solar_os_tui_t *tui)
{
    return tui_valid(tui) ? solar_os_shell_io_rows(tui->io) : 0;
}

static size_t tui_cols(const solar_os_tui_t *tui)
{
    return tui_valid(tui) ? solar_os_shell_io_cols(tui->io) : 0;
}

static esp_err_t tui_validate_origin(const solar_os_tui_t *tui, size_t row, size_t col)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (row >= tui_rows(tui) || col >= tui_cols(tui)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static size_t tui_encode_utf8(uint32_t codepoint, char out[4])
{
    if (out == NULL) {
        return 0;
    }

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

static void tui_free_diff_buffers(solar_os_tui_t *tui)
{
    if (tui == NULL) {
        return;
    }
    if (tui->front_codepoints != NULL) {
        heap_caps_free(tui->front_codepoints);
        tui->front_codepoints = NULL;
    }
    if (tui->back_codepoints != NULL) {
        heap_caps_free(tui->back_codepoints);
        tui->back_codepoints = NULL;
    }
    if (tui->front_attrs != NULL) {
        heap_caps_free(tui->front_attrs);
        tui->front_attrs = NULL;
    }
    if (tui->back_attrs != NULL) {
        heap_caps_free(tui->back_attrs);
        tui->back_attrs = NULL;
    }
    tui->diff_ready = false;
    tui->diff_cols = 0;
    tui->diff_rows = 0;
}

static esp_err_t tui_prepare_diff_buffers(solar_os_tui_t *tui)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (solar_os_shell_io_kind(tui->io) != SOLAR_OS_SHELL_IO_KIND_PORT) {
        return ESP_OK;
    }
    if (!solar_os_shell_io_is_cursor_addressable(tui->io)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint16_t cols = solar_os_shell_io_cols(tui->io);
    const uint16_t rows = solar_os_shell_io_rows(tui->io);
    if (cols == 0 || rows == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tui->diff_ready && tui->diff_cols == cols && tui->diff_rows == rows) {
        return ESP_OK;
    }

    tui_free_diff_buffers(tui);

    const size_t cells = (size_t)cols * (size_t)rows;
    tui->front_codepoints = solar_os_psram_malloc(cells * sizeof(tui->front_codepoints[0]));
    tui->back_codepoints = solar_os_psram_malloc(cells * sizeof(tui->back_codepoints[0]));
    tui->front_attrs = solar_os_psram_malloc(cells * sizeof(tui->front_attrs[0]));
    tui->back_attrs = solar_os_psram_malloc(cells * sizeof(tui->back_attrs[0]));
    if (tui->front_codepoints == NULL ||
        tui->back_codepoints == NULL ||
        tui->front_attrs == NULL ||
        tui->back_attrs == NULL) {
        tui_free_diff_buffers(tui);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < cells; i++) {
        tui->front_codepoints[i] = ' ';
        tui->front_attrs[i] = SOLAR_OS_TUI_ATTR_NORMAL;
        tui->back_codepoints[i] = TUI_SHADOW_INVALID;
        tui->back_attrs[i] = 0xffU;
    }
    tui->diff_cols = cols;
    tui->diff_rows = rows;
    tui->diff_ready = true;
    tui->draw_row = 0;
    tui->draw_col = 0;
    tui->cursor_row = 0;
    tui->cursor_col = 0;
    tui->cursor_visible = solar_os_shell_io_cursor_visible(tui->io);
    return ESP_OK;
}

static void tui_buffer_clear(solar_os_tui_t *tui)
{
    if (!tui_diff_active(tui)) {
        return;
    }

    const size_t cells = (size_t)tui->diff_cols * (size_t)tui->diff_rows;
    for (size_t i = 0; i < cells; i++) {
        tui->front_codepoints[i] = ' ';
        tui->front_attrs[i] = SOLAR_OS_TUI_ATTR_NORMAL;
    }
    tui->draw_row = 0;
    tui->draw_col = 0;
    tui->cursor_row = 0;
    tui->cursor_col = 0;
}

static void tui_buffer_track_cell(solar_os_tui_t *tui)
{
    if (!tui_diff_active(tui)) {
        return;
    }

    tui->draw_col++;
    if (tui->draw_col >= tui->diff_cols) {
        tui->draw_col = 0;
        if (tui->draw_row + 1U < tui->diff_rows) {
            tui->draw_row++;
        }
    }
    tui->cursor_row = tui->draw_row;
    tui->cursor_col = tui->draw_col;
}

static esp_err_t tui_buffer_put_codepoint(solar_os_tui_t *tui, uint32_t codepoint)
{
    if (!tui_diff_active(tui)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (codepoint == '\r') {
        tui->draw_col = 0;
        tui->cursor_row = tui->draw_row;
        tui->cursor_col = tui->draw_col;
        return ESP_OK;
    }
    if (codepoint == '\n') {
        tui->draw_col = 0;
        if (tui->draw_row + 1U < tui->diff_rows) {
            tui->draw_row++;
        }
        tui->cursor_row = tui->draw_row;
        tui->cursor_col = tui->draw_col;
        return ESP_OK;
    }
    if (codepoint == '\b') {
        if (tui->draw_col > 0) {
            tui->draw_col--;
        }
        tui->cursor_row = tui->draw_row;
        tui->cursor_col = tui->draw_col;
        return ESP_OK;
    }
    if (codepoint == '\t') {
        do {
            esp_err_t err = tui_buffer_put_codepoint(tui, ' ');
            if (err != ESP_OK) {
                return err;
            }
        } while ((tui->draw_col % 4U) != 0U);
        return ESP_OK;
    }

    if (tui->draw_row >= tui->diff_rows || tui->draw_col >= tui->diff_cols) {
        return ESP_OK;
    }

    const size_t index = (size_t)tui->draw_row * tui->diff_cols + tui->draw_col;
    tui->front_codepoints[index] = codepoint >= 0x20U ? codepoint : ' ';
    tui->front_attrs[index] = tui->draw_attr;
    tui_buffer_track_cell(tui);
    return ESP_OK;
}

static size_t tui_decode_utf8_char(const char *text, uint32_t *codepoint)
{
    const unsigned char *p = (const unsigned char *)text;
    if (p == NULL || codepoint == NULL || p[0] == '\0') {
        return 0;
    }
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1;
    }
    if ((p[0] & 0xe0U) == 0xc0U &&
        (p[1] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x1fU) << 6) |
            (uint32_t)(p[1] & 0x3fU);
        return 2;
    }
    if ((p[0] & 0xf0U) == 0xe0U &&
        (p[1] & 0xc0U) == 0x80U &&
        (p[2] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x0fU) << 12) |
            ((uint32_t)(p[1] & 0x3fU) << 6) |
            (uint32_t)(p[2] & 0x3fU);
        return 3;
    }
    if ((p[0] & 0xf8U) == 0xf0U &&
        (p[1] & 0xc0U) == 0x80U &&
        (p[2] & 0xc0U) == 0x80U &&
        (p[3] & 0xc0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x07U) << 18) |
            ((uint32_t)(p[1] & 0x3fU) << 12) |
            ((uint32_t)(p[2] & 0x3fU) << 6) |
            (uint32_t)(p[3] & 0x3fU);
        return 4;
    }
    *codepoint = '?';
    return 1;
}

static esp_err_t tui_buffer_write_text(solar_os_tui_t *tui, const char *text)
{
    if (!tui_diff_active(tui) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = text;
    while (*p != '\0') {
        uint32_t codepoint = 0;
        const size_t consumed = tui_decode_utf8_char(p, &codepoint);
        if (consumed == 0) {
            return ESP_FAIL;
        }
        esp_err_t err = tui_buffer_put_codepoint(tui, codepoint);
        if (err != ESP_OK) {
            return err;
        }
        p += consumed;
    }
    return ESP_OK;
}

static void tui_track_cell(solar_os_tui_t *tui)
{
    if (!tui_valid(tui) || tui->io->cols == 0) {
        return;
    }

    tui->io->cursor_col++;
    if (tui->io->cursor_col >= tui->io->cols) {
        tui->io->cursor_col = 0;
        if (tui->io->rows == 0 || tui->io->cursor_row + 1U < tui->io->rows) {
            tui->io->cursor_row++;
        }
    }
}

static esp_err_t tui_write_codepoint(solar_os_tui_t *tui, uint32_t codepoint)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tui_diff_active(tui)) {
        return tui_buffer_put_codepoint(tui, codepoint);
    }

    if (solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
        tui->terminal != NULL) {
        solar_os_terminal_put_codepoint(tui->terminal, codepoint);
        tui->io->cursor_row = solar_os_terminal_cursor_row(tui->terminal);
        tui->io->cursor_col = solar_os_terminal_cursor_col(tui->terminal);
        return ESP_OK;
    }

    char bytes[4];
    const size_t len = tui_encode_utf8(codepoint, bytes);
    const esp_err_t err = solar_os_shell_io_write_raw(tui->io, bytes, len);
    if (err == ESP_OK) {
        tui_track_cell(tui);
    }
    return err;
}

static esp_err_t tui_write_text(solar_os_tui_t *tui, const char *text)
{
    if (!tui_valid(tui) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tui_diff_active(tui)) {
        return tui_buffer_write_text(tui, text);
    }

    if (solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
        tui->terminal != NULL) {
        solar_os_terminal_write_utf8(tui->terminal, text);
        tui->io->cursor_row = solar_os_terminal_cursor_row(tui->terminal);
        tui->io->cursor_col = solar_os_terminal_cursor_col(tui->terminal);
        return ESP_OK;
    }

    return solar_os_shell_io_write(tui->io, text);
}

esp_err_t solar_os_tui_begin(solar_os_tui_t *tui, solar_os_context_t *ctx)
{
    if (tui == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(tui, 0, sizeof(*tui));
    tui->io = solar_os_context_shell_io(ctx);
    if (tui->io == NULL ||
        solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&tui->fallback_io, solar_os_context_terminal(ctx));
        tui->io = &tui->fallback_io;
    }
    tui->terminal = solar_os_shell_io_terminal(tui->io);
    if (!solar_os_shell_io_is_cursor_addressable(tui->io)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    tui->saved_attr = tui_attr_from_io(tui->io);
    tui->draw_attr = tui->saved_attr;
    tui->draw_row = solar_os_shell_io_cursor_row(tui->io);
    tui->draw_col = solar_os_shell_io_cursor_col(tui->io);
    tui->cursor_row = tui->draw_row;
    tui->cursor_col = tui->draw_col;
    tui->saved_cursor_visible = solar_os_shell_io_cursor_visible(tui->io);
    tui->cursor_visible = tui->saved_cursor_visible;
    return tui_valid(tui) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void solar_os_tui_end(solar_os_tui_t *tui)
{
    if (tui == NULL) {
        return;
    }
    tui_free_diff_buffers(tui);
    tui->diff_enabled = false;
    if (tui_valid(tui)) {
        tui_set_attr(tui, tui->saved_attr);
        tui->draw_attr = tui->saved_attr;
        tui->cursor_visible = tui->saved_cursor_visible;
        (void)solar_os_shell_io_set_cursor_visible(tui->io, tui->saved_cursor_visible);
        (void)solar_os_shell_io_flush(tui->io);
    }
}

esp_err_t solar_os_tui_enable_diff(solar_os_tui_t *tui, bool enabled)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        tui_free_diff_buffers(tui);
        tui->diff_enabled = false;
        return ESP_OK;
    }

    tui->diff_enabled = true;
    const esp_err_t err = tui_prepare_diff_buffers(tui);
    if (err != ESP_OK) {
        tui->diff_enabled = false;
        return err;
    }
    return ESP_OK;
}

size_t solar_os_tui_rows(const solar_os_tui_t *tui)
{
    return tui_rows(tui);
}

size_t solar_os_tui_cols(const solar_os_tui_t *tui)
{
    return tui_cols(tui);
}

void solar_os_tui_clear(solar_os_tui_t *tui)
{
    if (tui_diff_active(tui)) {
        tui_buffer_clear(tui);
        return;
    }
    if (tui_valid(tui)) {
        (void)solar_os_shell_io_clear(tui->io);
    }
}

static esp_err_t tui_emit_attr(solar_os_tui_t *tui, uint8_t attr)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    const bool bold = (attr & SOLAR_OS_TUI_ATTR_BOLD) != 0;
    const bool italic = (attr & SOLAR_OS_TUI_ATTR_ITALIC) != 0;
    const bool underline = (attr & SOLAR_OS_TUI_ATTR_UNDERLINE) != 0;
    const bool inverse = (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0;

    if (tui->io->bold != bold) {
        err = solar_os_shell_io_set_bold(tui->io, bold);
    }
    if (err == ESP_OK && tui->io->italic != italic) {
        err = solar_os_shell_io_set_italic(tui->io, italic);
    }
    if (err == ESP_OK && tui->io->underline != underline) {
        err = solar_os_shell_io_set_underline(tui->io, underline);
    }
    if (err == ESP_OK && tui->io->inverse != inverse) {
        err = solar_os_shell_io_set_inverse(tui->io, inverse);
    }
    return err;
}

static esp_err_t tui_emit_codepoint(solar_os_tui_t *tui, uint32_t codepoint)
{
    char bytes[4];
    const size_t len = tui_encode_utf8(codepoint, bytes);
    const esp_err_t err = solar_os_shell_io_write_raw(tui->io, bytes, len);
    if (err == ESP_OK) {
        tui_track_cell(tui);
    }
    return err;
}

static void tui_mark_shadow_invalid(solar_os_tui_t *tui)
{
    if (!tui_diff_active(tui)) {
        return;
    }
    const size_t cells = (size_t)tui->diff_cols * (size_t)tui->diff_rows;
    for (size_t i = 0; i < cells; i++) {
        tui->back_codepoints[i] = TUI_SHADOW_INVALID;
        tui->back_attrs[i] = 0xffU;
    }
}

static esp_err_t tui_refresh_diff(solar_os_tui_t *tui)
{
    esp_err_t err = tui_prepare_diff_buffers(tui);
    if (err != ESP_OK) {
        return err;
    }
    if (!tui_diff_active(tui)) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t cells = (size_t)tui->diff_cols * (size_t)tui->diff_rows;
    bool any_changed = false;
    for (size_t i = 0; i < cells; i++) {
        if (tui->front_codepoints[i] != tui->back_codepoints[i] ||
            tui->front_attrs[i] != tui->back_attrs[i]) {
            any_changed = true;
            break;
        }
    }

    if (any_changed) {
        for (size_t row = 0; row < tui->diff_rows; row++) {
            size_t col = 0;
            while (col < tui->diff_cols) {
                const size_t index = row * tui->diff_cols + col;
                if (tui->front_codepoints[index] == tui->back_codepoints[index] &&
                    tui->front_attrs[index] == tui->back_attrs[index]) {
                    col++;
                    continue;
                }

                err = solar_os_shell_io_set_cursor(tui->io, row, col);
                if (err != ESP_OK) {
                    return err;
                }
                err = tui_emit_attr(tui, tui->front_attrs[index]);
                if (err != ESP_OK) {
                    return err;
                }

                const uint8_t attr = tui->front_attrs[index];
                while (col < tui->diff_cols) {
                    const size_t run_index = row * tui->diff_cols + col;
                    if (tui->front_attrs[run_index] != attr ||
                        (tui->front_codepoints[run_index] == tui->back_codepoints[run_index] &&
                         tui->front_attrs[run_index] == tui->back_attrs[run_index])) {
                        break;
                    }
                    err = tui_emit_codepoint(tui, tui->front_codepoints[run_index]);
                    if (err != ESP_OK) {
                        return err;
                    }
                    col++;
                }
            }
        }
        memcpy(tui->back_codepoints, tui->front_codepoints, cells * sizeof(tui->front_codepoints[0]));
        memcpy(tui->back_attrs, tui->front_attrs, cells * sizeof(tui->front_attrs[0]));
    }

    const size_t cursor_row = tui->cursor_row < tui->diff_rows ?
        tui->cursor_row : tui->diff_rows - 1U;
    const size_t cursor_col = tui->cursor_col < tui->diff_cols ?
        tui->cursor_col : tui->diff_cols - 1U;
    err = solar_os_shell_io_set_cursor(tui->io, cursor_row, cursor_col);
    if (err == ESP_OK && solar_os_shell_io_cursor_visible(tui->io) != tui->cursor_visible) {
        err = solar_os_shell_io_set_cursor_visible(tui->io, tui->cursor_visible);
    }
    if (err == ESP_OK) {
        err = solar_os_shell_io_flush(tui->io);
    }
    return err;
}

void solar_os_tui_refresh(solar_os_tui_t *tui)
{
    if (tui != NULL && tui->diff_enabled &&
        solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_PORT &&
        solar_os_shell_io_is_cursor_addressable(tui->io)) {
        if (tui_prepare_diff_buffers(tui) == ESP_OK) {
            (void)tui_refresh_diff(tui);
            return;
        }
    }
    if (tui_valid(tui)) {
        (void)solar_os_shell_io_flush(tui->io);
    }
}

esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }
    if (tui_diff_active(tui)) {
        tui->draw_row = row;
        tui->draw_col = col;
        tui->cursor_row = row;
        tui->cursor_col = col;
        return ESP_OK;
    }

    return solar_os_shell_io_set_cursor(tui->io, row, col);
}

esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tui_diff_active(tui)) {
        tui->cursor_visible = visible;
        return ESP_OK;
    }

    return solar_os_shell_io_set_cursor_visible(tui->io, visible);
}

esp_err_t solar_os_tui_write(solar_os_tui_t *tui, const char *text, uint8_t attr)
{
    if (!tui_valid(tui) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    const esp_err_t err = tui_write_text(tui, text);
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return err;
}

esp_err_t solar_os_tui_addstr(solar_os_tui_t *tui,
                              size_t row,
                              size_t col,
                              const char *text,
                              uint8_t attr)
{
    esp_err_t err = solar_os_tui_move(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_tui_write(tui, text, attr);
}

esp_err_t solar_os_tui_putch(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             uint32_t codepoint,
                             uint8_t attr)
{
    esp_err_t err = solar_os_tui_move(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    const esp_err_t write_err = tui_write_codepoint(tui, codepoint);
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return write_err;
}

esp_err_t solar_os_tui_hline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t width,
                             uint32_t codepoint,
                             uint8_t attr)
{
    if (width == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t cols = tui_cols(tui);
    const size_t draw_width = col + width > cols ? cols - col : width;
    const uint32_t glyph = codepoint != 0 ? codepoint : TUI_BOX_H;

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    err = solar_os_tui_move(tui, row, col);
    for (size_t i = 0; i < draw_width; i++) {
        if (err == ESP_OK) {
            err = tui_write_codepoint(tui, glyph);
        }
    }
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return err;
}

esp_err_t solar_os_tui_vline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint32_t codepoint,
                             uint8_t attr)
{
    if (height == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t rows = tui_rows(tui);
    const size_t draw_height = row + height > rows ? rows - row : height;
    const uint32_t glyph = codepoint != 0 ? codepoint : TUI_BOX_V;

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    esp_err_t write_err = ESP_OK;
    for (size_t i = 0; i < draw_height; i++) {
        esp_err_t err = solar_os_tui_move(tui, row + i, col);
        if (err == ESP_OK) {
            err = tui_write_codepoint(tui, glyph);
        }
        if (write_err == ESP_OK) {
            write_err = err;
        }
    }
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return write_err;
}

esp_err_t solar_os_tui_vrule(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint8_t width,
                             uint8_t attr)
{
    if (height == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    if (solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
        tui->terminal != NULL) {
        return solar_os_terminal_add_vrule(tui->terminal,
                                           row,
                                           col,
                                           height,
                                           width,
                                           (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0);
    }

    esp_err_t write_err = ESP_OK;
    for (uint8_t i = 0; i < width; i++) {
        const esp_err_t err = solar_os_tui_vline(tui, row, col + i, height, TUI_BOX_V, attr);
        if (write_err == ESP_OK) {
            write_err = err;
        }
    }
    return write_err;
}

esp_err_t solar_os_tui_box(solar_os_tui_t *tui,
                           size_t row,
                           size_t col,
                           size_t height,
                           size_t width,
                           uint8_t attr)
{
    if (height == 0 || width == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t rows = tui_rows(tui);
    const size_t cols = tui_cols(tui);
    const size_t clipped_height = row + height > rows ? rows - row : height;
    const size_t clipped_width = col + width > cols ? cols - col : width;

    if (clipped_height == 1) {
        return solar_os_tui_hline(tui, row, col, clipped_width, TUI_BOX_H, attr);
    }
    if (clipped_width == 1) {
        return solar_os_tui_vline(tui, row, col, clipped_height, TUI_BOX_V, attr);
    }

    solar_os_tui_putch(tui, row, col, TUI_BOX_TL, attr);
    solar_os_tui_putch(tui, row, col + clipped_width - 1, TUI_BOX_TR, attr);
    solar_os_tui_putch(tui, row + clipped_height - 1, col, TUI_BOX_BL, attr);
    solar_os_tui_putch(tui,
                       row + clipped_height - 1,
                       col + clipped_width - 1,
                       TUI_BOX_BR,
                       attr);
    solar_os_tui_hline(tui, row, col + 1, clipped_width - 2, TUI_BOX_H, attr);
    solar_os_tui_hline(tui,
                       row + clipped_height - 1,
                       col + 1,
                       clipped_width - 2,
                       TUI_BOX_H,
                       attr);
    solar_os_tui_vline(tui, row + 1, col, clipped_height - 2, TUI_BOX_V, attr);
    solar_os_tui_vline(tui,
                       row + 1,
                       col + clipped_width - 1,
                       clipped_height - 2,
                       TUI_BOX_V,
                       attr);
    return ESP_OK;
}

esp_err_t solar_os_tui_fill(solar_os_tui_t *tui,
                            size_t row,
                            size_t col,
                            size_t height,
                            size_t width,
                            uint32_t codepoint,
                            uint8_t attr)
{
    if (height == 0 || width == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t rows = tui_rows(tui);
    const size_t cols = tui_cols(tui);
    const size_t draw_height = row + height > rows ? rows - row : height;
    const size_t draw_width = col + width > cols ? cols - col : width;
    const uint32_t glyph = codepoint != 0 ? codepoint : ' ';

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    esp_err_t write_err = ESP_OK;
    for (size_t y = 0; y < draw_height; y++) {
        esp_err_t err = solar_os_tui_move(tui, row + y, col);
        for (size_t x = 0; x < draw_width; x++) {
            if (err == ESP_OK) {
                err = tui_write_codepoint(tui, glyph);
            }
        }
        if (write_err == ESP_OK) {
            write_err = err;
        }
    }
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return write_err;
}
