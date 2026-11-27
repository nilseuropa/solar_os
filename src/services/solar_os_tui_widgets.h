#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_tui.h"

/* A scrollable form with an optional in-place editor on the selected row.
 * Draw surrounding titles/help/status first, then the form and refresh. The
 * widget draws the active editor last and owns cursor placement/visibility.
 * A NULL edit_state selects browse mode. Row callbacks must not draw. */

esp_err_t solar_os_tui_draw_form(solar_os_tui_t *tui, const solar_os_tui_form_t *form);

esp_err_t solar_os_tui_screen_begin(solar_os_tui_t *tui, solar_os_context_t *ctx);
bool solar_os_tui_screen_should_fullscreen(size_t rows);
bool solar_os_tui_screen_fullscreen(const solar_os_tui_t *tui);
size_t solar_os_tui_screen_bottom_rows(const solar_os_tui_t *tui,
                                       size_t normal_rows);
size_t solar_os_tui_screen_content_rows(const solar_os_tui_t *tui,
                                        size_t top_rows,
                                        size_t normal_bottom_rows);
size_t solar_os_tui_screen_content_end(const solar_os_tui_t *tui,
                                       size_t normal_bottom_rows);
solar_os_tui_screen_key_action_t solar_os_tui_screen_key(solar_os_tui_t *tui,
                                                         uint8_t key);
bool solar_os_tui_screen_layout(const solar_os_tui_t *tui,
                                size_t tab_rows,
                                size_t status_rows,
                                size_t input_rows,
                                solar_os_tui_screen_layout_t *layout);
bool solar_os_tui_layout_compute(size_t rows,
                                 size_t cols,
                                 size_t tab_rows,
                                 size_t status_rows,
                                 size_t input_rows,
                                 solar_os_tui_screen_layout_t *layout);
bool solar_os_tui_layout_compute_fullscreen(size_t rows,
                                            size_t cols,
                                            size_t tab_rows,
                                            size_t input_rows,
                                            solar_os_tui_screen_layout_t *layout);
esp_err_t solar_os_tui_write_cell(solar_os_tui_t *tui,
                                  size_t row,
                                  size_t col,
                                  size_t width,
                                  const char *text,
                                  uint8_t attr);
esp_err_t solar_os_tui_draw_title(solar_os_tui_t *tui,
                                  const char *title,
                                  const char *detail);
esp_err_t solar_os_tui_draw_help(solar_os_tui_t *tui, const char *text);
esp_err_t solar_os_tui_draw_footer(solar_os_tui_t *tui,
                                   const char *status,
                                   const char *help);
esp_err_t solar_os_tui_draw_tab(solar_os_tui_t *tui,
                                size_t row,
                                size_t col,
                                size_t width,
                                const char *text,
                                bool selected);
esp_err_t solar_os_tui_draw_too_small(solar_os_tui_t *tui, const char *name);

void solar_os_tui_viewport_reconcile(solar_os_tui_viewport_t *viewport,
                                     size_t item_count,
                                     size_t visible_rows);
bool solar_os_tui_viewport_key(solar_os_tui_viewport_t *viewport,
                               uint8_t key,
                               size_t item_count,
                               size_t visible_rows,
                               bool wrap);

solar_os_tui_input_action_t solar_os_tui_input_key(
    char *text,
    size_t capacity,
    solar_os_tui_input_state_t *state,
    uint32_t key,
    size_t visible_cells);
solar_os_tui_input_action_t solar_os_tui_input_text(char *text, size_t capacity,
    solar_os_tui_input_state_t *state, const char *input, size_t length, size_t visible_cells);
esp_err_t solar_os_tui_draw_input(solar_os_tui_t *tui,
                                  size_t row,
                                  size_t col,
                                  size_t width,
                                  const char *label,
                                  const char *text,
                                  solar_os_tui_input_state_t *state,
                                  uint8_t attr);
esp_err_t solar_os_tui_draw_input_ex(solar_os_tui_t *tui,
                                     size_t row,
                                     size_t col,
                                     size_t width,
                                     const char *label,
                                     const char *text,
                                     solar_os_tui_input_state_t *state,
                                     uint8_t attr,
                                     bool masked);
