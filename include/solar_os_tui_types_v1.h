#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_TUI_ATTR_NORMAL = 0,
    SOLAR_OS_TUI_ATTR_BOLD = 1U << 0,
    SOLAR_OS_TUI_ATTR_INVERSE = 1U << 1,
    SOLAR_OS_TUI_ATTR_ITALIC = 1U << 2,
    SOLAR_OS_TUI_ATTR_UNDERLINE = 1U << 3,
} solar_os_tui_attr_t;

typedef struct {
    size_t row;
    size_t col;
    size_t height;
    size_t width;
} solar_os_tui_rect_t;

typedef struct {
    solar_os_tui_rect_t title;
    solar_os_tui_rect_t tabs;
    solar_os_tui_rect_t body;
    solar_os_tui_rect_t status;
    solar_os_tui_rect_t input;
    solar_os_tui_rect_t help;
} solar_os_tui_screen_layout_t;
typedef struct {
    size_t cursor;
    size_t top;
} solar_os_tui_viewport_t;
typedef struct {
    size_t cursor;
    size_t view;
} solar_os_tui_input_state_t;
typedef void (*solar_os_tui_form_row_fn)(size_t index, char *text, size_t capacity, void *user);
/* The form borrows its strings, viewport, input state and row callback only
 * for draw_form. A NULL edit_state selects browse mode. Draw surrounding
 * titles/help/status first. Row callbacks fill text and must not draw. */
typedef struct {
    solar_os_tui_rect_t bounds;
    solar_os_tui_viewport_t *viewport;
    size_t count;
    solar_os_tui_form_row_fn row;
    void *user;
    const char *empty_text;
    const char *edit_label;
    const char *edit_text;
    solar_os_tui_input_state_t *edit_state;
    bool masked;
} solar_os_tui_form_t;
typedef enum {
    SOLAR_OS_TUI_INPUT_NONE,
    SOLAR_OS_TUI_INPUT_CHANGED,
    SOLAR_OS_TUI_INPUT_SUBMIT,
    SOLAR_OS_TUI_INPUT_CANCEL,
} solar_os_tui_input_action_t;
typedef enum {
    SOLAR_OS_TUI_SCREEN_KEY_NONE,
    SOLAR_OS_TUI_SCREEN_KEY_CONSUMED,
    SOLAR_OS_TUI_SCREEN_KEY_PASSTHROUGH,
    SOLAR_OS_TUI_SCREEN_KEY_TOGGLED,
} solar_os_tui_screen_key_action_t;
