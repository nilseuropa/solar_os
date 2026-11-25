#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_shell_io.h"

#include "solar_os_tui_types_v1.h"

typedef struct solar_os_tui {
    solar_os_shell_io_t *io;
    solar_os_shell_io_t fallback_io;
    solar_os_terminal_t *terminal;
    bool screen_active;
    bool fullscreen;
    bool alt_prefix_pending;
    bool saved_status_bar_visible;
    bool status_bar_overridden;
    bool diff_enabled;
    bool diff_ready;
    uint16_t diff_cols;
    uint16_t diff_rows;
    size_t draw_row;
    size_t draw_col;
    size_t cursor_row;
    size_t cursor_col;
    uint8_t draw_attr;
    uint8_t saved_attr;
    bool saved_cursor_visible;
    bool cursor_visible;
    uint32_t screen_generation;
    uint32_t *front_codepoints;
    uint32_t *back_codepoints;
    uint8_t *front_attrs;
    uint8_t *back_attrs;
} solar_os_tui_t;

esp_err_t solar_os_tui_begin(solar_os_tui_t *tui, solar_os_context_t *ctx);
void solar_os_tui_attach_session(solar_os_tui_t *tui);
void solar_os_tui_end(solar_os_tui_t *tui);
esp_err_t solar_os_tui_enable_diff(solar_os_tui_t *tui, bool enabled);
size_t solar_os_tui_rows(const solar_os_tui_t *tui);
size_t solar_os_tui_cols(const solar_os_tui_t *tui);
void solar_os_tui_clear(solar_os_tui_t *tui);
void solar_os_tui_refresh(solar_os_tui_t *tui);
esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col);
esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible);
esp_err_t solar_os_tui_write(solar_os_tui_t *tui, const char *text, uint8_t attr);
esp_err_t solar_os_tui_addstr(solar_os_tui_t *tui,
                              size_t row,
                              size_t col,
                              const char *text,
                              uint8_t attr);
esp_err_t solar_os_tui_putch(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             uint32_t codepoint,
                             uint8_t attr);
esp_err_t solar_os_tui_hline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t width,
                             uint32_t codepoint,
                             uint8_t attr);
esp_err_t solar_os_tui_vline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint32_t codepoint,
                             uint8_t attr);
esp_err_t solar_os_tui_vrule(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint8_t width,
                             uint8_t attr);
esp_err_t solar_os_tui_box(solar_os_tui_t *tui,
                           size_t row,
                           size_t col,
                           size_t height,
                           size_t width,
                           uint8_t attr);
esp_err_t solar_os_tui_fill(solar_os_tui_t *tui,
                            size_t row,
                            size_t col,
                            size_t height,
                            size_t width,
                            uint32_t codepoint,
                            uint8_t attr);
esp_err_t solar_os_tui_text_popup(solar_os_tui_t *tui,
                                  const solar_os_tui_rect_t *bounds,
                                  const char *title,
                                  const char *text,
                                  solar_os_tui_rect_t *popup);
esp_err_t solar_os_tui_progress_bar(solar_os_tui_t *tui,
                                    size_t row,
                                    size_t col,
                                    size_t width,
                                    const char *label,
                                    uint64_t value,
                                    uint64_t total,
                                    bool total_known);
