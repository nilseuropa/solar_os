#include "solar_os_shell_tui_apps.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_ble.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_keys.h"
#include "solar_os_shell_common.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define BLE_TUI_MIN_ROWS 7U
#define BLE_TUI_MIN_COLS 30U
#define BLE_TUI_STATUS_MAX 112U
#define BLE_TUI_POPUP_TEXT_MAX 192U
#define BLE_TUI_POPUP_TITLE_MAX 32U
#define BLE_TUI_WRITE_TEXT_MAX (SOLAR_OS_BLE_GATT_VALUE_MAX * 3U + 1U)
#define BLE_TUI_TIMEOUT_MIN_MS 1000U
#define BLE_TUI_TIMEOUT_MAX_MS 60000U
#define BLE_TUI_TIMEOUT_STEP_MS 1000U
#define BLE_TUI_REFRESH_MS 1000U
#define BLE_TUI_PROGRESS_HOLD_MS 300U

typedef enum {
    BLE_TUI_TAB_DEVICES,
    BLE_TUI_TAB_SERVICES,
    BLE_TUI_TAB_CHARACTERISTICS,
    BLE_TUI_TAB_SETTINGS,
    BLE_TUI_TAB_COUNT,
} ble_tui_tab_t;

typedef enum {
    BLE_TUI_OPERATION_NONE,
    BLE_TUI_OPERATION_SCAN,
    BLE_TUI_OPERATION_CONNECT,
} ble_tui_operation_t;

typedef enum {
    BLE_TUI_SETTING_BOOT,
    BLE_TUI_SETTING_KEEPALIVE,
    BLE_TUI_SETTING_LAYOUT,
    BLE_TUI_SETTING_REPEAT_RATE,
    BLE_TUI_SETTING_REPEAT_DELAY,
    BLE_TUI_SETTING_TIMEOUT,
    BLE_TUI_SETTING_WRITE_MODE,
    BLE_TUI_SETTING_PAIR,
    BLE_TUI_SETTING_FORGET,
    BLE_TUI_SETTING_DISCONNECT,
    BLE_TUI_SETTING_COUNT,
} ble_tui_setting_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    ble_tui_tab_t tab;
    solar_os_ble_session_t session;
    solar_os_ble_peer_t peer;
    solar_os_tui_viewport_t devices_viewport;
    solar_os_tui_viewport_t services_viewport;
    solar_os_tui_viewport_t chars_viewport;
    solar_os_tui_viewport_t settings_viewport;
    solar_os_ble_scan_result_t devices[SOLAR_OS_BLE_SCAN_MAX_RESULTS];
    size_t device_count;
    ble_tui_operation_t operation;
    uint32_t operation_not_before_ms;
    solar_os_ble_scan_result_t operation_device;
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t service_count;
    solar_os_ble_gatt_characteristic_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t char_count;
    size_t chars_service_index;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len;
    uint16_t value_handle;
    bool value_view;
    bool editing_write;
    bool write_with_response;
    char write_text[BLE_TUI_WRITE_TEXT_MAX];
    solar_os_tui_input_state_t write_input;
    uint32_t timeout_ms;
    uint32_t last_refresh_ms;
    char status[BLE_TUI_STATUS_MAX];
    bool popup_active;
    char popup_title[BLE_TUI_POPUP_TITLE_MAX];
    char popup_text[BLE_TUI_POPUP_TEXT_MAX];
} ble_tui_state_t;

static void *ble_tui_state;
#define ble_tui (*(ble_tui_state_t *)ble_tui_state)

static bool ble_tui_enter(uint8_t key)
{
    return key == SOLAR_OS_KEY_ENTER || key == '\r' || key == '\n';
}

static void ble_tui_set_status(const char *status)
{
    strlcpy(ble_tui.status, status != NULL ? status : "", sizeof(ble_tui.status));
}

static void ble_tui_set_error(const char *operation, esp_err_t err)
{
    if (err == SOLAR_OS_BLE_ERR_CAPACITY) {
        snprintf(ble_tui.status, sizeof(ble_tui.status), "%s: connection capacity reached", operation);
    } else if (err == SOLAR_OS_BLE_ERR_CANCELLED) {
        snprintf(ble_tui.status, sizeof(ble_tui.status), "%s: cancelled", operation);
    } else if (err == ESP_ERR_TIMEOUT) {
        snprintf(ble_tui.status, sizeof(ble_tui.status), "%s: timeout", operation);
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(ble_tui.status, sizeof(ble_tui.status), "%s: unavailable or busy", operation);
    } else {
        snprintf(ble_tui.status,
                 sizeof(ble_tui.status),
                 "%s: %s",
                 operation,
                 solar_os_shell_error_text(err));
    }
}

static void ble_tui_set_popup(const char *title, const char *text)
{
    strlcpy(ble_tui.popup_title,
            title != NULL ? title : "BLE",
            sizeof(ble_tui.popup_title));
    strlcpy(ble_tui.popup_text,
            text != NULL ? text : "",
            sizeof(ble_tui.popup_text));
    ble_tui.popup_active = true;
}

static void ble_tui_format_address(const uint8_t bda[6], char address[18])
{
    snprintf(address,
             18,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void ble_tui_format_properties(uint8_t properties,
                                      char *buffer,
                                      size_t buffer_len)
{
    size_t len = 0U;
    static const struct {
        uint8_t property;
        char label;
    } labels[] = {
        {SOLAR_OS_BLE_CHAR_READ, 'r'},
        {SOLAR_OS_BLE_CHAR_WRITE_NO_RESPONSE, 'w'},
        {SOLAR_OS_BLE_CHAR_WRITE, 'W'},
        {SOLAR_OS_BLE_CHAR_NOTIFY, 'n'},
        {SOLAR_OS_BLE_CHAR_INDICATE, 'i'},
    };
    for (size_t i = 0U; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if ((properties & labels[i].property) != 0U && len + 1U < buffer_len) {
            buffer[len++] = labels[i].label;
        }
    }
    if (len == 0U && buffer_len > 1U) {
        buffer[len++] = '-';
    }
    if (buffer_len > 0U) {
        buffer[len] = '\0';
    }
}

static size_t ble_tui_visible_rows(void)
{
    const size_t rows = solar_os_tui_rows(&ble_tui.tui);
    const size_t reserved = ble_tui.editing_write ? 5U : 4U;
    return rows > reserved ? rows - reserved : 0U;
}

static bool ble_tui_peer_connected(solar_os_ble_session_info_t *info)
{
    if (ble_tui.peer == SOLAR_OS_BLE_PEER_INVALID) {
        return false;
    }
    solar_os_ble_session_info_t current = {0};
    const esp_err_t err = solar_os_ble_peer_get_info(ble_tui.session,
                                                      ble_tui.peer,
                                                      &current);
    if (err != ESP_OK) {
        return false;
    }
    if (info != NULL) {
        *info = current;
    }
    return current.gatt.connected;
}

static void ble_tui_release_peer(void)
{
    if (ble_tui.peer != SOLAR_OS_BLE_PEER_INVALID) {
        (void)solar_os_ble_peer_disconnect(ble_tui.session, ble_tui.peer);
        ble_tui.peer = SOLAR_OS_BLE_PEER_INVALID;
    }
    ble_tui.service_count = 0U;
    ble_tui.char_count = 0U;
    ble_tui.value_view = false;
}

static void ble_tui_draw_tabs(const solar_os_tui_screen_layout_t *layout)
{
    static const char * const labels[BLE_TUI_TAB_COUNT] = {
        "Devices", "Services", "Chars", "Settings",
    };
    size_t col = 0U;
    for (size_t i = 0U; i < BLE_TUI_TAB_COUNT; i++) {
        const size_t remaining = layout->tabs.width - col;
        const size_t tabs_left = BLE_TUI_TAB_COUNT - i;
        const size_t width = tabs_left > 0U ? remaining / tabs_left : 0U;
        solar_os_tui_draw_tab(&ble_tui.tui,
                              layout->tabs.row,
                              col,
                              width,
                              labels[i],
                              i == (size_t)ble_tui.tab);
        col += width;
    }
}

static void ble_tui_draw_devices(const solar_os_tui_screen_layout_t *layout)
{
    solar_os_tui_viewport_reconcile(&ble_tui.devices_viewport,
                                    ble_tui.device_count,
                                    layout->body.height);
    if (ble_tui.device_count == 0U && layout->body.height > 0U) {
        solar_os_tui_write_cell(&ble_tui.tui,
                                layout->body.row,
                                0U,
                                layout->body.width,
                                solar_os_ble_keyboard_enabled_for_current_boot() ?
                                    "Press R or Enter to scan for BLE devices." :
                                    "BLE is disabled this boot; enable it in Settings.",
                                SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = ble_tui.devices_viewport.top + row;
        char line[128] = "";
        if (index < ble_tui.device_count) {
            const solar_os_ble_scan_result_t *device = &ble_tui.devices[index];
            char address[18];
            ble_tui_format_address(device->bda, address);
            snprintf(line,
                     sizeof(line),
                     "%4d %-17s %-10s %c%c%c %s",
                     (int)device->rssi,
                     address,
                     solar_os_ble_keyboard_addr_type_name(device->addr_type),
                     device->connected ? 'c' : '-',
                     device->hid_service ? 'h' : '-',
                     device->remembered ? '*' : '-',
                     device->name[0] != '\0' ? device->name : "(unnamed)");
        }
        solar_os_tui_write_cell(
            &ble_tui.tui,
            layout->body.row + row,
            0U,
            layout->body.width,
            line,
            index == ble_tui.devices_viewport.cursor ? SOLAR_OS_TUI_ATTR_INVERSE :
                                                       SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void ble_tui_draw_services(const solar_os_tui_screen_layout_t *layout)
{
    solar_os_tui_viewport_reconcile(&ble_tui.services_viewport,
                                    ble_tui.service_count,
                                    layout->body.height);
    if (ble_tui.service_count == 0U && layout->body.height > 0U) {
        solar_os_tui_write_cell(&ble_tui.tui,
                                layout->body.row,
                                0U,
                                layout->body.width,
                                ble_tui_peer_connected(NULL) ? "No services discovered." :
                                                              "Connect to a device first.",
                                SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = ble_tui.services_viewport.top + row;
        char line[96] = "";
        if (index < ble_tui.service_count) {
            const solar_os_ble_gatt_service_t *service = &ble_tui.services[index];
            snprintf(line,
                     sizeof(line),
                     "%2u %04x-%04x %c %s",
                     (unsigned)index,
                     (unsigned)service->start_handle,
                     (unsigned)service->end_handle,
                     service->primary ? 'p' : '-',
                     service->uuid);
        }
        solar_os_tui_write_cell(
            &ble_tui.tui,
            layout->body.row + row,
            0U,
            layout->body.width,
            line,
            index == ble_tui.services_viewport.cursor ? SOLAR_OS_TUI_ATTR_INVERSE :
                                                        SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void ble_tui_draw_value(const solar_os_tui_screen_layout_t *layout)
{
    size_t offset = 0U;
    for (size_t row = 0U; row < layout->body.height; row++) {
        char line[96] = "";
        if (row == 0U) {
            snprintf(line,
                     sizeof(line),
                     "handle 0x%04x, %u byte%s",
                     (unsigned)ble_tui.value_handle,
                     (unsigned)ble_tui.value_len,
                     ble_tui.value_len == 1U ? "" : "s");
        } else if (offset < ble_tui.value_len) {
            size_t used = 0U;
            used += (size_t)snprintf(line + used,
                                     sizeof(line) - used,
                                     "%04x: ",
                                     (unsigned)offset);
            const size_t chunk = ble_tui.value_len - offset > 16U ? 16U :
                                                                         ble_tui.value_len - offset;
            for (size_t i = 0U; i < chunk && used + 4U < sizeof(line); i++) {
                used += (size_t)snprintf(line + used,
                                         sizeof(line) - used,
                                         "%02x ",
                                         ble_tui.value[offset + i]);
            }
            if (used + 2U < sizeof(line)) {
                line[used++] = ' ';
                line[used++] = '|';
            }
            for (size_t i = 0U; i < chunk && used + 2U < sizeof(line); i++) {
                const uint8_t value = ble_tui.value[offset + i];
                line[used++] = isprint(value) ? (char)value : '.';
            }
            if (used + 1U < sizeof(line)) {
                line[used++] = '|';
            }
            line[used] = '\0';
            offset += chunk;
        }
        solar_os_tui_write_cell(&ble_tui.tui,
                                layout->body.row + row,
                                0U,
                                layout->body.width,
                                line,
                                row == 0U ? SOLAR_OS_TUI_ATTR_BOLD :
                                            SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void ble_tui_draw_characteristics(const solar_os_tui_screen_layout_t *layout)
{
    if (ble_tui.value_view) {
        ble_tui_draw_value(layout);
        return;
    }
    solar_os_tui_viewport_reconcile(&ble_tui.chars_viewport,
                                    ble_tui.char_count,
                                    layout->body.height);
    if (ble_tui.char_count == 0U && layout->body.height > 0U) {
        solar_os_tui_write_cell(&ble_tui.tui,
                                layout->body.row,
                                0U,
                                layout->body.width,
                                ble_tui.service_count > 0U ?
                                    "Select a service and press Enter." :
                                    "Connect to a device first.",
                                SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = ble_tui.chars_viewport.top + row;
        char line[96] = "";
        if (index < ble_tui.char_count) {
            char properties[8];
            ble_tui_format_properties(ble_tui.chars[index].properties,
                                      properties,
                                      sizeof(properties));
            snprintf(line,
                     sizeof(line),
                     "0x%04x %-5s %s",
                     (unsigned)ble_tui.chars[index].handle,
                     properties,
                     ble_tui.chars[index].uuid);
        }
        solar_os_tui_write_cell(
            &ble_tui.tui,
            layout->body.row + row,
            0U,
            layout->body.width,
            line,
            index == ble_tui.chars_viewport.cursor ? SOLAR_OS_TUI_ATTR_INVERSE :
                                                     SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void ble_tui_setting_text(ble_tui_setting_t setting,
                                 char *line,
                                 size_t line_len)
{
    uint16_t repeat_rate = 0U;
    uint16_t repeat_delay = 0U;
    solar_os_ble_keyboard_get_repeat(&repeat_rate, &repeat_delay);
    switch (setting) {
    case BLE_TUI_SETTING_BOOT:
        snprintf(line,
                 line_len,
                 "boot preference     %s",
                 solar_os_ble_keyboard_boot_setting_name(
                     solar_os_ble_keyboard_boot_setting()));
        break;
    case BLE_TUI_SETTING_KEEPALIVE:
        snprintf(line,
                 line_len,
                 "keyboard keepalive  %s",
                 solar_os_ble_keyboard_keepalive_enabled() ? "on" : "off");
        break;
    case BLE_TUI_SETTING_LAYOUT:
        snprintf(line,
                 line_len,
                 "keyboard layout     %s",
                 solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout()));
        break;
    case BLE_TUI_SETTING_REPEAT_RATE:
        snprintf(line, line_len, "repeat rate        %u cps", (unsigned)repeat_rate);
        break;
    case BLE_TUI_SETTING_REPEAT_DELAY:
        snprintf(line, line_len, "repeat delay       %u ms", (unsigned)repeat_delay);
        break;
    case BLE_TUI_SETTING_TIMEOUT:
        snprintf(line, line_len, "operation timeout  %u ms", (unsigned)ble_tui.timeout_ms);
        break;
    case BLE_TUI_SETTING_WRITE_MODE:
        snprintf(line,
                 line_len,
                 "default write       %s",
                 ble_tui.write_with_response ? "with response" : "without response");
        break;
    case BLE_TUI_SETTING_PAIR:
        strlcpy(line, "pair keyboard...", line_len);
        break;
    case BLE_TUI_SETTING_FORGET:
        strlcpy(line, "forget keyboard...", line_len);
        break;
    case BLE_TUI_SETTING_DISCONNECT:
        strlcpy(line,
                ble_tui.peer == SOLAR_OS_BLE_PEER_INVALID ? "disconnect          -" :
                                                           "disconnect          Enter",
                line_len);
        break;
    default:
        line[0] = '\0';
        break;
    }
}

static void ble_tui_draw_settings(const solar_os_tui_screen_layout_t *layout)
{
    solar_os_tui_viewport_reconcile(&ble_tui.settings_viewport,
                                    BLE_TUI_SETTING_COUNT,
                                    layout->body.height);
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = ble_tui.settings_viewport.top + row;
        char line[80] = "";
        if (index < BLE_TUI_SETTING_COUNT) {
            ble_tui_setting_text((ble_tui_setting_t)index, line, sizeof(line));
        }
        solar_os_tui_write_cell(
            &ble_tui.tui,
            layout->body.row + row,
            0U,
            layout->body.width,
            line,
            index == ble_tui.settings_viewport.cursor ? SOLAR_OS_TUI_ATTR_INVERSE :
                                                        SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static solar_os_tui_rect_t ble_tui_popup_bounds(void)
{
    const size_t rows = solar_os_tui_rows(&ble_tui.tui);
    return (solar_os_tui_rect_t) {
        .row = rows > 2U ? 1U : 0U,
        .col = 0U,
        .height = rows > 2U ? rows - 2U : rows,
        .width = solar_os_tui_cols(&ble_tui.tui),
    };
}

static void ble_tui_draw_popup(const char *title, const char *text)
{
    const solar_os_tui_rect_t bounds = ble_tui_popup_bounds();
    (void)solar_os_tui_text_popup(&ble_tui.tui,
                                  &bounds,
                                  title,
                                  text,
                                  NULL);
}

static void ble_tui_draw_connect_progress(void);

static void ble_tui_render(void)
{
    const size_t rows = solar_os_tui_rows(&ble_tui.tui);
    const size_t cols = solar_os_tui_cols(&ble_tui.tui);
    if (rows < BLE_TUI_MIN_ROWS || cols < BLE_TUI_MIN_COLS) {
        solar_os_tui_draw_too_small(&ble_tui.tui, "ble");
        solar_os_tui_refresh(&ble_tui.tui);
        return;
    }

    solar_os_tui_screen_layout_t layout;
    if (!solar_os_tui_layout_compute(rows,
                                     cols,
                                     1U,
                                     1U,
                                     ble_tui.editing_write ? 1U : 0U,
                                     &layout)) {
        solar_os_tui_draw_too_small(&ble_tui.tui, "ble");
        solar_os_tui_refresh(&ble_tui.tui);
        return;
    }

    solar_os_ble_session_info_t info = {0};
    const bool connected = ble_tui_peer_connected(&info);
    char detail[48];
    if (connected) {
        char address[18];
        ble_tui_format_address(info.gatt.bda, address);
        snprintf(detail, sizeof(detail), "%s mtu %u", address, (unsigned)info.gatt.mtu);
    } else {
        char keyboard[32];
        solar_os_ble_keyboard_get_status(keyboard, sizeof(keyboard));
        snprintf(detail, sizeof(detail), "keyboard %s", keyboard);
    }

    solar_os_tui_clear(&ble_tui.tui);
    solar_os_tui_draw_title(&ble_tui.tui, "BLE Inspector", detail);
    ble_tui_draw_tabs(&layout);
    switch (ble_tui.tab) {
    case BLE_TUI_TAB_DEVICES:
        ble_tui_draw_devices(&layout);
        break;
    case BLE_TUI_TAB_SERVICES:
        ble_tui_draw_services(&layout);
        break;
    case BLE_TUI_TAB_CHARACTERISTICS:
        ble_tui_draw_characteristics(&layout);
        break;
    case BLE_TUI_TAB_SETTINGS:
        ble_tui_draw_settings(&layout);
        break;
    default:
        break;
    }

    if (ble_tui.editing_write) {
        solar_os_tui_draw_input(&ble_tui.tui,
                                layout.input.row,
                                layout.input.col,
                                layout.input.width,
                                "hex: ",
                                ble_tui.write_text,
                                &ble_tui.write_input,
                                SOLAR_OS_TUI_ATTR_INVERSE);
    }
    if (layout.status.height > 0U) {
        solar_os_tui_write_cell(&ble_tui.tui,
                                layout.status.row,
                                layout.status.col,
                                layout.status.width,
                                ble_tui.status,
                                SOLAR_OS_TUI_ATTR_NORMAL);
    }
    const char *help = "TAB next  arrows select  ESC exit";
    if (ble_tui.tab == BLE_TUI_TAB_DEVICES) {
        help = "R scan  ENTER connect  D disconnect  TAB next  ESC exit";
    } else if (ble_tui.tab == BLE_TUI_TAB_SERVICES) {
        help = "ENTER characteristics  D disconnect  TAB next  ESC exit";
    } else if (ble_tui.tab == BLE_TUI_TAB_CHARACTERISTICS) {
        help = ble_tui.value_view ? "ESC back  TAB next" :
                                    "ENTER read  W write  D disconnect  TAB next  ESC exit";
    } else if (ble_tui.tab == BLE_TUI_TAB_SETTINGS) {
        help = "arrows select/change  ENTER acts  TAB next  ESC exit";
    }
    if (ble_tui.editing_write) {
        help = "hex bytes: 01 a0 ff  ENTER write  ESC cancel";
    }
    if (ble_tui.popup_active) {
        help = "ANY close";
    }
    solar_os_tui_write_cell(&ble_tui.tui,
                            layout.help.row,
                            layout.help.col,
                            layout.help.width,
                            help,
                            SOLAR_OS_TUI_ATTR_INVERSE);
    if (ble_tui.popup_active) {
        ble_tui_draw_popup(ble_tui.popup_title, ble_tui.popup_text);
    } else if (ble_tui.operation == BLE_TUI_OPERATION_SCAN) {
        ble_tui_draw_popup("Scanning",
                           "Searching for nearby BLE devices.");
    } else if (ble_tui.operation == BLE_TUI_OPERATION_CONNECT) {
        ble_tui_draw_connect_progress();
    }
    solar_os_tui_set_cursor_visible(&ble_tui.tui,
                                    ble_tui.editing_write &&
                                        !ble_tui.popup_active &&
                                        ble_tui.operation == BLE_TUI_OPERATION_NONE);
    solar_os_tui_refresh(&ble_tui.tui);
}

static void ble_tui_draw_connect_progress(void)
{
    const solar_os_ble_scan_result_t *device = &ble_tui.operation_device;
    char address[18];
    ble_tui_format_address(device->bda, address);
    char message[BLE_TUI_POPUP_TEXT_MAX];
    snprintf(message,
             sizeof(message),
             "%s\n%s (%s)\nWaiting for link and GATT discovery.\nTimeout: %u s",
             device->name[0] != '\0' ? device->name : "(unnamed)",
             address,
             solar_os_ble_keyboard_addr_type_name(device->addr_type),
             (unsigned)(ble_tui.timeout_ms / 1000U));
    ble_tui_draw_popup("Connecting", message);
}

static void ble_tui_begin_scan(void)
{
    if (!solar_os_ble_keyboard_enabled_for_current_boot()) {
        ble_tui_set_status("enable BLE in Settings, then reboot");
        return;
    }
    if (ble_tui.peer != SOLAR_OS_BLE_PEER_INVALID) {
        ble_tui_set_status("disconnect before scanning");
        return;
    }
    ble_tui_set_status("scanning...");
    ble_tui.operation_not_before_ms = 0U;
    ble_tui.operation = BLE_TUI_OPERATION_SCAN;
}

static void ble_tui_run_scan(void)
{
    size_t found = 0U;
    const esp_err_t err = solar_os_ble_scan(ble_tui.devices,
                                            SOLAR_OS_BLE_SCAN_MAX_RESULTS,
                                            &found);
    ble_tui.operation = BLE_TUI_OPERATION_NONE;
    ble_tui.operation_not_before_ms = 0U;
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        ble_tui.device_count = found < SOLAR_OS_BLE_SCAN_MAX_RESULTS ?
                                   found : SOLAR_OS_BLE_SCAN_MAX_RESULTS;
        ble_tui.devices_viewport = (solar_os_tui_viewport_t){0};
        if (ble_tui.device_count == 0U) {
            ble_tui_set_status("no BLE devices found");
        } else {
            snprintf(ble_tui.status,
                     sizeof(ble_tui.status),
                     "%u device%s found",
                     (unsigned)ble_tui.device_count,
                     ble_tui.device_count == 1U ? "" : "s");
        }
    } else {
        ble_tui_set_error("scan failed", err);
    }
}

static void ble_tui_begin_connect_selected(void)
{
    if (!solar_os_ble_keyboard_enabled_for_current_boot()) {
        ble_tui_set_status("enable BLE in Settings, then reboot");
        return;
    }
    if (ble_tui.device_count == 0U) {
        ble_tui_begin_scan();
        return;
    }
    if (ble_tui.devices_viewport.cursor >= ble_tui.device_count) {
        return;
    }
    if (ble_tui.peer != SOLAR_OS_BLE_PEER_INVALID) {
        ble_tui_set_status("disconnect the current device first");
        return;
    }
    const solar_os_ble_scan_result_t *device =
        &ble_tui.devices[ble_tui.devices_viewport.cursor];
    char address[18];
    ble_tui_format_address(device->bda, address);
    snprintf(ble_tui.status, sizeof(ble_tui.status), "connecting %s...", address);
    ble_tui.operation_device = *device;
    ble_tui.operation_not_before_ms = 0U;
    ble_tui.operation = BLE_TUI_OPERATION_CONNECT;
}

static void ble_tui_run_connect(void)
{
    const solar_os_ble_scan_result_t *device = &ble_tui.operation_device;
    char address[18];
    ble_tui_format_address(device->bda, address);
    const esp_err_t err = solar_os_ble_peer_connect(ble_tui.session,
                                                    device->bda,
                                                    device->addr_type,
                                                    ble_tui.timeout_ms,
                                                    &ble_tui.peer);
    ble_tui.operation = BLE_TUI_OPERATION_NONE;
    ble_tui.operation_not_before_ms = 0U;
    if (err != ESP_OK) {
        ble_tui.peer = SOLAR_OS_BLE_PEER_INVALID;
        ble_tui_set_error("connect failed", err);
        char message[BLE_TUI_POPUP_TEXT_MAX];
        snprintf(message,
                 sizeof(message),
                 "%s\n%s\nCause: %s\n\nPress any key to close.",
                 device->name[0] != '\0' ? device->name : "(unnamed)",
                 address,
                 ble_tui.status);
        ble_tui_set_popup("Connection failed", message);
        return;
    }
    ble_tui.service_count = 0U;
    const esp_err_t service_err = solar_os_ble_peer_services(
        ble_tui.session,
        ble_tui.peer,
        ble_tui.services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES,
        &ble_tui.service_count);
    if (service_err != ESP_OK) {
        ble_tui_set_error("services failed", service_err);
        return;
    }
    ble_tui.services_viewport = (solar_os_tui_viewport_t){0};
    ble_tui.char_count = 0U;
    ble_tui.tab = BLE_TUI_TAB_SERVICES;
    snprintf(ble_tui.status,
             sizeof(ble_tui.status),
             "connected; %u service%s",
             (unsigned)ble_tui.service_count,
             ble_tui.service_count == 1U ? "" : "s");
}

static void ble_tui_disconnect(void)
{
    if (ble_tui.peer == SOLAR_OS_BLE_PEER_INVALID) {
        ble_tui_set_status("not connected");
        return;
    }
    const esp_err_t err = solar_os_ble_peer_disconnect(ble_tui.session,
                                                        ble_tui.peer);
    ble_tui.peer = SOLAR_OS_BLE_PEER_INVALID;
    ble_tui.service_count = 0U;
    ble_tui.char_count = 0U;
    ble_tui.value_view = false;
    if (err == ESP_OK) {
        ble_tui_set_status("disconnected");
    } else {
        ble_tui_set_error("disconnect failed", err);
    }
}

static void ble_tui_open_service(void)
{
    if (!ble_tui_peer_connected(NULL)) {
        ble_tui_set_status("connect to a device first");
        return;
    }
    if (ble_tui.services_viewport.cursor >= ble_tui.service_count) {
        return;
    }
    ble_tui.char_count = 0U;
    ble_tui.chars_service_index = ble_tui.services_viewport.cursor;
    const esp_err_t err = solar_os_ble_peer_characteristics(
        ble_tui.session,
        ble_tui.peer,
        ble_tui.chars_service_index,
        ble_tui.chars,
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS,
        &ble_tui.char_count);
    if (err != ESP_OK) {
        ble_tui_set_error("characteristics failed", err);
        return;
    }
    ble_tui.chars_viewport = (solar_os_tui_viewport_t){0};
    ble_tui.value_view = false;
    ble_tui.tab = BLE_TUI_TAB_CHARACTERISTICS;
    snprintf(ble_tui.status,
             sizeof(ble_tui.status),
             "service %u; %u characteristic%s",
             (unsigned)ble_tui.chars_service_index,
             (unsigned)ble_tui.char_count,
             ble_tui.char_count == 1U ? "" : "s");
}

static void ble_tui_read_selected(void)
{
    if (ble_tui.chars_viewport.cursor >= ble_tui.char_count) {
        return;
    }
    const solar_os_ble_gatt_characteristic_t *characteristic =
        &ble_tui.chars[ble_tui.chars_viewport.cursor];
    if ((characteristic->properties & SOLAR_OS_BLE_CHAR_READ) == 0U) {
        ble_tui_set_status("characteristic is not readable");
        return;
    }
    ble_tui.value_len = 0U;
    const esp_err_t err = solar_os_ble_peer_read(ble_tui.session,
                                                 ble_tui.peer,
                                                 characteristic->handle,
                                                 ble_tui.value,
                                                 sizeof(ble_tui.value),
                                                 &ble_tui.value_len,
                                                 ble_tui.timeout_ms);
    if (err != ESP_OK) {
        ble_tui_set_error("read failed", err);
        return;
    }
    ble_tui.value_handle = characteristic->handle;
    ble_tui.value_view = true;
    ble_tui_set_status("read complete");
}

static void ble_tui_begin_write(void)
{
    if (ble_tui.chars_viewport.cursor >= ble_tui.char_count) {
        return;
    }
    const uint8_t properties = ble_tui.chars[ble_tui.chars_viewport.cursor].properties;
    if ((properties & (SOLAR_OS_BLE_CHAR_WRITE |
                       SOLAR_OS_BLE_CHAR_WRITE_NO_RESPONSE)) == 0U) {
        ble_tui_set_status("characteristic is not writable");
        return;
    }
    if ((properties & SOLAR_OS_BLE_CHAR_WRITE) == 0U) {
        ble_tui.write_with_response = false;
    } else if ((properties & SOLAR_OS_BLE_CHAR_WRITE_NO_RESPONSE) == 0U) {
        ble_tui.write_with_response = true;
    }
    ble_tui.write_text[0] = '\0';
    ble_tui.write_input = (solar_os_tui_input_state_t){0};
    ble_tui.editing_write = true;
    ble_tui_set_status(ble_tui.write_with_response ? "write with response" :
                                                      "write without response");
}

static bool ble_tui_parse_hex(const char *text,
                              uint8_t *value,
                              size_t capacity,
                              size_t *value_len)
{
    size_t count = 0U;
    const char *cursor = text;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count >= capacity || !isxdigit((unsigned char)cursor[0])) {
            return false;
        }
        char *end = NULL;
        const unsigned long parsed = strtoul(cursor, &end, 16);
        if (end == cursor || parsed > UINT8_MAX ||
            (*end != '\0' && !isspace((unsigned char)*end))) {
            return false;
        }
        value[count++] = (uint8_t)parsed;
        cursor = end;
    }
    if (count == 0U) {
        return false;
    }
    *value_len = count;
    return true;
}

static void ble_tui_submit_write(void)
{
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len = 0U;
    if (!ble_tui_parse_hex(ble_tui.write_text,
                           value,
                           sizeof(value),
                           &value_len)) {
        ble_tui_set_status("invalid hex bytes; use: 01 a0 ff");
        return;
    }
    const solar_os_ble_gatt_characteristic_t *characteristic =
        &ble_tui.chars[ble_tui.chars_viewport.cursor];
    const esp_err_t err = solar_os_ble_peer_write(ble_tui.session,
                                                  ble_tui.peer,
                                                  characteristic->handle,
                                                  value,
                                                  value_len,
                                                  ble_tui.write_with_response,
                                                  ble_tui.timeout_ms);
    if (err != ESP_OK) {
        ble_tui_set_error("write failed", err);
        return;
    }
    ble_tui.editing_write = false;
    memset(ble_tui.write_text, 0, sizeof(ble_tui.write_text));
    snprintf(ble_tui.status,
             sizeof(ble_tui.status),
             "wrote %u byte%s %s response",
             (unsigned)value_len,
             value_len == 1U ? "" : "s",
             ble_tui.write_with_response ? "with" : "without");
}

static void ble_tui_cycle_boot(int direction)
{
    int setting = (int)solar_os_ble_keyboard_boot_setting();
    setting = (setting + direction + 3) % 3;
    const esp_err_t err = solar_os_ble_keyboard_set_boot_setting(
        (solar_os_ble_keyboard_boot_setting_t)setting);
    if (err == ESP_OK) {
        ble_tui_set_status("boot preference saved; reboot to apply");
    } else {
        ble_tui_set_error("boot preference failed", err);
    }
}

static void ble_tui_change_setting(int direction, bool activate)
{
    const ble_tui_setting_t setting =
        (ble_tui_setting_t)ble_tui.settings_viewport.cursor;
    uint16_t rate = 0U;
    uint16_t delay = 0U;
    solar_os_ble_keyboard_get_repeat(&rate, &delay);
    esp_err_t err = ESP_OK;
    switch (setting) {
    case BLE_TUI_SETTING_BOOT:
        ble_tui_cycle_boot(direction != 0 ? direction : 1);
        return;
    case BLE_TUI_SETTING_KEEPALIVE:
        err = solar_os_ble_keyboard_set_keepalive_enabled(
            !solar_os_ble_keyboard_keepalive_enabled());
        break;
    case BLE_TUI_SETTING_LAYOUT: {
        const solar_os_ble_keyboard_layout_t layout =
            solar_os_ble_keyboard_layout() == SOLAR_OS_BLE_KEYBOARD_LAYOUT_US ?
                SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE : SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
        err = solar_os_ble_keyboard_set_layout(layout);
        break;
    }
    case BLE_TUI_SETTING_REPEAT_RATE: {
        int next = (int)rate + (direction != 0 ? direction : 1);
        if (next < SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN) {
            next = SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN;
        } else if (next > SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX) {
            next = SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX;
        }
        err = solar_os_ble_keyboard_set_repeat((uint16_t)next, delay);
        break;
    }
    case BLE_TUI_SETTING_REPEAT_DELAY: {
        int next = (int)delay + (direction != 0 ? direction * 50 : 50);
        if (next < SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS) {
            next = SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS;
        } else if (next > SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS) {
            next = SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS;
        }
        err = solar_os_ble_keyboard_set_repeat(rate, (uint16_t)next);
        break;
    }
    case BLE_TUI_SETTING_TIMEOUT:
        if (direction < 0 && ble_tui.timeout_ms > BLE_TUI_TIMEOUT_MIN_MS) {
            ble_tui.timeout_ms -= BLE_TUI_TIMEOUT_STEP_MS;
        } else if (direction >= 0 && ble_tui.timeout_ms < BLE_TUI_TIMEOUT_MAX_MS) {
            ble_tui.timeout_ms += BLE_TUI_TIMEOUT_STEP_MS;
        }
        ble_tui_set_status("timeout applies to this inspector session");
        return;
    case BLE_TUI_SETTING_WRITE_MODE:
        ble_tui.write_with_response = !ble_tui.write_with_response;
        ble_tui_set_status("default write mode changed");
        return;
    case BLE_TUI_SETTING_PAIR:
        if (activate) {
            if (!solar_os_ble_keyboard_enabled_for_current_boot()) {
                ble_tui_set_status("enable BLE, then reboot before pairing");
                return;
            }
            err = solar_os_ble_keyboard_start_pairing();
        } else {
            return;
        }
        break;
    case BLE_TUI_SETTING_FORGET:
        if (activate) {
            err = solar_os_ble_keyboard_forget();
        } else {
            return;
        }
        break;
    case BLE_TUI_SETTING_DISCONNECT:
        if (activate) {
            ble_tui_disconnect();
        }
        return;
    default:
        return;
    }
    if (err == ESP_OK) {
        if (setting == BLE_TUI_SETTING_PAIR) {
            ble_tui_set_status("keyboard pairing scan started");
        } else if (setting == BLE_TUI_SETTING_FORGET) {
            ble_tui_set_status("remembered keyboard forgotten");
        } else {
            ble_tui_set_status("setting saved");
        }
    } else {
        ble_tui_set_error("setting failed", err);
    }
}

static void ble_tui_handle_browse_key(uint8_t key)
{
    if (key == 'd' || key == 'D') {
        ble_tui_disconnect();
        return;
    }
    if (ble_tui.tab == BLE_TUI_TAB_DEVICES) {
        if (key == 'r' || key == 'R') {
            ble_tui_begin_scan();
        } else if (ble_tui_enter(key)) {
            ble_tui_begin_connect_selected();
        } else {
            (void)solar_os_tui_viewport_key(&ble_tui.devices_viewport,
                                             key,
                                             ble_tui.device_count,
                                             ble_tui_visible_rows(),
                                             false);
        }
        return;
    }
    if (ble_tui.tab == BLE_TUI_TAB_SERVICES) {
        if (ble_tui_enter(key)) {
            ble_tui_open_service();
        } else {
            (void)solar_os_tui_viewport_key(&ble_tui.services_viewport,
                                             key,
                                             ble_tui.service_count,
                                             ble_tui_visible_rows(),
                                             false);
        }
        return;
    }
    if (ble_tui.tab == BLE_TUI_TAB_CHARACTERISTICS) {
        if (ble_tui.value_view) {
            if (key == SOLAR_OS_KEY_ESCAPE) {
                ble_tui.value_view = false;
                ble_tui_set_status("");
            }
        } else if (ble_tui_enter(key)) {
            ble_tui_read_selected();
        } else if (key == 'w' || key == 'W') {
            ble_tui_begin_write();
        } else {
            (void)solar_os_tui_viewport_key(&ble_tui.chars_viewport,
                                             key,
                                             ble_tui.char_count,
                                             ble_tui_visible_rows(),
                                             false);
        }
        return;
    }
    if (key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN ||
        key == SOLAR_OS_KEY_PAGE_UP || key == SOLAR_OS_KEY_PAGE_DOWN ||
        key == SOLAR_OS_KEY_HOME || key == SOLAR_OS_KEY_END) {
        (void)solar_os_tui_viewport_key(&ble_tui.settings_viewport,
                                         key,
                                         BLE_TUI_SETTING_COUNT,
                                         ble_tui_visible_rows(),
                                         false);
    } else if (key == SOLAR_OS_KEY_LEFT) {
        ble_tui_change_setting(-1, false);
    } else if (key == SOLAR_OS_KEY_RIGHT) {
        ble_tui_change_setting(1, false);
    } else if (ble_tui_enter(key)) {
        ble_tui_change_setting(0, true);
    }
}

static esp_err_t ble_tui_start(solar_os_context_t *ctx)
{
    memset(&ble_tui, 0, sizeof(ble_tui));
    ble_tui.ctx = ctx;
    ble_tui.peer = SOLAR_OS_BLE_PEER_INVALID;
    ble_tui.timeout_ms = 10000U;
    ble_tui.write_with_response = true;
    esp_err_t err = solar_os_ble_session_create("ble.tui", &ble_tui.session);
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_tui_screen_begin(&ble_tui.tui, ctx);
    if (err != ESP_OK) {
        (void)solar_os_ble_session_close(ble_tui.session);
        ble_tui.session = SOLAR_OS_BLE_SESSION_INVALID;
        return err;
    }
    ble_tui_set_status(solar_os_ble_keyboard_enabled_for_current_boot() ?
                           "R scans; Enter connects" :
                           "BLE disabled this boot; use Settings");
    solar_os_tui_set_cursor_visible(&ble_tui.tui, false);
    ble_tui_render();
    return ESP_OK;
}

static void ble_tui_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&ble_tui.tui, true);
    solar_os_tui_refresh(&ble_tui.tui);
}

static void ble_tui_resume(solar_os_context_t *ctx)
{
    ble_tui.ctx = ctx;
    solar_os_tui_set_cursor_visible(&ble_tui.tui, false);
    ble_tui_render();
}

static void ble_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    ble_tui_release_peer();
    if (ble_tui.session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_close(ble_tui.session);
        ble_tui.session = SOLAR_OS_BLE_SESSION_INVALID;
    }
    memset(ble_tui.write_text, 0, sizeof(ble_tui.write_text));
    memset(ble_tui.value, 0, sizeof(ble_tui.value));
    solar_os_tui_set_cursor_visible(&ble_tui.tui, true);
    solar_os_tui_clear(&ble_tui.tui);
    solar_os_tui_refresh(&ble_tui.tui);
    solar_os_tui_end(&ble_tui.tui);
}

static bool ble_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        ble_tui_render();
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if (ble_tui.operation != BLE_TUI_OPERATION_NONE) {
            if (ble_tui.operation_not_before_ms == 0U) {
                ble_tui.operation_not_before_ms =
                    now_ms + BLE_TUI_PROGRESS_HOLD_MS;
                return true;
            }
            if ((int32_t)(now_ms - ble_tui.operation_not_before_ms) < 0) {
                return true;
            }
            if (ble_tui.operation == BLE_TUI_OPERATION_SCAN) {
                ble_tui_run_scan();
            } else {
                ble_tui_run_connect();
            }
            ble_tui.last_refresh_ms = now_ms;
            ble_tui_render();
            return true;
        }
        if (!ble_tui.editing_write &&
            (ble_tui.last_refresh_ms == 0U ||
             now_ms - ble_tui.last_refresh_ms >= BLE_TUI_REFRESH_MS)) {
            ble_tui.last_refresh_ms = now_ms;
            ble_tui_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (ble_tui.operation != BLE_TUI_OPERATION_NONE) {
        return true;
    }
    if (ble_tui.popup_active) {
        ble_tui.popup_active = false;
        ble_tui.popup_title[0] = '\0';
        ble_tui.popup_text[0] = '\0';
        ble_tui_render();
        return true;
    }
    if (ble_tui.editing_write) {
        const size_t width = solar_os_tui_cols(&ble_tui.tui);
        const solar_os_tui_input_action_t action = solar_os_tui_input_key(
            ble_tui.write_text,
            sizeof(ble_tui.write_text),
            &ble_tui.write_input,
            key,
            width > 5U ? width - 5U : 1U);
        if (action == SOLAR_OS_TUI_INPUT_CANCEL) {
            ble_tui.editing_write = false;
            memset(ble_tui.write_text, 0, sizeof(ble_tui.write_text));
            ble_tui_set_status("write cancelled");
        } else if (action == SOLAR_OS_TUI_INPUT_SUBMIT) {
            ble_tui_submit_write();
        } else if (action == SOLAR_OS_TUI_INPUT_CHANGED) {
            ble_tui_set_status(ble_tui.write_with_response ? "write with response" :
                                                              "write without response");
        }
        ble_tui_render();
        return true;
    }

    const solar_os_tui_screen_key_action_t screen_action =
        solar_os_tui_screen_key(&ble_tui.tui, key);
    if (screen_action == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED) {
        return true;
    }
    if (screen_action == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED) {
        ble_tui_render();
        return true;
    }
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(ble_tui.ctx, 0, NULL);
        return true;
    }
    if (key == SOLAR_OS_KEY_ESCAPE) {
        if (ble_tui.value_view) {
            ble_tui.value_view = false;
            ble_tui_set_status("");
            ble_tui_render();
        } else {
            solar_os_context_finish(ble_tui.ctx, 0, NULL);
        }
        return true;
    }
    if (key == '\t') {
        ble_tui.tab = (ble_tui_tab_t)(((size_t)ble_tui.tab + 1U) % BLE_TUI_TAB_COUNT);
        ble_tui.value_view = false;
        ble_tui_set_status("");
    } else {
        ble_tui_handle_browse_key(key);
    }
    ble_tui_render();
    return true;
}

static const solar_os_app_t ble_tui_app = {
    .name = "ble",
    .summary = "BLE inspector and settings",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = ble_tui_start,
    .suspend = ble_tui_suspend,
    .resume = ble_tui_resume,
    .stop = ble_tui_stop,
    .event = ble_tui_event,
    .state_slot = &ble_tui_state,
    .state_size = sizeof(ble_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};

esp_err_t solar_os_shell_launch_ble_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &ble_tui_app, 0, NULL);
}
