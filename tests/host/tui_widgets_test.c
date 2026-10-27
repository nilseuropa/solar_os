#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_keys.h"
#include "solar_os_tui_widgets.h"

#define TEST_ROWS 4U
#define TEST_COLS 32U

static uint32_t test_screen[TEST_ROWS][TEST_COLS];
static size_t test_cursor_row;
static size_t test_cursor_col;
static bool test_cursor_visible;
static size_t test_status_bar_calls;
static bool test_status_bar_visible;

esp_err_t solar_os_tui_enable_diff(solar_os_tui_t *tui, bool enabled)
{
    tui->diff_enabled = enabled;
    return ESP_OK;
}

esp_err_t solar_os_terminal_set_status_bar_visible_transient(
    solar_os_terminal_t *terminal,
    bool visible)
{
    (void)terminal;
    test_status_bar_calls++;
    test_status_bar_visible = visible;
    return ESP_OK;
}

size_t solar_os_tui_rows(const solar_os_tui_t *tui)
{
    (void)tui;
    return TEST_ROWS;
}

size_t solar_os_tui_cols(const solar_os_tui_t *tui)
{
    (void)tui;
    return TEST_COLS;
}

esp_err_t solar_os_tui_fill(solar_os_tui_t *tui,
                            size_t row,
                            size_t col,
                            size_t height,
                            size_t width,
                            uint32_t codepoint,
                            uint8_t attr)
{
    (void)tui;
    (void)attr;
    for (size_t y = row; y < row + height && y < TEST_ROWS; y++) {
        for (size_t x = col; x < col + width && x < TEST_COLS; x++) {
            test_screen[y][x] = codepoint;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_tui_putch(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             uint32_t codepoint,
                             uint8_t attr)
{
    (void)tui;
    (void)attr;
    assert(row < TEST_ROWS && col < TEST_COLS);
    test_screen[row][col] = codepoint;
    return ESP_OK;
}

esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{
    (void)tui;
    test_cursor_row = row;
    test_cursor_col = col;
    return ESP_OK;
}

esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{
    (void)tui;
    test_cursor_visible = visible;
    return ESP_OK;
}

static void test_viewport(void)
{
    solar_os_tui_viewport_t viewport = {.cursor = 0, .top = 0};
    assert(solar_os_tui_viewport_key(&viewport, SOLAR_OS_KEY_DOWN, 10, 3, false));
    assert(viewport.cursor == 1 && viewport.top == 0);
    assert(solar_os_tui_viewport_key(&viewport, SOLAR_OS_KEY_PAGE_DOWN, 10, 3, false));
    assert(viewport.cursor == 4 && viewport.top == 2);
    assert(solar_os_tui_viewport_key(&viewport, SOLAR_OS_KEY_END, 10, 3, false));
    assert(viewport.cursor == 9 && viewport.top == 7);
    assert(!solar_os_tui_viewport_key(&viewport, SOLAR_OS_KEY_DOWN, 10, 3, false));
    assert(solar_os_tui_viewport_key(&viewport, SOLAR_OS_KEY_DOWN, 10, 3, true));
    assert(viewport.cursor == 0 && viewport.top == 0);
}

static void test_layout(void)
{
    solar_os_tui_screen_layout_t layout;
    assert(solar_os_tui_layout_compute(20, 40, 1, 1, 1, &layout));
    assert(layout.title.row == 0 && layout.title.width == 40);
    assert(layout.tabs.row == 1 && layout.tabs.height == 1);
    assert(layout.body.row == 2 && layout.body.height == 15);
    assert(layout.status.row == 17 && layout.input.row == 18 && layout.help.row == 19);
    assert(!solar_os_tui_layout_compute(5, 40, 1, 1, 1, &layout));

    assert(solar_os_tui_layout_compute_fullscreen(10, 40, 1, 1, &layout));
    assert(layout.title.row == 0 && layout.tabs.row == 1);
    assert(layout.body.row == 2 && layout.body.height == 7);
    assert(layout.status.height == 0 && layout.help.height == 0);
    assert(layout.input.row == 9 && layout.input.height == 1);
}

static void test_fullscreen_key(void)
{
    solar_os_tui_t tui = {
        .terminal = (solar_os_terminal_t *)(uintptr_t)1U,
        .screen_active = true,
        .saved_status_bar_visible = true,
    };
    assert(!solar_os_tui_screen_should_fullscreen(0));
    assert(solar_os_tui_screen_should_fullscreen(10));
    assert(!solar_os_tui_screen_should_fullscreen(11));
    assert(!solar_os_tui_screen_fullscreen(&tui));
    assert(solar_os_tui_screen_bottom_rows(&tui, 2) == 2);
    assert(solar_os_tui_screen_content_rows(&tui, 1, 2) == 1);
    assert(solar_os_tui_screen_content_end(&tui, 2) == 2);
    assert(solar_os_tui_screen_key(&tui, SOLAR_OS_KEY_ALT_PREFIX) ==
           SOLAR_OS_TUI_SCREEN_KEY_CONSUMED);
    assert(solar_os_tui_screen_key(&tui, 'x') ==
           SOLAR_OS_TUI_SCREEN_KEY_PASSTHROUGH);
    assert(!solar_os_tui_screen_fullscreen(&tui));
    assert(solar_os_tui_screen_key(&tui, SOLAR_OS_KEY_ALT_PREFIX) ==
           SOLAR_OS_TUI_SCREEN_KEY_CONSUMED);
    assert(solar_os_tui_screen_key(&tui, SOLAR_OS_KEY_ENTER) ==
           SOLAR_OS_TUI_SCREEN_KEY_TOGGLED);
    assert(solar_os_tui_screen_fullscreen(&tui));
    assert(test_status_bar_calls == 1U && !test_status_bar_visible);
    assert(solar_os_tui_screen_bottom_rows(&tui, 2) == 0);
    assert(solar_os_tui_screen_content_rows(&tui, 1, 2) == 3);
    assert(solar_os_tui_screen_content_end(&tui, 2) == 4);

    memset(test_screen, 0, sizeof(test_screen));
    assert(solar_os_tui_draw_footer(&tui, "saved", "help") == ESP_OK);
    assert(test_screen[TEST_ROWS - 1U][0] == 's');
    memset(test_screen, 0, sizeof(test_screen));
    assert(solar_os_tui_draw_footer(&tui, "", "help") == ESP_OK);
    assert(test_screen[TEST_ROWS - 1U][0] == 0U);

    assert(solar_os_tui_screen_key(&tui, SOLAR_OS_KEY_ALT_PREFIX) ==
           SOLAR_OS_TUI_SCREEN_KEY_CONSUMED);
    assert(solar_os_tui_screen_key(&tui, SOLAR_OS_KEY_ENTER) ==
           SOLAR_OS_TUI_SCREEN_KEY_TOGGLED);
    assert(!solar_os_tui_screen_fullscreen(&tui));
    assert(test_status_bar_calls == 2U && test_status_bar_visible);
}

static void test_input(void)
{
    char text[32] = "a\xc3\xa4" "b";
    solar_os_tui_input_state_t state = {.cursor = strlen(text), .view = 0};
    assert(solar_os_tui_input_key(text, sizeof(text), &state,
                                  SOLAR_OS_KEY_LEFT, 2) == SOLAR_OS_TUI_INPUT_NONE);
    assert(state.cursor == 3);
    assert(solar_os_tui_input_key(text, sizeof(text), &state, '\b', 2) ==
           SOLAR_OS_TUI_INPUT_CHANGED);
    assert(strcmp(text, "ab") == 0 && state.cursor == 1);
    assert(solar_os_tui_input_key(text, sizeof(text), &state, 'X', 2) ==
           SOLAR_OS_TUI_INPUT_CHANGED);
    assert(strcmp(text, "aXb") == 0 && state.cursor == 2);
    strcpy(text, "one two");
    state.cursor = strlen(text);
    state.view = 0;
    assert(solar_os_tui_input_key(text, sizeof(text), &state,
                                  SOLAR_OS_KEY_CTRL_LEFT, 8) == SOLAR_OS_TUI_INPUT_NONE);
    assert(state.cursor == 4);
    assert(solar_os_tui_input_key(text, sizeof(text), &state,
                                  SOLAR_OS_KEY_CTRL_RIGHT, 8) == SOLAR_OS_TUI_INPUT_NONE);
    assert(state.cursor == strlen(text));
    text[0] = (char)0xc3;
    text[1] = '\0';
    state.cursor = 0;
    assert(solar_os_tui_input_key(text, sizeof(text), &state,
                                  SOLAR_OS_KEY_DELETE, 2) ==
           SOLAR_OS_TUI_INPUT_CHANGED);
    assert(text[0] == '\0');
    assert(solar_os_tui_input_key(text, sizeof(text), &state,
                                  SOLAR_OS_KEY_ENTER, 2) == SOLAR_OS_TUI_INPUT_SUBMIT);
}

static void test_masked_input_draw(void)
{
    memset(test_screen, 0, sizeof(test_screen));
    solar_os_tui_t tui = {0};
    const char text[] = "p\xc3\xa4ss";
    solar_os_tui_input_state_t state = {
        .cursor = strlen(text),
        .view = 0,
    };
    assert(solar_os_tui_draw_input_ex(&tui,
                                      1,
                                      0,
                                      10,
                                      "P:",
                                      text,
                                      &state,
                                      SOLAR_OS_TUI_ATTR_NORMAL,
                                      true) == ESP_OK);
    assert(test_screen[1][0] == 'P' && test_screen[1][1] == ':');
    for (size_t col = 2; col < 6; col++) {
        assert(test_screen[1][col] == '*');
    }
    assert(test_screen[1][6] == ' ');
    assert(test_cursor_row == 1 && test_cursor_col == 6);
    assert(test_cursor_visible);
}

static void form_row(size_t index, char *text, size_t capacity, void *user)
{
    assert(user == test_screen);
    snprintf(text, capacity, "Field%zu: value", index);
}

static void test_inline_form(void)
{
    solar_os_tui_t tui = {0};
    solar_os_tui_viewport_t viewport = {.cursor = 3};
    const char value[] = "https://example.org/long-path";
    solar_os_tui_input_state_t input = {.cursor = strlen(value)};
    solar_os_tui_form_t form = {
        .bounds = {.row = 0, .col = 2, .height = 2, .width = 12},
        .viewport = &viewport, .count = 5, .row = form_row, .user = test_screen,
        .empty_text = "Empty", .edit_label = "URL: ", .edit_text = value, .edit_state = &input,
    };
    /* Surrounding UI is drawn first; the form owns the final cursor position. */
    assert(solar_os_tui_write_cell(&tui, 3, 0, TEST_COLS, "Enter apply  Esc cancel", 0) == ESP_OK);
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_OK);
    assert(viewport.top == 2 && test_cursor_visible);
    assert(test_cursor_row == 1 && test_cursor_col == 13 && input.view > 0);
    assert(test_screen[0][2] == 'F' && test_screen[0][7] == '2');
    assert(test_screen[1][2] == 'U' && test_screen[1][5] == ':');
    assert(test_screen[3][0] == 'E');
    input.cursor = 0;
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_OK);
    assert(test_cursor_row == 1 && test_cursor_col == 7 && input.view == 0);
    form.edit_label = "Token: "; form.edit_text = "p\xc3\xa4ss";
    form.masked = true; input.cursor = strlen(form.edit_text);
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_OK);
    for (size_t col = 9; col < 13; col++) assert(test_screen[1][col] == '*');
    assert(test_cursor_visible && test_cursor_col == 13);
    form.bounds.height = 1;
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_OK && viewport.top == 3 && test_cursor_row == 0);
    form.edit_state = NULL;
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_OK && !test_cursor_visible);
    form.count = 0;
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_OK && viewport.cursor == 0 && !test_cursor_visible);
    assert(test_screen[0][2] == 'E');
    form.bounds.width = TEST_COLS;
    assert(solar_os_tui_draw_form(&tui, &form) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_viewport();
    test_layout();
    test_fullscreen_key();
    test_input();
    test_masked_input_draw();
    test_inline_form();
    puts("tui_widgets_test: ok");
    return 0;
}
