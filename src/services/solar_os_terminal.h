#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"

#define SOLAR_OS_TERMINAL_MAX_COLS 96
#define SOLAR_OS_TERMINAL_MAX_ROWS 64

typedef enum {
    SOLAR_OS_TERMINAL_FONT_MONO,
    SOLAR_OS_TERMINAL_FONT_COMPACT,
} solar_os_terminal_font_t;

typedef enum {
    SOLAR_OS_TERMINAL_TEXT_SIZE_14 = 0,
    SOLAR_OS_TERMINAL_TEXT_SIZE_20 = 1,
    SOLAR_OS_TERMINAL_TEXT_SIZE_12 = 2,
    SOLAR_OS_TERMINAL_TEXT_SIZE_16 = 3,
    SOLAR_OS_TERMINAL_TEXT_SIZE_18 = 4,
    SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT,
} solar_os_terminal_text_size_t;

typedef struct {
    uint16_t inbox_unread;
    bool battery_valid;
    uint8_t battery_percent;
    bool battery_external_power;
    bool ble_connected;
    bool ble_scanning;
    bool wifi_started;
    bool wifi_connected;
    bool wifi_has_ip;
    uint8_t wifi_level;
    bool audio_enabled;
    uint8_t audio_volume;
    bool time_valid;
    uint8_t hour;
    uint8_t minute;
    bool sd_mounted;
} solar_os_status_bar_t;

void solar_os_terminal_clear(solar_os_terminal_t *terminal);
void solar_os_terminal_newline(solar_os_terminal_t *terminal);
void solar_os_terminal_backspace(solar_os_terminal_t *terminal);
void solar_os_terminal_put_printable(solar_os_terminal_t *terminal, char ch);
void solar_os_terminal_put_codepoint(solar_os_terminal_t *terminal, uint32_t codepoint);
void solar_os_terminal_put_char(solar_os_terminal_t *terminal, char ch);
void solar_os_terminal_put_utf8_byte(solar_os_terminal_t *terminal, uint8_t byte);
void solar_os_terminal_write_utf8(solar_os_terminal_t *terminal, const char *text);
void solar_os_terminal_utf8_reset(solar_os_terminal_t *terminal);
void solar_os_terminal_write(solar_os_terminal_t *terminal, const char *text);
void solar_os_terminal_writeln(solar_os_terminal_t *terminal, const char *text);
void solar_os_terminal_printf(solar_os_terminal_t *terminal, const char *fmt, ...);
void solar_os_terminal_set_bold(solar_os_terminal_t *terminal, bool enabled);
bool solar_os_terminal_bold(const solar_os_terminal_t *terminal);
void solar_os_terminal_set_italic(solar_os_terminal_t *terminal, bool enabled);
bool solar_os_terminal_italic(const solar_os_terminal_t *terminal);
void solar_os_terminal_set_underline(solar_os_terminal_t *terminal, bool enabled);
bool solar_os_terminal_underline(const solar_os_terminal_t *terminal);
void solar_os_terminal_set_inverse(solar_os_terminal_t *terminal, bool enabled);
bool solar_os_terminal_inverse(const solar_os_terminal_t *terminal);
void solar_os_terminal_write_bold(solar_os_terminal_t *terminal, const char *text);
void solar_os_terminal_writeln_bold(solar_os_terminal_t *terminal, const char *text);
void solar_os_terminal_printf_bold(solar_os_terminal_t *terminal, const char *fmt, ...);
void solar_os_terminal_set_status_bar(solar_os_terminal_t *terminal,
                                      const solar_os_status_bar_t *status);
void solar_os_terminal_get_status_bar(const solar_os_terminal_t *terminal,
                                      solar_os_status_bar_t *status);
void solar_os_terminal_set_black_is_one(solar_os_terminal_t *terminal, bool black_is_one);
size_t solar_os_terminal_cursor_row(const solar_os_terminal_t *terminal);
size_t solar_os_terminal_cursor_col(const solar_os_terminal_t *terminal);
size_t solar_os_terminal_rows(const solar_os_terminal_t *terminal);
size_t solar_os_terminal_cols(const solar_os_terminal_t *terminal);
void solar_os_terminal_set_cursor(solar_os_terminal_t *terminal, size_t row, size_t col);
void solar_os_terminal_set_cursor_visible(solar_os_terminal_t *terminal, bool visible);
bool solar_os_terminal_cursor_visible(const solar_os_terminal_t *terminal);
void solar_os_terminal_clear_primitives(solar_os_terminal_t *terminal);
esp_err_t solar_os_terminal_add_vrule(solar_os_terminal_t *terminal,
                                      size_t row,
                                      size_t col,
                                      size_t height,
                                      uint8_t width,
                                      bool inverse);
void solar_os_terminal_clear_line_from(solar_os_terminal_t *terminal, size_t row, size_t col);
void solar_os_terminal_page_up(solar_os_terminal_t *terminal);
void solar_os_terminal_page_down(solar_os_terminal_t *terminal);
void solar_os_terminal_scroll_to_live(solar_os_terminal_t *terminal);
bool solar_os_terminal_is_scrolled_back(const solar_os_terminal_t *terminal);
uint16_t solar_os_terminal_orientation(const solar_os_terminal_t *terminal);
esp_err_t solar_os_terminal_set_orientation(solar_os_terminal_t *terminal, uint16_t degrees);
solar_os_terminal_font_t solar_os_terminal_font(const solar_os_terminal_t *terminal);
esp_err_t solar_os_terminal_set_font(solar_os_terminal_t *terminal, solar_os_terminal_font_t font);
const char *solar_os_terminal_font_name(solar_os_terminal_font_t font);
bool solar_os_terminal_parse_font(const char *name, solar_os_terminal_font_t *font);
solar_os_terminal_text_size_t solar_os_terminal_text_size(const solar_os_terminal_t *terminal);
esp_err_t solar_os_terminal_set_text_size(solar_os_terminal_t *terminal,
                                          solar_os_terminal_text_size_t text_size);
esp_err_t solar_os_terminal_set_text_size_transient(solar_os_terminal_t *terminal,
                                                    solar_os_terminal_text_size_t text_size);
const char *solar_os_terminal_text_size_name(solar_os_terminal_text_size_t text_size);
bool solar_os_terminal_parse_text_size(const char *name, solar_os_terminal_text_size_t *text_size);
bool solar_os_terminal_needs_draw(const solar_os_terminal_t *terminal);
void solar_os_terminal_draw(solar_os_terminal_t *terminal);
