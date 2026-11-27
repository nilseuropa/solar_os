#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_terminal_types_v1.h"

#define SOLAR_OS_NATIVE_TERMINAL_SERVICE "terminal"
#define SOLAR_OS_NATIVE_TERMINAL_ABI 1U

#define SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_ORIENTATION 0U
#define SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_SCALE 1U
#define SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_FONT 2U
#define SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_TEXT_SIZE 3U
#define SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_PALETTE 4U
#define SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_STATUS_BAR 5U

typedef struct {
    uint16_t orientation_degrees;
    uint8_t scale;
} solar_os_native_terminal_display_preferences_v1_t;

typedef struct {
    size_t rows;
    int grid_top;
    int baseline_offset;
    int content_bottom;
} solar_os_terminal_geometry_t;

/* Borrow application.terminal during its owning callback. Serialize access;
 * do not retain the reference beyond its owner or access it from another task.
 * The core owns storage, rendering and scrollback. Text is consumed before
 * return. Persistent preference setters affect local terminals; transient
 * setters affect only the referenced terminal. No drawing buffer is copied
 * by this table. Status and profile records are caller-owned values. */
typedef struct {
    uint32_t abi_version, struct_size;
    void (*clear)(solar_os_terminal_t *terminal);
    void (*newline)(solar_os_terminal_t *terminal);
    void (*backspace)(solar_os_terminal_t *terminal);
    void (*put_printable)(solar_os_terminal_t *terminal, char ch);
    void (*put_codepoint)(solar_os_terminal_t *terminal, uint32_t codepoint);
    void (*put_char)(solar_os_terminal_t *terminal, char ch);
    void (*put_utf8_byte)(solar_os_terminal_t *terminal, uint8_t byte);
    void (*write_utf8)(solar_os_terminal_t *terminal, const char *text);
    void (*utf8_reset)(solar_os_terminal_t *terminal);
    void (*write)(solar_os_terminal_t *terminal, const char *text);
    void (*writeln)(solar_os_terminal_t *terminal, const char *text);
    void (*printf)(solar_os_terminal_t *terminal, const char *fmt, ...);
    void (*set_bold)(solar_os_terminal_t *terminal, bool enabled);
    bool (*bold)(const solar_os_terminal_t *terminal);
    void (*set_italic)(solar_os_terminal_t *terminal, bool enabled);
    bool (*italic)(const solar_os_terminal_t *terminal);
    void (*set_underline)(solar_os_terminal_t *terminal, bool enabled);
    bool (*underline)(const solar_os_terminal_t *terminal);
    void (*set_inverse)(solar_os_terminal_t *terminal, bool enabled);
    bool (*inverse)(const solar_os_terminal_t *terminal);
    void (*write_bold)(solar_os_terminal_t *terminal, const char *text);
    void (*writeln_bold)(solar_os_terminal_t *terminal, const char *text);
    void (*printf_bold)(solar_os_terminal_t *terminal, const char *fmt, ...);
    void (*set_status_bar)(solar_os_terminal_t *terminal, const solar_os_status_bar_t *status);
    void (*get_status_bar)(const solar_os_terminal_t *terminal, solar_os_status_bar_t *status);
    bool (*status_bar_preference_visible)(void);
    int (*set_status_bar_preference)(bool visible);
    bool (*status_bar_visible)(const solar_os_terminal_t *terminal);
    int (*set_status_bar_visible)(solar_os_terminal_t *terminal, bool visible);
    int (*set_status_bar_visible_transient)(solar_os_terminal_t *terminal, bool visible);
    void (*set_footer)(solar_os_terminal_t *terminal, const char *text);
    void (*set_black_is_one)(solar_os_terminal_t *terminal, bool black_is_one);
    bool (*palette_preference_inverted)(void);
    int (*set_palette_preference)(bool inverted);
    bool (*palette_inverted)(const solar_os_terminal_t *terminal);
    int (*set_palette_inverted)(solar_os_terminal_t *terminal, bool inverted);
    int (*set_palette_inverted_transient)(solar_os_terminal_t *terminal, bool inverted);
    size_t (*cursor_row)(const solar_os_terminal_t *terminal);
    size_t (*cursor_col)(const solar_os_terminal_t *terminal);
    size_t (*rows)(const solar_os_terminal_t *terminal);
    size_t (*cols)(const solar_os_terminal_t *terminal);
    void (*set_cursor)(solar_os_terminal_t *terminal, size_t row, size_t col);
    void (*set_cursor_visible)(solar_os_terminal_t *terminal, bool visible);
    bool (*cursor_visible)(const solar_os_terminal_t *terminal);
    void (*get_cursor_preference)(solar_os_terminal_cursor_shape_t *shape, bool *blink);
    int (*set_cursor_preference)(solar_os_terminal_cursor_shape_t shape, bool blink);
    const char * (*cursor_shape_name)(solar_os_terminal_cursor_shape_t shape);
    bool (*parse_cursor_shape)(const char *name, solar_os_terminal_cursor_shape_t *shape);
    void (*clear_primitives)(solar_os_terminal_t *terminal);
    int (*add_vrule)(solar_os_terminal_t *terminal, size_t row, size_t col, size_t height, uint8_t width, bool inverse);
    void (*clear_line_from)(solar_os_terminal_t *terminal, size_t row, size_t col);
    void (*page_up)(solar_os_terminal_t *terminal);
    void (*page_down)(solar_os_terminal_t *terminal);
    void (*scroll_to_live)(solar_os_terminal_t *terminal);
    bool (*is_scrolled_back)(const solar_os_terminal_t *terminal);
    uint16_t (*orientation)(const solar_os_terminal_t *terminal);
    int (*set_orientation)(solar_os_terminal_t *terminal, uint16_t degrees);
    int (*set_orientation_transient)(solar_os_terminal_t *terminal, uint16_t degrees);
    solar_os_terminal_font_t (*font)(const solar_os_terminal_t *terminal);
    int (*set_font)(solar_os_terminal_t *terminal, solar_os_terminal_font_t font);
    int (*set_font_transient)(solar_os_terminal_t *terminal, solar_os_terminal_font_t font);
    const char * (*font_name)(solar_os_terminal_font_t font);
    bool (*parse_font)(const char *name, solar_os_terminal_font_t *font);
    solar_os_terminal_text_size_t (*text_size)(const solar_os_terminal_t *terminal);
    int (*set_text_size)(solar_os_terminal_t *terminal, solar_os_terminal_text_size_t text_size);
    int (*set_text_size_transient)(solar_os_terminal_t *terminal, solar_os_terminal_text_size_t text_size);
    const char * (*text_size_name)(solar_os_terminal_text_size_t text_size);
    bool (*parse_text_size)(const char *name, solar_os_terminal_text_size_t *text_size);
    void (*get_profile)(const solar_os_terminal_t *terminal, solar_os_terminal_profile_t *profile);
    int (*apply_profile_transient)(solar_os_terminal_t *terminal, const solar_os_terminal_profile_t *profile);
    bool (*needs_draw)(const solar_os_terminal_t *terminal);
    void (*invalidate_render)(solar_os_terminal_t *terminal);
    void (*draw)(solar_os_terminal_t *terminal);
    size_t (*columns_for_width)(int display_width, int horizontal_margin, int cell_width, size_t max_cols);
    bool (*geometry_for_height)(int display_height, int status_bar_height, int footer_height,
        int line_height, int cell_ascent, size_t max_rows, solar_os_terminal_geometry_t *geometry);
    bool (*profile_valid)(const solar_os_terminal_profile_t *profile);
    void (*load_saved_profile)(solar_os_terminal_profile_t *profile);
    int (*load_display_preferences)(const char *display, uint16_t orientation_default,
                                    solar_os_native_terminal_display_preferences_v1_t *preferences);
    /* NULL display selects global defaults. Named displays accept orientation
     * and scale. The palette and status-bar values are booleans (0 or 1). */
    int (*save_preference)(const char *display, uint32_t field, uint32_t value);
    /* The saved cursor choice takes precedence over the panel default. */
    void (*cursor_preference_for_panel)(bool epaper, solar_os_terminal_cursor_shape_t *shape, bool *blink);
} solar_os_native_terminal_api_v1_t;
