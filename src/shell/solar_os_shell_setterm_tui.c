#include "solar_os_shell_tui_apps.h"
#include "solar_os_shell_common.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_board_caps.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble_keyboard.h"
#endif
#include "solar_os_display.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_power.h"
#if SOLAR_OS_PACKAGE_SERVICE_OTA
#include "solar_os_ota.h"
#endif
#include "solar_os_sessions.h"
#include "solar_os_shell.h"
#include "solar_os_terminal.h"
#include "solar_os_time.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define SETTERM_TUI_EDIT_MAX 256
#define SETTERM_TUI_CURSOR_BLINK_MS 500

static solar_os_terminal_t *display_terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_display_terminal(ctx);
}

static bool parse_size_arg(const char *text, size_t min, size_t max, size_t *value)
{
    return solar_os_shell_parse_size_arg(text, min, max, value);
}

typedef enum {
    SETTERM_TUI_ORIENTATION,
    SETTERM_TUI_FONT,
    SETTERM_TUI_TEXTSIZE,
    SETTERM_TUI_PALETTE,
    SETTERM_TUI_FOREGROUND,
    SETTERM_TUI_BACKGROUND,
    SETTERM_TUI_STATUSBAR,
    SETTERM_TUI_BRIGHTNESS,
    SETTERM_TUI_KEYBOARD,
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    SETTERM_TUI_BLE,
#endif
    SETTERM_TUI_POWERKEY,
    SETTERM_TUI_KEYRATE,
    SETTERM_TUI_TIMEZONE,
    SETTERM_TUI_STARTUP,
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    SETTERM_TUI_OTAURL,
#endif
    SETTERM_TUI_ITEM_COUNT,
} setterm_tui_item_t;

typedef struct {
    const char *label;
} setterm_tui_item_def_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected;
    size_t first_visible;
    bool editing;
    bool cursor_visible;
    uint32_t last_cursor_blink_ms;
    char edit_text[SETTERM_TUI_EDIT_MAX];
    char original_text[SETTERM_TUI_EDIT_MAX];
    char status[64];
} setterm_tui_state_t;

static void *setterm_tui_state;
#define setterm_tui (*(setterm_tui_state_t *)setterm_tui_state)

static const setterm_tui_item_def_t setterm_tui_items[] = {
    [SETTERM_TUI_ORIENTATION] = {.label = "orientation"},
    [SETTERM_TUI_FONT] = {.label = "font"},
    [SETTERM_TUI_TEXTSIZE] = {.label = "textsize"},
    [SETTERM_TUI_PALETTE] = {.label = "palette"},
    [SETTERM_TUI_FOREGROUND] = {.label = "foreground"},
    [SETTERM_TUI_BACKGROUND] = {.label = "background"},
    [SETTERM_TUI_STATUSBAR] = {.label = "statusbar"},
    [SETTERM_TUI_BRIGHTNESS] = {.label = "brightness"},
    [SETTERM_TUI_KEYBOARD] = {.label = "keyboard"},
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    [SETTERM_TUI_BLE] = {.label = "ble"},
#endif
    [SETTERM_TUI_POWERKEY] = {.label = "powerkey"},
    [SETTERM_TUI_KEYRATE] = {.label = "keyrate"},
    [SETTERM_TUI_TIMEZONE] = {.label = "timezone"},
    [SETTERM_TUI_STARTUP] = {.label = "startup"},
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    [SETTERM_TUI_OTAURL] = {.label = "otaurl"},
#endif
};

static size_t setterm_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void setterm_tui_current_value(setterm_tui_item_t item, char *buffer, size_t buffer_len)
{
    solar_os_terminal_t *term = display_terminal(setterm_tui.ctx);

    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    switch (item) {
    case SETTERM_TUI_ORIENTATION:
        snprintf(buffer, buffer_len, "%u", (unsigned)solar_os_terminal_orientation(term));
        break;
    case SETTERM_TUI_FONT:
        strlcpy(buffer,
                solar_os_terminal_font_name(solar_os_terminal_font(term)),
                buffer_len);
        break;
    case SETTERM_TUI_TEXTSIZE:
        strlcpy(buffer,
                solar_os_terminal_text_size_name(solar_os_terminal_text_size(term)),
                buffer_len);
        break;
    case SETTERM_TUI_PALETTE:
        strlcpy(buffer,
                solar_os_terminal_palette_inverted(term) ? "inverted" : "normal",
                buffer_len);
        break;
    case SETTERM_TUI_FOREGROUND:
    case SETTERM_TUI_BACKGROUND: {
        uint32_t foreground_rgb888 = 0;
        uint32_t background_rgb888 = 0;
        (void)solar_os_display_get_colors(&foreground_rgb888, &background_rgb888);
        const uint32_t color = item == SETTERM_TUI_FOREGROUND ?
            foreground_rgb888 : background_rgb888;
        snprintf(buffer, buffer_len, "#%06" PRIx32, color);
        break;
    }
    case SETTERM_TUI_STATUSBAR:
        strlcpy(buffer,
                solar_os_terminal_status_bar_visible(term) ? "show" : "hide",
                buffer_len);
        break;
    case SETTERM_TUI_BRIGHTNESS: {
        uint8_t percent = 0;
        const esp_err_t err = solar_os_display_get_brightness(&percent);
        if (err == ESP_OK) {
            snprintf(buffer, buffer_len, "%u", (unsigned)percent);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            strlcpy(buffer, "unsupported", buffer_len);
        } else {
            strlcpy(buffer, "unavailable", buffer_len);
        }
        break;
    }
    case SETTERM_TUI_KEYBOARD:
        strlcpy(buffer,
                solar_os_input_keyboard_layout_name(solar_os_input_keyboard_layout()),
                buffer_len);
        break;
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    case SETTERM_TUI_BLE:
        strlcpy(buffer,
                solar_os_ble_keyboard_boot_setting_name(
                    solar_os_ble_keyboard_boot_setting()),
                buffer_len);
        break;
#endif
    case SETTERM_TUI_POWERKEY: {
        solar_os_power_status_t status;
        solar_os_power_get_status(&status);
        strlcpy(buffer,
                solar_os_power_key_action_name(status.key_action),
                buffer_len);
        break;
    }
    case SETTERM_TUI_KEYRATE: {
        uint16_t rate = 0;
        uint16_t delay_ms = 0;
        solar_os_input_get_repeat(&rate, &delay_ms);
        if (rate == 0) {
            strlcpy(buffer, "off", buffer_len);
        } else {
            snprintf(buffer, buffer_len, "%u %u", (unsigned)rate, (unsigned)delay_ms);
        }
        break;
    }
    case SETTERM_TUI_TIMEZONE:
        solar_os_time_get_timezone(buffer, buffer_len, NULL, 0);
        break;
    case SETTERM_TUI_STARTUP:
        strlcpy(buffer,
                solar_os_shell_startup_source_name(solar_os_shell_startup_source()),
                buffer_len);
        break;
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    case SETTERM_TUI_OTAURL:
        solar_os_ota_get_url(buffer, buffer_len);
        break;
#endif
    default:
        strlcpy(buffer, "-", buffer_len);
        break;
    }
}

static void setterm_tui_set_status(const char *status)
{
    strlcpy(setterm_tui.status, status != NULL ? status : "", sizeof(setterm_tui.status));
}

static void setterm_tui_render(void)
{
    solar_os_tui_t *tui = &setterm_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_tui_clear(tui);

    size_t split = cols / 2;
    if (cols >= 24 && split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    solar_os_tui_write_cell(&setterm_tui.tui, 0,
                           0,
                           split,
                           "parameter",
                           SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_write_cell(&setterm_tui.tui, 0,
                               split + 1,
                               setterm_tui_visible_width(cols, split + 1),
                               "value",
                               SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t value_col = split + 1;
    const size_t value_width = setterm_tui_visible_width(cols, value_col);
    const size_t visible_items = rows > 2 ? rows - 2 : 0;
    if (setterm_tui.selected < setterm_tui.first_visible) {
        setterm_tui.first_visible = setterm_tui.selected;
    } else if (visible_items > 0 &&
               setterm_tui.selected >= setterm_tui.first_visible + visible_items) {
        setterm_tui.first_visible = setterm_tui.selected - visible_items + 1;
    }
    for (size_t row = 0; row < visible_items; row++) {
        const size_t i = setterm_tui.first_visible + row;
        if (i >= SETTERM_TUI_ITEM_COUNT) {
            break;
        }
        char value[SETTERM_TUI_EDIT_MAX];
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (i == setterm_tui.selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }

        setterm_tui_current_value((setterm_tui_item_t)i, value, sizeof(value));
        if (setterm_tui.editing && i == setterm_tui.selected) {
            strlcpy(value, setterm_tui.edit_text, sizeof(value));
            value_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
        }

        solar_os_tui_write_cell(&setterm_tui.tui, row + 1,
                               0,
                               split,
                               setterm_tui_items[i].label,
                               label_attr);
        if (value_width > 0) {
            solar_os_tui_write_cell(&setterm_tui.tui,
                                    row + 1,
                                    value_col,
                                    value_width,
                                    value,
                                    value_attr);
        }
    }

    if (rows > 1) {
        solar_os_tui_draw_help(
            &setterm_tui.tui,
            setterm_tui.status[0] != '\0' ? setterm_tui.status :
                "arrows select/change  enter edit  esc exits");
    }

    if (setterm_tui.editing && value_width > 0 && visible_items > 0) {
        const size_t edit_len = strlen(setterm_tui.edit_text);
        size_t cursor_col = value_col + edit_len;
        if (cursor_col >= cols) {
            cursor_col = cols - 1;
        }
        solar_os_tui_move(tui,
                          setterm_tui.selected - setterm_tui.first_visible + 1,
                          cursor_col);
    }

    solar_os_tui_set_cursor_visible(tui, setterm_tui.editing && setterm_tui.cursor_visible);
    solar_os_tui_refresh(tui);
}

static bool setterm_tui_cycle_value(const char * const *values,
                                    size_t count,
                                    int direction)
{
    size_t index = 0;

    if (values == NULL || count == 0) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(setterm_tui.edit_text, values[i]) == 0) {
            index = i;
            break;
        }
    }

    if (direction > 0) {
        index = (index + 1) % count;
    } else {
        index = index == 0 ? count - 1 : index - 1;
    }
    strlcpy(setterm_tui.edit_text, values[index], sizeof(setterm_tui.edit_text));
    return true;
}

static bool setterm_tui_cycle_selected(int direction)
{
    static const char * const orientation_values[] = {"0", "90", "180", "270"};
    static const char * const font_values[] = {"mono", "compact"};
    static const char * const textsize_values[] = {"10", "12", "14", "16", "18", "20"};
    static const char * const palette_values[] = {"normal", "inverted"};
    static const char * const statusbar_values[] = {"show", "hide"};
    static const char * const brightness_values[] = {"0", "25", "50", "75", "100"};
    static const char * const powerkey_values[] = {"sleep", "suspend"};
    static const char * const startup_values[] = {"flash", "sd"};
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    static const char * const keyboard_values[] = {"us", "de"};
    static const char * const ble_values[] = {"default", "on", "off"};
#endif

    switch ((setterm_tui_item_t)setterm_tui.selected) {
    case SETTERM_TUI_ORIENTATION:
        return setterm_tui_cycle_value(orientation_values,
                                       sizeof(orientation_values) / sizeof(orientation_values[0]),
                                       direction);
    case SETTERM_TUI_FONT:
        return setterm_tui_cycle_value(font_values,
                                       sizeof(font_values) / sizeof(font_values[0]),
                                       direction);
    case SETTERM_TUI_TEXTSIZE:
        return setterm_tui_cycle_value(textsize_values,
                                       sizeof(textsize_values) / sizeof(textsize_values[0]),
                                       direction);
    case SETTERM_TUI_PALETTE:
        return setterm_tui_cycle_value(palette_values,
                                       sizeof(palette_values) / sizeof(palette_values[0]),
                                       direction);
    case SETTERM_TUI_STATUSBAR:
        return setterm_tui_cycle_value(statusbar_values,
                                       sizeof(statusbar_values) / sizeof(statusbar_values[0]),
                                       direction);
    case SETTERM_TUI_BRIGHTNESS:
        return setterm_tui_cycle_value(brightness_values,
                                       sizeof(brightness_values) / sizeof(brightness_values[0]),
                                       direction);
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    case SETTERM_TUI_KEYBOARD:
        return setterm_tui_cycle_value(keyboard_values,
                                       sizeof(keyboard_values) / sizeof(keyboard_values[0]),
                                       direction);
    case SETTERM_TUI_BLE:
        return setterm_tui_cycle_value(ble_values,
                                       sizeof(ble_values) / sizeof(ble_values[0]),
                                       direction);
#endif
    case SETTERM_TUI_POWERKEY:
        return setterm_tui_cycle_value(powerkey_values,
                                       sizeof(powerkey_values) / sizeof(powerkey_values[0]),
                                       direction);
    case SETTERM_TUI_STARTUP:
        return setterm_tui_cycle_value(
            startup_values,
            solar_os_board_has(SOLAR_OS_BOARD_CAP_SD) ?
                sizeof(startup_values) / sizeof(startup_values[0]) : 1,
            direction);
    default:
        return false;
    }
}

static void setterm_tui_reset_cursor_blink(void)
{
    setterm_tui.cursor_visible = true;
    setterm_tui.last_cursor_blink_ms = 0;
}

static int setterm_tui_tokenize(char *line, char **argv, int argv_max)
{
    int argc = 0;
    char *saveptr = NULL;
    char *token = NULL;

    if (line == NULL || argv == NULL || argv_max <= 0) {
        return 0;
    }

    token = strtok_r(line, " \t", &saveptr);
    while (token != NULL && argc < argv_max) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    return argc;
}

static bool setterm_tui_apply_keyrate(void)
{
    char text[SETTERM_TUI_EDIT_MAX];
    char *argv[2];
    uint16_t current_delay_ms = 0;

    strlcpy(text, setterm_tui.edit_text, sizeof(text));
    const int argc = setterm_tui_tokenize(text, argv, 2);
    if (argc < 1) {
        return false;
    }

    solar_os_input_get_repeat(NULL, &current_delay_ms);
    if (strcmp(argv[0], "off") == 0) {
        return argc == 1 &&
            solar_os_input_set_repeat(0, current_delay_ms) == ESP_OK;
    }

    size_t rate = 0;
    size_t delay_ms = current_delay_ms;
    if (!parse_size_arg(argv[0],
                        SOLAR_OS_INPUT_REPEAT_RATE_MIN,
                        SOLAR_OS_INPUT_REPEAT_RATE_MAX,
                        &rate)) {
        return false;
    }
    if (argc == 2 &&
        !parse_size_arg(argv[1],
                        SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS,
                        SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS,
                        &delay_ms)) {
        return false;
    }

    return solar_os_input_set_repeat((uint16_t)rate, (uint16_t)delay_ms) == ESP_OK;
}

static bool setterm_tui_apply_selected(void)
{
    solar_os_terminal_t *term = display_terminal(setterm_tui.ctx);

    switch ((setterm_tui_item_t)setterm_tui.selected) {
    case SETTERM_TUI_ORIENTATION: {
        size_t degrees = 0;
        if (!parse_size_arg(setterm_tui.edit_text, 0, 270, &degrees) ||
            !(degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270)) {
            return false;
        }
        return solar_os_sessions_set_terminal_orientation(term, (uint16_t)degrees) == ESP_OK;
    }
    case SETTERM_TUI_FONT: {
        solar_os_terminal_font_t font;
        return solar_os_terminal_parse_font(setterm_tui.edit_text, &font) &&
            solar_os_sessions_set_terminal_font(term, font) == ESP_OK;
    }
    case SETTERM_TUI_TEXTSIZE: {
        solar_os_terminal_text_size_t text_size;
        return solar_os_terminal_parse_text_size(setterm_tui.edit_text, &text_size) &&
            solar_os_sessions_set_terminal_text_size(term, text_size) == ESP_OK;
    }
    case SETTERM_TUI_PALETTE:
        if (strcmp(setterm_tui.edit_text, "normal") == 0) {
            return solar_os_sessions_set_terminal_palette_inverted(term, false) == ESP_OK;
        }
        if (strcmp(setterm_tui.edit_text, "inverted") == 0) {
            return solar_os_sessions_set_terminal_palette_inverted(term, true) == ESP_OK;
        }
        return false;
    case SETTERM_TUI_FOREGROUND:
    case SETTERM_TUI_BACKGROUND: {
        uint32_t rgb888 = 0;
        if (!solar_os_shell_parse_rgb888(setterm_tui.edit_text, &rgb888)) {
            return false;
        }
        return ((setterm_tui_item_t)setterm_tui.selected == SETTERM_TUI_FOREGROUND ?
                    solar_os_display_set_foreground_color(rgb888) :
                    solar_os_display_set_background_color(rgb888)) == ESP_OK;
    }
    case SETTERM_TUI_STATUSBAR:
        if (strcmp(setterm_tui.edit_text, "show") == 0) {
            return solar_os_sessions_set_terminal_status_bar_visible(term, true) == ESP_OK;
        }
        if (strcmp(setterm_tui.edit_text, "hide") == 0) {
            return solar_os_sessions_set_terminal_status_bar_visible(term, false) == ESP_OK;
        }
        return false;
    case SETTERM_TUI_BRIGHTNESS: {
        size_t percent = 0;
        return parse_size_arg(setterm_tui.edit_text, 0, 100, &percent) &&
            solar_os_display_set_brightness((uint8_t)percent) == ESP_OK;
    }
    case SETTERM_TUI_KEYBOARD: {
        solar_os_input_keyboard_layout_t layout;
        return solar_os_input_parse_keyboard_layout(setterm_tui.edit_text, &layout) &&
            solar_os_input_set_keyboard_layout(layout) == ESP_OK;
    }
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    case SETTERM_TUI_BLE: {
        solar_os_ble_keyboard_boot_setting_t setting;
        return solar_os_ble_keyboard_parse_boot_setting(setterm_tui.edit_text, &setting) &&
            solar_os_ble_keyboard_set_boot_setting(setting) == ESP_OK;
    }
#endif
    case SETTERM_TUI_POWERKEY: {
        solar_os_power_key_action_t action;
        return solar_os_power_parse_key_action(setterm_tui.edit_text, &action) &&
            (action == SOLAR_OS_POWER_KEY_ACTION_SLEEP ||
             action == SOLAR_OS_POWER_KEY_ACTION_SUSPEND) &&
            solar_os_power_set_key_action(action) == ESP_OK;
    }
    case SETTERM_TUI_KEYRATE:
        return setterm_tui_apply_keyrate();
    case SETTERM_TUI_TIMEZONE:
        return solar_os_time_set_timezone(setterm_tui.edit_text) == ESP_OK;
    case SETTERM_TUI_STARTUP: {
        solar_os_shell_startup_source_t source;
        return solar_os_shell_parse_startup_source(setterm_tui.edit_text, &source) &&
            solar_os_shell_set_startup_source(source) == ESP_OK;
    }
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    case SETTERM_TUI_OTAURL:
        return solar_os_ota_set_url(setterm_tui.edit_text) == ESP_OK;
#endif
    default:
        return false;
    }
}

static void setterm_tui_begin_edit(void)
{
    setterm_tui_current_value((setterm_tui_item_t)setterm_tui.selected,
                              setterm_tui.edit_text,
                              sizeof(setterm_tui.edit_text));
    strlcpy(setterm_tui.original_text,
            setterm_tui.edit_text,
            sizeof(setterm_tui.original_text));
    setterm_tui.editing = true;
    setterm_tui_reset_cursor_blink();
    setterm_tui_set_status("");
    setterm_tui_render();
}

static void setterm_tui_commit_edit(void)
{
    if (setterm_tui_apply_selected()) {
        setterm_tui.editing = false;
        setterm_tui.cursor_visible = false;
        setterm_tui_set_status(
            setterm_tui.selected == SETTERM_TUI_STARTUP
#if SOLAR_OS_PACKAGE_SERVICE_BLE
                || setterm_tui.selected == SETTERM_TUI_BLE
#endif
                ?
                "saved; reboot to apply" : "saved");
    } else {
        setterm_tui_reset_cursor_blink();
        setterm_tui_set_status("invalid value");
    }
    setterm_tui_render();
}

static void setterm_tui_cancel_edit(void)
{
    strlcpy(setterm_tui.edit_text,
            setterm_tui.original_text,
            sizeof(setterm_tui.edit_text));
    setterm_tui.editing = false;
    setterm_tui.cursor_visible = false;
    setterm_tui_set_status("");
    setterm_tui_render();
}

static void setterm_tui_handle_edit_key(char ch)
{
    const uint8_t key = (uint8_t)ch;
    const size_t len = strlen(setterm_tui.edit_text);

    setterm_tui_reset_cursor_blink();

    switch (key) {
    case SOLAR_OS_KEY_ESCAPE:
        setterm_tui_cancel_edit();
        return;
    case SOLAR_OS_KEY_LEFT:
        if (setterm_tui_cycle_selected(-1)) {
            setterm_tui_render();
        }
        return;
    case SOLAR_OS_KEY_RIGHT:
        if (setterm_tui_cycle_selected(1)) {
            setterm_tui_render();
        }
        return;
    case '\r':
    case '\n':
        setterm_tui_commit_edit();
        return;
    case '\b':
    case 0x7f:
    case SOLAR_OS_KEY_DELETE:
        if (len > 0) {
            setterm_tui.edit_text[len - 1] = '\0';
            setterm_tui_set_status("");
            setterm_tui_render();
        }
        return;
    default:
        break;
    }

    if (isprint((unsigned char)ch) && len + 1 < sizeof(setterm_tui.edit_text)) {
        setterm_tui.edit_text[len] = ch;
        setterm_tui.edit_text[len + 1] = '\0';
        setterm_tui_set_status("");
        setterm_tui_render();
    }
}

static esp_err_t setterm_tui_start(solar_os_context_t *ctx)
{
    memset(&setterm_tui, 0, sizeof(setterm_tui));
    setterm_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_screen_begin(&setterm_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    solar_os_tui_set_cursor_visible(&setterm_tui.tui, false);
    setterm_tui_render();
    return ESP_OK;
}

static void setterm_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&setterm_tui.tui, true);
    solar_os_tui_clear(&setterm_tui.tui);
    solar_os_tui_refresh(&setterm_tui.tui);
    solar_os_tui_end(&setterm_tui.tui);
}

static bool setterm_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!setterm_tui.editing) {
            return false;
        }

        const uint32_t now_ms = event->data.tick_ms;
        if (setterm_tui.last_cursor_blink_ms == 0) {
            setterm_tui.last_cursor_blink_ms = now_ms;
            return true;
        }
        if ((now_ms - setterm_tui.last_cursor_blink_ms) >= SETTERM_TUI_CURSOR_BLINK_MS) {
            setterm_tui.last_cursor_blink_ms = now_ms;
            setterm_tui.cursor_visible = !setterm_tui.cursor_visible;
            solar_os_tui_set_cursor_visible(&setterm_tui.tui, setterm_tui.cursor_visible);
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(setterm_tui.ctx);
        return true;
    }

    if (setterm_tui.editing) {
        setterm_tui_handle_edit_key(event->data.ch);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (setterm_tui.selected > 0) {
            setterm_tui.selected--;
            setterm_tui_set_status("");
            setterm_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (setterm_tui.selected + 1 < SETTERM_TUI_ITEM_COUNT) {
            setterm_tui.selected++;
            setterm_tui_set_status("");
            setterm_tui_render();
        }
        break;
    case '\r':
    case '\n':
        setterm_tui_begin_edit();
        break;
    case SOLAR_OS_KEY_ESCAPE:
        solar_os_context_request_exit(setterm_tui.ctx);
        break;
    default:
        break;
    }

    return true;
}

static const solar_os_app_t setterm_tui_app = {
    .name = "setterm",
    .summary = "terminal settings",
    .start = setterm_tui_start,
    .stop = setterm_tui_stop,
    .event = setterm_tui_event,
    .state_slot = &setterm_tui_state,
    .state_size = sizeof(setterm_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};

esp_err_t solar_os_shell_launch_setterm_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &setterm_tui_app, 0, NULL);
}
