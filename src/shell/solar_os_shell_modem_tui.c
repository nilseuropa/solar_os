#include "solar_os_shell_tui_apps.h"
#include "solar_os_shell_common.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_modem.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define MODEM_TUI_REFRESH_MS 2000U
#define MODEM_TUI_MIN_ROWS 7U
#define MODEM_TUI_MIN_COLS 28U
#define MODEM_TUI_ROW_MAX 16U
#define MODEM_TUI_TEXT_MAX 112U
#define MODEM_TUI_STATUS_MAX 96U
#define MODEM_TUI_EDIT_MAX (SOLAR_OS_MODEM_APN_MAX + 1U)

typedef enum {
    MODEM_TUI_TAB_STATUS,
    MODEM_TUI_TAB_SETTINGS,
} modem_tui_tab_t;

typedef enum {
    MODEM_TUI_VIEW_MAIN,
    MODEM_TUI_VIEW_INPUT,
} modem_tui_view_t;

typedef enum {
    MODEM_TUI_INPUT_APN,
    MODEM_TUI_INPUT_DNS,
    MODEM_TUI_INPUT_USERNAME,
    MODEM_TUI_INPUT_PASSWORD,
} modem_tui_input_t;

typedef enum {
    MODEM_TUI_ITEM_DEVICE,
    MODEM_TUI_ITEM_POWER,
    MODEM_TUI_ITEM_RESET,
    MODEM_TUI_ITEM_DATA,
    MODEM_TUI_ITEM_APN,
    MODEM_TUI_ITEM_DNS,
    MODEM_TUI_ITEM_IP_TYPE,
    MODEM_TUI_ITEM_AUTH,
    MODEM_TUI_ITEM_USERNAME,
    MODEM_TUI_ITEM_PASSWORD,
    MODEM_TUI_ITEM_BAUD,
    MODEM_TUI_ITEM_SAVE_PROFILE,
    MODEM_TUI_ITEM_CLEAR_PROFILE,
    MODEM_TUI_ITEM_COUNT,
} modem_tui_item_t;

typedef struct {
    char text[MODEM_TUI_TEXT_MAX];
    uint8_t attr;
} modem_tui_row_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    modem_tui_tab_t tab;
    modem_tui_view_t view;
    modem_tui_input_t input;
    size_t device_index;
    size_t device_count;
    bool have_device;
    solar_os_modem_info_t device;
    char profile_device[SOLAR_OS_MODEM_NAME_MAX];
    solar_os_modem_profile_t profile;
    bool profile_exists;
    bool profile_dirty;
    solar_os_tui_viewport_t status_viewport;
    solar_os_tui_viewport_t settings_viewport;
    char edit[MODEM_TUI_EDIT_MAX];
    solar_os_tui_input_state_t edit_state;
    char status[MODEM_TUI_STATUS_MAX];
    uint32_t last_refresh_ms;
} modem_tui_state_t;

static void *modem_tui_state;
#define modem_tui (*(modem_tui_state_t *)modem_tui_state)

static void modem_tui_render(void);

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static void modem_tui_set_status(const char *status)
{
    strlcpy(modem_tui.status,
            status != NULL ? status : "",
            sizeof(modem_tui.status));
}

static void modem_tui_set_error(const char *operation, esp_err_t error)
{
    snprintf(modem_tui.status,
             sizeof(modem_tui.status),
             "%s: %s",
             operation,
             solar_os_shell_error_text(error));
}

static void modem_tui_profile_defaults(void)
{
    secure_zero(&modem_tui.profile, sizeof(modem_tui.profile));
    modem_tui.profile.ip_type = SOLAR_OS_MODEM_IP_IPV4V6;
    modem_tui.profile.auth = SOLAR_OS_MODEM_AUTH_NONE;
    modem_tui.profile_exists = false;
    modem_tui.profile_dirty = false;
}

static void modem_tui_load_profile(void)
{
    modem_tui_profile_defaults();
    if (!modem_tui.have_device) {
        modem_tui.profile_device[0] = '\0';
        return;
    }
    strlcpy(modem_tui.profile_device,
            modem_tui.device.name,
            sizeof(modem_tui.profile_device));
    solar_os_modem_profile_t profile = {0};
    const esp_err_t ret = solar_os_modem_profile_get(modem_tui.device.name,
                                                     &profile);
    if (ret == ESP_OK) {
        modem_tui.profile = profile;
        modem_tui.profile_exists = true;
    } else if (ret != ESP_ERR_NOT_FOUND) {
        modem_tui_set_error("profile load", ret);
    }
    secure_zero(&profile, sizeof(profile));
}

static void modem_tui_refresh_device(void)
{
    modem_tui.device_count = solar_os_modem_count();
    if (modem_tui.device_count == 0U) {
        modem_tui.have_device = false;
        memset(&modem_tui.device, 0, sizeof(modem_tui.device));
        if (modem_tui.profile_device[0] != '\0') {
            modem_tui_load_profile();
        }
        return;
    }
    if (modem_tui.device_index >= modem_tui.device_count) {
        modem_tui.device_index = modem_tui.device_count - 1U;
    }
    solar_os_modem_info_t device = {0};
    if (!solar_os_modem_get(modem_tui.device_index, &device)) {
        modem_tui.have_device = false;
        return;
    }
    const bool changed = !modem_tui.have_device ||
        strcmp(modem_tui.device.name, device.name) != 0;
    modem_tui.device = device;
    modem_tui.have_device = true;
    if (changed || strcmp(modem_tui.profile_device, device.name) != 0) {
        modem_tui_load_profile();
    }
}

static void modem_tui_select_device(int direction)
{
    modem_tui_refresh_device();
    if (modem_tui.device_count < 2U) {
        modem_tui_set_status(modem_tui.device_count == 0U ?
                             "no modem registered" : "only one modem");
        return;
    }
    const bool discarded_changes = modem_tui.profile_dirty;
    if (direction < 0) {
        modem_tui.device_index = modem_tui.device_index == 0U ?
            modem_tui.device_count - 1U : modem_tui.device_index - 1U;
    } else {
        modem_tui.device_index =
            (modem_tui.device_index + 1U) % modem_tui.device_count;
    }
    modem_tui.have_device = false;
    modem_tui_refresh_device();
    modem_tui.status_viewport = (solar_os_tui_viewport_t){0};
    modem_tui.settings_viewport = (solar_os_tui_viewport_t){0};
    modem_tui_set_status(discarded_changes ?
                         "unsaved profile changes discarded" : "");
}

static void modem_tui_add_row(modem_tui_row_t *rows,
                              size_t max_rows,
                              size_t *count,
                              uint8_t attr,
                              const char *format,
                              ...)
{
    if (rows == NULL || count == NULL || *count >= max_rows || format == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)vsnprintf(rows[*count].text,
                    sizeof(rows[*count].text),
                    format,
                    args);
    va_end(args);
    rows[*count].attr = attr;
    (*count)++;
}

static size_t modem_tui_status_rows(modem_tui_row_t *rows, size_t max_rows)
{
    size_t count = 0U;
    if (!modem_tui.have_device) {
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "No modem registered");
        return count;
    }

    modem_tui_add_row(rows, max_rows, &count,
                      SOLAR_OS_TUI_ATTR_BOLD, "Device");
    modem_tui_add_row(rows, max_rows, &count,
                      SOLAR_OS_TUI_ATTR_NORMAL,
                      "  driver: %s", modem_tui.device.driver);
    modem_tui_add_row(rows, max_rows, &count,
                      SOLAR_OS_TUI_ATTR_NORMAL,
                      "  transport: %s", modem_tui.device.transport);

    solar_os_modem_status_t status = {0};
    const esp_err_t status_ret = solar_os_modem_get_status(
        modem_tui.device.name, &status);
    if (status_ret != ESP_OK) {
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "  status: %s", solar_os_shell_error_text(status_ret));
    } else {
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "  power: %s  online: %s",
                          status.power_control ?
                              (status.powered ? "on" : "off") : "always-on",
                          status.online ? "yes" : "no");
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "  SIM: %s",
                          status.sim_status_valid ?
                              (status.sim_ready ? "ready" : "not ready") :
                              "unknown");
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "  registration: %s",
                          status.registration_status_valid ?
                              solar_os_modem_registration_name(
                                  status.registration) : "unknown");
        if (status.signal_status_valid && status.rssi_valid) {
            if (status.bit_error_rate == 99U) {
                modem_tui_add_row(rows, max_rows, &count,
                                  SOLAR_OS_TUI_ATTR_NORMAL,
                                  "  signal: %d dBm  BER: unknown",
                                  (int)status.rssi_dbm);
            } else {
                modem_tui_add_row(rows, max_rows, &count,
                                  SOLAR_OS_TUI_ATTR_NORMAL,
                                  "  signal: %d dBm  BER: %u",
                                  (int)status.rssi_dbm,
                                  (unsigned)status.bit_error_rate);
            }
        } else {
            modem_tui_add_row(rows, max_rows, &count,
                              SOLAR_OS_TUI_ATTR_NORMAL,
                              "  signal: unknown");
        }
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "  data: %s  network: %s",
                          status.data_status_valid ?
                              (status.data_active ? "active" : "inactive") :
                              "unknown",
                          status.network_status_valid ?
                              solar_os_modem_network_state_name(
                                  status.network_state) : "unknown");
        if (status.ipv4_address[0] != '\0') {
            modem_tui_add_row(rows, max_rows, &count,
                              SOLAR_OS_TUI_ATTR_NORMAL,
                              "  %s: %s",
                              status.network_interface[0] != '\0' ?
                                  status.network_interface : "IPv4",
                              status.ipv4_address);
        }
    }

    solar_os_modem_transport_rate_info_t rate = {0};
    if (solar_os_modem_transport_rate_get(modem_tui.device.name, &rate) ==
        ESP_OK) {
        modem_tui_add_row(rows, max_rows, &count,
                          SOLAR_OS_TUI_ATTR_NORMAL,
                          "  baud: %u (%s)",
                          (unsigned)rate.active_rate,
                          rate.automatic ? "auto" : "fixed");
    }
    return count;
}

static void modem_tui_draw_tabs(const solar_os_tui_screen_layout_t *layout)
{
    static const char status_label[] = " Status ";
    static const char settings_label[] = " Settings ";
    solar_os_tui_write_cell(&modem_tui.tui,
                            layout->tabs.row,
                            0U,
                            layout->tabs.width,
                            "",
                            SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_draw_tab(&modem_tui.tui,
                          layout->tabs.row,
                          0U,
                          sizeof(status_label) - 1U,
                          status_label,
                          modem_tui.tab == MODEM_TUI_TAB_STATUS);
    solar_os_tui_draw_tab(&modem_tui.tui,
                          layout->tabs.row,
                          sizeof(status_label) - 1U,
                          sizeof(settings_label) - 1U,
                          settings_label,
                          modem_tui.tab == MODEM_TUI_TAB_SETTINGS);
}

static void modem_tui_draw_status(const solar_os_tui_screen_layout_t *layout)
{
    modem_tui_row_t rows[MODEM_TUI_ROW_MAX];
    const size_t count = modem_tui_status_rows(
        rows, sizeof(rows) / sizeof(rows[0]));
    solar_os_tui_viewport_reconcile(&modem_tui.status_viewport,
                                    count,
                                    layout->body.height);
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = modem_tui.status_viewport.top + row;
        solar_os_tui_write_cell(&modem_tui.tui,
                                layout->body.row + row,
                                0U,
                                layout->body.width,
                                index < count ? rows[index].text : "",
                                index < count ? rows[index].attr :
                                                SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void modem_tui_network_value(char *value, size_t value_len)
{
    solar_os_modem_status_t status = {0};
    const esp_err_t ret = solar_os_modem_get_status(modem_tui.device.name,
                                                    &status);
    if (ret != ESP_OK) {
        strlcpy(value, solar_os_shell_error_text(ret), value_len);
    } else if (status.network_status_valid) {
        strlcpy(value,
                solar_os_modem_network_state_name(status.network_state),
                value_len);
    } else if (status.data_status_valid) {
        strlcpy(value, status.data_active ? "active" : "inactive", value_len);
    } else {
        strlcpy(value, "unknown", value_len);
    }
}

static void modem_tui_setting_line(modem_tui_item_t item,
                                   char *line,
                                   size_t line_len)
{
    if (!modem_tui.have_device) {
        strlcpy(line, "no modem registered", line_len);
        return;
    }
    switch (item) {
    case MODEM_TUI_ITEM_DEVICE:
        snprintf(line, line_len, "device       %s (%u/%u)",
                 modem_tui.device.name,
                 (unsigned)(modem_tui.device_index + 1U),
                 (unsigned)modem_tui.device_count);
        break;
    case MODEM_TUI_ITEM_POWER:
        snprintf(line, line_len, "power        %s",
                 modem_tui.device.power_control ?
                     (modem_tui.device.powered ? "on" : "off") :
                     "always-on");
        break;
    case MODEM_TUI_ITEM_RESET:
        snprintf(line, line_len, "reset        %s",
                 modem_tui.device.reset_control ? "enter" : "unavailable");
        break;
    case MODEM_TUI_ITEM_DATA: {
        char value[32];
        modem_tui_network_value(value, sizeof(value));
        snprintf(line, line_len, "connection   %s", value);
        break;
    }
    case MODEM_TUI_ITEM_APN:
        snprintf(line, line_len, "APN          %s",
                 modem_tui.profile.apn[0] != '\0' ?
                     modem_tui.profile.apn : "unset");
        break;
    case MODEM_TUI_ITEM_DNS:
        snprintf(line, line_len, "DNS          %s",
                 modem_tui.profile.dns[0] != '\0' ?
                     modem_tui.profile.dns : "auto");
        break;
    case MODEM_TUI_ITEM_IP_TYPE:
        snprintf(line, line_len, "IP type      %s",
                 solar_os_modem_ip_type_name(modem_tui.profile.ip_type));
        break;
    case MODEM_TUI_ITEM_AUTH:
        snprintf(line, line_len, "auth         %s",
                 solar_os_modem_auth_name(modem_tui.profile.auth));
        break;
    case MODEM_TUI_ITEM_USERNAME:
        snprintf(line, line_len, "username     %s",
                 modem_tui.profile.username[0] != '\0' ?
                     modem_tui.profile.username : "none");
        break;
    case MODEM_TUI_ITEM_PASSWORD:
        snprintf(line, line_len, "password     %s",
                 modem_tui.profile.password[0] != '\0' ? "set" : "none");
        break;
    case MODEM_TUI_ITEM_BAUD: {
        solar_os_modem_transport_rate_info_t rate = {0};
        const esp_err_t ret = solar_os_modem_transport_rate_get(
            modem_tui.device.name, &rate);
        if (ret == ESP_OK) {
            if (rate.automatic) {
                snprintf(line, line_len, "baud         auto (%u)",
                         (unsigned)rate.active_rate);
            } else {
                snprintf(line, line_len, "baud         %u",
                         (unsigned)rate.configured_rate);
            }
        } else {
            strlcpy(line, "baud         unavailable", line_len);
        }
        break;
    }
    case MODEM_TUI_ITEM_SAVE_PROFILE:
        snprintf(line, line_len, "save profile %s",
                 modem_tui.profile_dirty ? "* changed" :
                 (modem_tui.profile_exists ? "saved" : "unset"));
        break;
    case MODEM_TUI_ITEM_CLEAR_PROFILE:
        snprintf(line, line_len, "clear profile %s",
                 modem_tui.profile_exists ? "enter" : "none");
        break;
    default:
        line[0] = '\0';
        break;
    }
}

static void modem_tui_draw_settings(const solar_os_tui_screen_layout_t *layout)
{
    const size_t item_count = modem_tui.have_device ?
        MODEM_TUI_ITEM_COUNT : 1U;
    solar_os_tui_viewport_reconcile(&modem_tui.settings_viewport,
                                    item_count,
                                    layout->body.height);
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = modem_tui.settings_viewport.top + row;
        char line[MODEM_TUI_TEXT_MAX] = "";
        if (index < item_count) {
            modem_tui_setting_line((modem_tui_item_t)index,
                                   line,
                                   sizeof(line));
        }
        solar_os_tui_write_cell(
            &modem_tui.tui,
            layout->body.row + row,
            0U,
            layout->body.width,
            line,
            index < item_count && index == modem_tui.settings_viewport.cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void modem_tui_render_input(void)
{
    solar_os_tui_t *tui = &modem_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    if (rows == 0U || cols == 0U) {
        return;
    }
    const char *label = "value: ";
    const char *title = "Modem setting";
    bool masked = false;
    switch (modem_tui.input) {
    case MODEM_TUI_INPUT_APN:
        label = "APN: ";
        title = "Modem APN";
        break;
    case MODEM_TUI_INPUT_DNS:
        label = "DNS: ";
        title = "Modem DNS";
        break;
    case MODEM_TUI_INPUT_USERNAME:
        label = "username: ";
        title = "Modem username";
        break;
    case MODEM_TUI_INPUT_PASSWORD:
        label = "password: ";
        title = "Modem password";
        masked = true;
        break;
    }
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, title,
                            modem_tui.have_device ? modem_tui.device.name : "");
    if (rows > 1U) {
        const size_t content_end = solar_os_tui_screen_content_end(tui, 1U);
        const size_t input_row = content_end > 1U ? content_end - 1U : 1U;
        solar_os_tui_draw_footer(tui,
                                 modem_tui.status,
                                 "ENTER accepts  ESC cancels");
        solar_os_tui_draw_input_ex(tui,
                                   input_row,
                                   0U,
                                   cols,
                                   label,
                                   modem_tui.edit,
                                   &modem_tui.edit_state,
                                   SOLAR_OS_TUI_ATTR_INVERSE,
                                   masked);
        solar_os_tui_set_cursor_visible(tui, true);
    }
    solar_os_tui_refresh(tui);
}

static void modem_tui_render(void)
{
    if (modem_tui.view == MODEM_TUI_VIEW_INPUT) {
        modem_tui_render_input();
        return;
    }
    modem_tui_refresh_device();
    const size_t rows = solar_os_tui_rows(&modem_tui.tui);
    const size_t cols = solar_os_tui_cols(&modem_tui.tui);
    if (rows < MODEM_TUI_MIN_ROWS || cols < MODEM_TUI_MIN_COLS) {
        solar_os_tui_draw_too_small(&modem_tui.tui, "modem");
        solar_os_tui_refresh(&modem_tui.tui);
        return;
    }
    solar_os_tui_screen_layout_t layout;
    if (!solar_os_tui_screen_layout(&modem_tui.tui, 1U, 0U, 0U, &layout)) {
        solar_os_tui_draw_too_small(&modem_tui.tui, "modem");
        solar_os_tui_refresh(&modem_tui.tui);
        return;
    }
    char detail[40];
    if (modem_tui.have_device) {
        snprintf(detail, sizeof(detail), "%s %u/%u",
                 modem_tui.device.name,
                 (unsigned)(modem_tui.device_index + 1U),
                 (unsigned)modem_tui.device_count);
    } else {
        strlcpy(detail, "no device", sizeof(detail));
    }
    solar_os_tui_clear(&modem_tui.tui);
    solar_os_tui_draw_title(&modem_tui.tui, "Modem", detail);
    modem_tui_draw_tabs(&layout);
    if (modem_tui.tab == MODEM_TUI_TAB_STATUS) {
        modem_tui_draw_status(&layout);
    } else {
        modem_tui_draw_settings(&layout);
    }
    solar_os_tui_draw_footer(
        &modem_tui.tui,
        modem_tui.status,
        modem_tui.tab == MODEM_TUI_TAB_STATUS ?
            "TAB settings  LEFT/RIGHT device  ESC exit" :
            "TAB status  arrows select/change  ENTER acts  ESC exit");
    solar_os_tui_set_cursor_visible(&modem_tui.tui, false);
    solar_os_tui_refresh(&modem_tui.tui);
}

static void modem_tui_begin_input(modem_tui_input_t input)
{
    secure_zero(modem_tui.edit, sizeof(modem_tui.edit));
    modem_tui.input = input;
    switch (input) {
    case MODEM_TUI_INPUT_APN:
        strlcpy(modem_tui.edit,
                modem_tui.profile.apn,
                sizeof(modem_tui.edit));
        break;
    case MODEM_TUI_INPUT_DNS:
        strlcpy(modem_tui.edit,
                modem_tui.profile.dns,
                sizeof(modem_tui.edit));
        break;
    case MODEM_TUI_INPUT_USERNAME:
        strlcpy(modem_tui.edit,
                modem_tui.profile.username,
                sizeof(modem_tui.edit));
        break;
    case MODEM_TUI_INPUT_PASSWORD:
        break;
    }
    modem_tui.edit_state = (solar_os_tui_input_state_t) {
        .cursor = strlen(modem_tui.edit),
    };
    modem_tui.view = MODEM_TUI_VIEW_INPUT;
    modem_tui_set_status(input == MODEM_TUI_INPUT_PASSWORD ?
                         "empty input clears the password" : "");
}

static size_t modem_tui_input_capacity(void)
{
    switch (modem_tui.input) {
    case MODEM_TUI_INPUT_APN:
        return SOLAR_OS_MODEM_APN_MAX + 1U;
    case MODEM_TUI_INPUT_DNS:
        return SOLAR_OS_MODEM_DNS_MAX + 1U;
    case MODEM_TUI_INPUT_USERNAME:
        return SOLAR_OS_MODEM_USERNAME_MAX + 1U;
    case MODEM_TUI_INPUT_PASSWORD:
        return SOLAR_OS_MODEM_PASSWORD_MAX + 1U;
    default:
        return sizeof(modem_tui.edit);
    }
}

static void modem_tui_accept_input(void)
{
    switch (modem_tui.input) {
    case MODEM_TUI_INPUT_APN:
        strlcpy(modem_tui.profile.apn,
                modem_tui.edit,
                sizeof(modem_tui.profile.apn));
        break;
    case MODEM_TUI_INPUT_DNS:
        strlcpy(modem_tui.profile.dns,
                modem_tui.edit,
                sizeof(modem_tui.profile.dns));
        break;
    case MODEM_TUI_INPUT_USERNAME:
        strlcpy(modem_tui.profile.username,
                modem_tui.edit,
                sizeof(modem_tui.profile.username));
        break;
    case MODEM_TUI_INPUT_PASSWORD:
        secure_zero(modem_tui.profile.password,
                    sizeof(modem_tui.profile.password));
        strlcpy(modem_tui.profile.password,
                modem_tui.edit,
                sizeof(modem_tui.profile.password));
        break;
    }
    secure_zero(modem_tui.edit, sizeof(modem_tui.edit));
    modem_tui.edit_state = (solar_os_tui_input_state_t){0};
    modem_tui.profile_dirty = true;
    modem_tui.view = MODEM_TUI_VIEW_MAIN;
    modem_tui_set_status("profile changed; select save profile");
}

static void modem_tui_cycle_ip_type(int direction)
{
    int value = (int)modem_tui.profile.ip_type + direction;
    if (value < (int)SOLAR_OS_MODEM_IP_IPV4) {
        value = (int)SOLAR_OS_MODEM_IP_IPV4V6;
    } else if (value > (int)SOLAR_OS_MODEM_IP_IPV4V6) {
        value = (int)SOLAR_OS_MODEM_IP_IPV4;
    }
    modem_tui.profile.ip_type = (solar_os_modem_ip_type_t)value;
    modem_tui.profile_dirty = true;
    modem_tui_set_status("profile changed; select save profile");
}

static void modem_tui_cycle_auth(int direction)
{
    int value = (int)modem_tui.profile.auth + direction;
    if (value < (int)SOLAR_OS_MODEM_AUTH_NONE) {
        value = (int)SOLAR_OS_MODEM_AUTH_AUTO;
    } else if (value > (int)SOLAR_OS_MODEM_AUTH_AUTO) {
        value = (int)SOLAR_OS_MODEM_AUTH_NONE;
    }
    modem_tui.profile.auth = (solar_os_modem_auth_t)value;
    if (modem_tui.profile.auth == SOLAR_OS_MODEM_AUTH_NONE ||
        modem_tui.profile.auth == SOLAR_OS_MODEM_AUTH_AUTO) {
        secure_zero(modem_tui.profile.username,
                    sizeof(modem_tui.profile.username));
        secure_zero(modem_tui.profile.password,
                    sizeof(modem_tui.profile.password));
    }
    modem_tui.profile_dirty = true;
    modem_tui_set_status("profile changed; select save profile");
}

static void modem_tui_cycle_baud(int direction)
{
    solar_os_modem_transport_rate_info_t rate = {0};
    esp_err_t ret = solar_os_modem_transport_rate_get(modem_tui.device.name,
                                                      &rate);
    if (ret != ESP_OK) {
        modem_tui_set_error("baud", ret);
        return;
    }
    size_t index = 0U;
    if (!rate.automatic) {
        for (size_t i = 0U; i < rate.supported_rate_count; i++) {
            if (rate.supported_rates[i] == rate.configured_rate) {
                index = i + 1U;
                break;
            }
        }
    }
    const size_t choice_count = rate.supported_rate_count + 1U;
    if (direction < 0) {
        index = index == 0U ? choice_count - 1U : index - 1U;
    } else {
        index = (index + 1U) % choice_count;
    }
    const uint32_t selected_rate = index == 0U ? 0U :
        rate.supported_rates[index - 1U];
    ret = solar_os_modem_transport_rate_set(modem_tui.device.name,
                                            selected_rate);
    if (ret == ESP_OK) {
        if (selected_rate == 0U) {
            modem_tui_set_status("baud set to auto");
        } else {
            snprintf(modem_tui.status,
                     sizeof(modem_tui.status),
                     "baud set to %u",
                     (unsigned)selected_rate);
        }
    } else {
        modem_tui_set_error("baud", ret);
    }
}

static void modem_tui_set_power(bool enable)
{
    if (!modem_tui.device.power_control) {
        modem_tui_set_status("power control unavailable");
        return;
    }
    if (enable == modem_tui.device.powered) {
        modem_tui_set_status(enable ? "modem already on" : "modem already off");
        return;
    }
    modem_tui_set_status(enable ? "powering modem on..." :
                                  "powering modem off...");
    modem_tui_render();
    const esp_err_t ret = solar_os_modem_set_power(modem_tui.device.name,
                                                   enable);
    if (ret == ESP_OK) {
        modem_tui_set_status(enable ? "modem powered on" : "modem powered off");
        modem_tui_refresh_device();
    } else {
        modem_tui_set_error("power", ret);
    }
}

static void modem_tui_reset(void)
{
    if (!modem_tui.device.reset_control) {
        modem_tui_set_status("reset unavailable");
        return;
    }
    modem_tui_set_status("resetting modem...");
    modem_tui_render();
    const esp_err_t ret = solar_os_modem_reset(modem_tui.device.name);
    if (ret == ESP_OK) {
        modem_tui_set_status("modem reset complete");
    } else {
        modem_tui_set_error("reset", ret);
    }
}

static void modem_tui_set_data(int requested_state)
{
    solar_os_modem_status_t status = {0};
    const esp_err_t status_ret = solar_os_modem_get_status(
        modem_tui.device.name, &status);
    if (status_ret != ESP_OK) {
        modem_tui_set_error("status", status_ret);
        return;
    }
    const bool active =
        (status.network_status_valid &&
         (status.network_state == SOLAR_OS_MODEM_NETWORK_UP ||
          status.network_state == SOLAR_OS_MODEM_NETWORK_CONNECTING)) ||
        (status.data_status_valid && status.data_active);
    const bool enable = requested_state < 0 ? false :
        requested_state > 0 ? true : !active;
    if (enable == active) {
        modem_tui_set_status(enable ? "connection already active" :
                                      "connection already down");
        return;
    }
    modem_tui_set_status(enable ? "connecting..." : "disconnecting...");
    modem_tui_render();
    const esp_err_t ret = solar_os_modem_set_data_active(modem_tui.device.name,
                                                        enable);
    if (ret == ESP_OK) {
        modem_tui_set_status(enable ? "connected" : "disconnected");
    } else if (ret == ESP_ERR_NOT_FOUND && enable) {
        modem_tui_set_status("save an APN profile before connecting");
    } else {
        modem_tui_set_error(enable ? "connect" : "disconnect", ret);
    }
}

static void modem_tui_save_profile(void)
{
    const esp_err_t ret = solar_os_modem_profile_set(modem_tui.device.name,
                                                     &modem_tui.profile);
    if (ret == ESP_OK) {
        modem_tui.profile_exists = true;
        modem_tui.profile_dirty = false;
        modem_tui_set_status("profile saved and applied");
    } else if (ret == ESP_ERR_INVALID_ARG) {
        modem_tui_set_status("invalid profile; check APN, DNS, auth, credentials");
    } else {
        modem_tui_set_error("profile save", ret);
    }
}

static void modem_tui_clear_profile(void)
{
    if (!modem_tui.profile_exists) {
        modem_tui_set_status("no saved profile");
        return;
    }
    const esp_err_t ret = solar_os_modem_profile_clear(modem_tui.device.name);
    if (ret == ESP_OK) {
        modem_tui_profile_defaults();
        modem_tui_set_status("profile cleared");
    } else {
        modem_tui_set_error("profile clear", ret);
    }
}

static void modem_tui_apply_item(modem_tui_item_t item, int direction)
{
    if (!modem_tui.have_device) {
        modem_tui_set_status("no modem registered");
        return;
    }
    const int step = direction == 0 ? 1 : direction;
    switch (item) {
    case MODEM_TUI_ITEM_DEVICE:
        modem_tui_select_device(step);
        break;
    case MODEM_TUI_ITEM_POWER:
        modem_tui_set_power(direction < 0 ? false :
                            direction > 0 ? true :
                            !modem_tui.device.powered);
        break;
    case MODEM_TUI_ITEM_RESET:
        if (direction == 0) {
            modem_tui_reset();
        }
        break;
    case MODEM_TUI_ITEM_DATA:
        modem_tui_set_data(direction);
        break;
    case MODEM_TUI_ITEM_APN:
        if (direction == 0) {
            modem_tui_begin_input(MODEM_TUI_INPUT_APN);
        }
        break;
    case MODEM_TUI_ITEM_DNS:
        if (direction == 0) {
            modem_tui_begin_input(MODEM_TUI_INPUT_DNS);
        }
        break;
    case MODEM_TUI_ITEM_IP_TYPE:
        modem_tui_cycle_ip_type(step);
        break;
    case MODEM_TUI_ITEM_AUTH:
        modem_tui_cycle_auth(step);
        break;
    case MODEM_TUI_ITEM_USERNAME:
        if (direction == 0) {
            modem_tui_begin_input(MODEM_TUI_INPUT_USERNAME);
        }
        break;
    case MODEM_TUI_ITEM_PASSWORD:
        if (direction == 0) {
            modem_tui_begin_input(MODEM_TUI_INPUT_PASSWORD);
        }
        break;
    case MODEM_TUI_ITEM_BAUD:
        modem_tui_cycle_baud(step);
        break;
    case MODEM_TUI_ITEM_SAVE_PROFILE:
        if (direction == 0) {
            modem_tui_save_profile();
        }
        break;
    case MODEM_TUI_ITEM_CLEAR_PROFILE:
        if (direction == 0) {
            modem_tui_clear_profile();
        }
        break;
    default:
        break;
    }
}

static esp_err_t modem_tui_start(solar_os_context_t *ctx)
{
    memset(&modem_tui, 0, sizeof(modem_tui));
    modem_tui.ctx = ctx;
    const esp_err_t ret = solar_os_tui_screen_begin(&modem_tui.tui, ctx);
    if (ret != ESP_OK) {
        return ret;
    }
    modem_tui_profile_defaults();
    modem_tui_refresh_device();
    solar_os_tui_set_cursor_visible(&modem_tui.tui, false);
    modem_tui_render();
    return ESP_OK;
}

static void modem_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    secure_zero(&modem_tui.profile, sizeof(modem_tui.profile));
    secure_zero(modem_tui.edit, sizeof(modem_tui.edit));
    solar_os_tui_set_cursor_visible(&modem_tui.tui, true);
    solar_os_tui_clear(&modem_tui.tui);
    solar_os_tui_refresh(&modem_tui.tui);
    solar_os_tui_end(&modem_tui.tui);
}

static bool modem_tui_input_event(uint8_t key)
{
    const size_t cols = solar_os_tui_cols(&modem_tui.tui);
    const char *label = modem_tui.input == MODEM_TUI_INPUT_APN ? "APN: " :
        modem_tui.input == MODEM_TUI_INPUT_DNS ? "DNS: " :
        modem_tui.input == MODEM_TUI_INPUT_USERNAME ? "username: " :
                                                       "password: ";
    const size_t label_width = strlen(label);
    const solar_os_tui_input_action_t action = solar_os_tui_input_key(
        modem_tui.edit,
        modem_tui_input_capacity(),
        &modem_tui.edit_state,
        key,
        cols > label_width ? cols - label_width : 1U);
    if (action == SOLAR_OS_TUI_INPUT_CANCEL) {
        secure_zero(modem_tui.edit, sizeof(modem_tui.edit));
        modem_tui.edit_state = (solar_os_tui_input_state_t){0};
        modem_tui.view = MODEM_TUI_VIEW_MAIN;
        modem_tui_set_status("");
    } else if (action == SOLAR_OS_TUI_INPUT_SUBMIT) {
        modem_tui_accept_input();
    } else if (action == SOLAR_OS_TUI_INPUT_CHANGED) {
        modem_tui_set_status("");
    }
    modem_tui_render();
    return true;
}

static bool modem_tui_event(solar_os_context_t *ctx,
                            const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        modem_tui_render();
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (modem_tui.view == MODEM_TUI_VIEW_MAIN &&
            (modem_tui.last_refresh_ms == 0U ||
             event->data.tick_ms - modem_tui.last_refresh_ms >=
                 MODEM_TUI_REFRESH_MS)) {
            modem_tui.last_refresh_ms = event->data.tick_ms;
            modem_tui_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }
    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(modem_tui.ctx, 0, NULL);
        return true;
    }
    if (modem_tui.view == MODEM_TUI_VIEW_INPUT) {
        return modem_tui_input_event(key);
    }
    if (key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_finish(modem_tui.ctx, 0, NULL);
        return true;
    }
    if (key == '\t') {
        modem_tui.tab = modem_tui.tab == MODEM_TUI_TAB_STATUS ?
            MODEM_TUI_TAB_SETTINGS : MODEM_TUI_TAB_STATUS;
        modem_tui_set_status("");
        modem_tui_render();
        return true;
    }

    if (modem_tui.tab == MODEM_TUI_TAB_STATUS) {
        if (key == SOLAR_OS_KEY_LEFT) {
            modem_tui_select_device(-1);
        } else if (key == SOLAR_OS_KEY_RIGHT) {
            modem_tui_select_device(1);
        } else {
            modem_tui_row_t rows[MODEM_TUI_ROW_MAX];
            const size_t count = modem_tui_status_rows(
                rows, sizeof(rows) / sizeof(rows[0]));
            const size_t visible = solar_os_tui_screen_content_rows(
                &modem_tui.tui, 2U, 1U);
            (void)solar_os_tui_viewport_key(&modem_tui.status_viewport,
                                            key,
                                            count,
                                            visible,
                                            false);
        }
    } else {
        const size_t count = modem_tui.have_device ?
            MODEM_TUI_ITEM_COUNT : 1U;
        const size_t visible = solar_os_tui_screen_content_rows(
            &modem_tui.tui, 2U, 1U);
        if (key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN) {
            (void)solar_os_tui_viewport_key(&modem_tui.settings_viewport,
                                            key,
                                            count,
                                            visible,
                                            false);
        } else if (key == SOLAR_OS_KEY_LEFT || key == SOLAR_OS_KEY_RIGHT ||
                   key == SOLAR_OS_KEY_ENTER || key == '\r' || key == '\n') {
            solar_os_tui_viewport_reconcile(&modem_tui.settings_viewport,
                                            count,
                                            visible);
            modem_tui_apply_item(
                (modem_tui_item_t)modem_tui.settings_viewport.cursor,
                key == SOLAR_OS_KEY_LEFT ? -1 :
                key == SOLAR_OS_KEY_RIGHT ? 1 : 0);
        }
    }
    modem_tui_render();
    return true;
}

static const solar_os_app_t modem_tui_app = {
    .name = "modem",
    .summary = "Cellular modem control",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .start = modem_tui_start,
    .stop = modem_tui_stop,
    .event = modem_tui_event,
    .state_slot = &modem_tui_state,
    .state_size = sizeof(modem_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};

esp_err_t solar_os_shell_launch_modem_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &modem_tui_app, 0, NULL);
}
