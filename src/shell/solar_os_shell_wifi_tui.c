#include "solar_os_shell_tui_apps.h"
#include "solar_os_shell_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"
#include "solar_os_wifi.h"

#define WIFI_TUI_STATUS_MAX 96
#define WIFI_TUI_REFRESH_MS 1000
#define WIFI_TUI_SSID_LABEL "ssid: "
#define WIFI_TUI_PASSWORD_LABEL "password: "

typedef enum {
    WIFI_TUI_VIEW_MAIN,
    WIFI_TUI_VIEW_SCANNING,
    WIFI_TUI_VIEW_SCAN,
    WIFI_TUI_VIEW_STATION_PASSWORD,
    WIFI_TUI_VIEW_SAVED_STATIONS,
    WIFI_TUI_VIEW_SAVED_APS,
    WIFI_TUI_VIEW_AP_SSID,
    WIFI_TUI_VIEW_AP_PASSWORD,
} wifi_tui_view_t;

typedef enum {
    WIFI_TUI_RADIO,
    WIFI_TUI_STATION,
    WIFI_TUI_DISCONNECT,
    WIFI_TUI_REPEATER,
    WIFI_TUI_AP,
    WIFI_TUI_SHARE,
    WIFI_TUI_SCAN,
    WIFI_TUI_SAVED_STA,
    WIFI_TUI_SAVED_AP,
    WIFI_TUI_ITEM_COUNT,
} wifi_tui_item_t;

typedef struct {
    const char *label;
} wifi_tui_item_def_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    wifi_tui_view_t view;
    size_t selected;
    char status[WIFI_TUI_STATUS_MAX];
    solar_os_wifi_ap_t scan_aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t scan_count;
    bool scan_valid;
    solar_os_tui_viewport_t scan_viewport;
    char password[SOLAR_OS_WIFI_PASSWORD_MAX];
    solar_os_tui_input_state_t password_input;
    solar_os_wifi_profile_t saved_stations[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t saved_station_count;
    solar_os_tui_viewport_t saved_station_viewport;
    solar_os_wifi_ap_config_t saved_ap;
    bool has_saved_ap;
    bool confirming_forget;
    bool editing_ap;
    char ap_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char ap_password[SOLAR_OS_WIFI_PASSWORD_MAX];
    char ap_auth[SOLAR_OS_WIFI_AUTH_MAX];
    solar_os_tui_input_state_t ap_input;
    uint32_t last_refresh_ms;
} wifi_tui_state_t;

static void *wifi_tui_state;
#define wifi_tui (*(wifi_tui_state_t *)wifi_tui_state)

static const wifi_tui_item_def_t wifi_tui_items[] = {
    [WIFI_TUI_RADIO] = {.label = "radio"},
    [WIFI_TUI_STATION] = {.label = "station"},
    [WIFI_TUI_DISCONNECT] = {.label = "disconnect"},
    [WIFI_TUI_REPEATER] = {.label = "repeater"},
    [WIFI_TUI_AP] = {.label = "ap"},
    [WIFI_TUI_SHARE] = {.label = "internet share"},
    [WIFI_TUI_SCAN] = {.label = "scan"},
    [WIFI_TUI_SAVED_STA] = {.label = "saved stations"},
    [WIFI_TUI_SAVED_AP] = {.label = "saved access points"},
};

static size_t wifi_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void wifi_tui_set_status(const char *status)
{
    strlcpy(wifi_tui.status, status != NULL ? status : "", sizeof(wifi_tui.status));
}

static void wifi_tui_share_value(const solar_os_wifi_status_t *status,
                                 char *buffer,
                                 size_t buffer_len)
{
    if (!status->nat_enabled) {
        strlcpy(buffer, "off", buffer_len);
    } else if (status->nat_active) {
        snprintf(buffer,
                 buffer_len,
                 "active via %s",
                 status->nat_uplink[0] != '\0' ? status->nat_uplink : "uplink");
    } else if (status->nat_last_error != ESP_OK) {
        snprintf(buffer, buffer_len, "error %s", solar_os_shell_error_text(status->nat_last_error));
    } else if (!status->ap_running) {
        strlcpy(buffer, "starting ap", buffer_len);
    } else {
        strlcpy(buffer, "waiting uplink", buffer_len);
    }
}

static void wifi_tui_repeater_value(const solar_os_wifi_status_t *status,
                                    char *buffer,
                                    size_t buffer_len)
{
    if (!status->repeater_enabled) {
        strlcpy(buffer, "off", buffer_len);
    } else if (status->repeater_active) {
        snprintf(buffer,
                 buffer_len,
                 "active %u client%s",
                 (unsigned)status->repeater_learned_clients,
                 status->repeater_learned_clients == 1U ? "" : "s");
    } else if (!status->connected || !status->has_ip) {
        strlcpy(buffer, "waiting upstream", buffer_len);
    } else if (!status->ap_running) {
        strlcpy(buffer, "starting ap", buffer_len);
    } else {
        strlcpy(buffer, "starting", buffer_len);
    }
}

static void wifi_tui_current_value(wifi_tui_item_t item,
                                   const solar_os_wifi_status_t *status,
                                   char *buffer,
                                   size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0 || status == NULL) {
        return;
    }

    switch (item) {
    case WIFI_TUI_RADIO:
        strlcpy(buffer, status->started ? "on" : "off", buffer_len);
        break;
    case WIFI_TUI_STATION:
        if (status->connected && status->has_ip) {
            snprintf(buffer,
                     buffer_len,
                     "%s %s",
                     status->ssid[0] != '\0' ? status->ssid : "connected",
                     status->ip);
        } else if (status->state == SOLAR_OS_WIFI_STATE_CONNECTING) {
            snprintf(buffer,
                     buffer_len,
                     "connecting %s",
                     status->ssid[0] != '\0' ? status->ssid : status->saved_ssid);
        } else {
            strlcpy(buffer, solar_os_wifi_state_name(status->state), buffer_len);
        }
        break;
    case WIFI_TUI_DISCONNECT:
        strlcpy(buffer, status->connected ? "ready" : "-", buffer_len);
        break;
    case WIFI_TUI_REPEATER:
        wifi_tui_repeater_value(status, buffer, buffer_len);
        break;
    case WIFI_TUI_AP:
        if (status->ap_running) {
            snprintf(buffer,
                     buffer_len,
                     "on %s",
                     status->ap_ssid[0] != '\0' ? status->ap_ssid : status->ap_ip);
        } else if (status->ap_enabled) {
            strlcpy(buffer, "starting", buffer_len);
        } else if (status->has_saved_ap_config) {
            snprintf(buffer, buffer_len, "off saved %s", status->saved_ap_ssid);
        } else {
            strlcpy(buffer, "off", buffer_len);
        }
        break;
    case WIFI_TUI_SHARE:
        wifi_tui_share_value(status, buffer, buffer_len);
        break;
    case WIFI_TUI_SCAN:
        if (wifi_tui.scan_valid) {
            snprintf(buffer, buffer_len, "%u shown", (unsigned)wifi_tui.scan_count);
        } else {
            strlcpy(buffer, "enter", buffer_len);
        }
        break;
    case WIFI_TUI_SAVED_STA:
        if (status->has_saved_config) {
            snprintf(buffer,
                     buffer_len,
                     "%u %s",
                     (unsigned)status->saved_profile_count,
                     status->saved_ssid);
        } else {
            strlcpy(buffer, "none", buffer_len);
        }
        break;
    case WIFI_TUI_SAVED_AP:
        if (status->has_saved_ap_config) {
            snprintf(buffer,
                     buffer_len,
                     "%s (%s)",
                     status->saved_ap_ssid,
                     status->saved_ap_auth[0] != '\0' ? status->saved_ap_auth : "open");
        } else {
            strlcpy(buffer, "none", buffer_len);
        }
        break;
    default:
        strlcpy(buffer, "-", buffer_len);
        break;
    }
}

static size_t wifi_tui_scan_visible_rows(size_t rows)
{
    (void)rows;
    return solar_os_tui_screen_content_rows(&wifi_tui.tui, 2U, 1U);
}

static void wifi_tui_render_scan(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_tui_clear(tui);

    char detail[24];
    snprintf(detail,
             sizeof(detail),
             "%u network%s",
             (unsigned)wifi_tui.scan_count,
             wifi_tui.scan_count == 1U ? "" : "s");
    solar_os_tui_draw_title(tui, "wifi scan", detail);

    if (rows > 2U) {
        solar_os_tui_write_cell(tui,
                                1U,
                                0,
                                cols,
                                "RSSI CH AUTH       K SSID",
                                SOLAR_OS_TUI_ATTR_BOLD);
    }

    const size_t visible_rows = wifi_tui_scan_visible_rows(rows);
    solar_os_tui_viewport_reconcile(&wifi_tui.scan_viewport,
                                    wifi_tui.scan_count,
                                    visible_rows);

    for (size_t line_index = 0; line_index < visible_rows; line_index++) {
        const size_t ap_index = wifi_tui.scan_viewport.top + line_index;
        if (ap_index >= wifi_tui.scan_count) {
            break;
        }

        char line[WIFI_TUI_STATUS_MAX];
        const bool known = solar_os_wifi_is_known_ssid(wifi_tui.scan_aps[ap_index].ssid);
        snprintf(line,
                 sizeof(line),
                 "%4d %2u %-10s %c %s",
                 (int)wifi_tui.scan_aps[ap_index].rssi,
                 (unsigned)wifi_tui.scan_aps[ap_index].channel,
                 wifi_tui.scan_aps[ap_index].auth,
                 known ? '*' : '-',
                 wifi_tui.scan_aps[ap_index].ssid);
        solar_os_tui_write_cell(tui,
                                2U + line_index,
                                0,
                                cols,
                                line,
                                ap_index == wifi_tui.scan_viewport.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }

    if (wifi_tui.scan_count == 0U && rows > 2U) {
        solar_os_tui_write_cell(tui, 2U, 0, cols, "no networks found", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 1U) {
        solar_os_tui_draw_footer(tui, wifi_tui.status,
                                 "arrows select  enter connects  esc back");
    }

    solar_os_tui_set_cursor_visible(tui, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_render_station_password(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    if (rows == 0 || cols == 0 || wifi_tui.scan_viewport.cursor >= wifi_tui.scan_count) {
        return;
    }

    const solar_os_wifi_ap_t *ap = &wifi_tui.scan_aps[wifi_tui.scan_viewport.cursor];
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, "wifi connect", ap->ssid);

    if (rows > 2U) {
        char details[WIFI_TUI_STATUS_MAX];
        snprintf(details,
                 sizeof(details),
                 "%s, %d dBm, channel %u",
                 ap->auth,
                 (int)ap->rssi,
                 (unsigned)ap->channel);
        solar_os_tui_write_cell(tui, 1U, 0, cols, details, SOLAR_OS_TUI_ATTR_NORMAL);
    }

    if (rows > 1U) {
        const size_t content_end = solar_os_tui_screen_content_end(tui, 1U);
        const size_t input_row = content_end > 1U ? content_end - 1U : 1U;
        solar_os_tui_draw_footer(tui, wifi_tui.status,
                                 "enter connects  esc back");
        solar_os_tui_draw_input(tui,
                                input_row,
                                0,
                                cols,
                                WIFI_TUI_PASSWORD_LABEL,
                                wifi_tui.password,
                                &wifi_tui.password_input,
                                SOLAR_OS_TUI_ATTR_INVERSE);
        solar_os_tui_set_cursor_visible(tui, true);
    }

    solar_os_tui_refresh(tui);
}

static void wifi_tui_render_main(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    solar_os_wifi_status_t status;

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_wifi_get_status(&status);
    solar_os_tui_clear(tui);

    size_t split = cols / 2;
    if (cols >= 24 && split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    solar_os_tui_write_cell(&wifi_tui.tui, 0,
                        0,
                        split,
                        "wifi",
                        SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_write_cell(&wifi_tui.tui, 0,
                            split + 1,
                            wifi_tui_visible_width(cols, split + 1),
                            "value",
                            SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t value_col = split + 1;
    const size_t value_width = wifi_tui_visible_width(cols, value_col);
    for (size_t i = 0; i < WIFI_TUI_ITEM_COUNT && i + 1 < rows; i++) {
        char value[WIFI_TUI_STATUS_MAX];
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (i == wifi_tui.selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }

        wifi_tui_current_value((wifi_tui_item_t)i, &status, value, sizeof(value));
        solar_os_tui_write_cell(&wifi_tui.tui, i + 1, 0, split, wifi_tui_items[i].label, label_attr);
        if (value_width > 0) {
            solar_os_tui_write_cell(&wifi_tui.tui, i + 1, value_col, value_width, value, value_attr);
        }
    }

    if (rows > 1) {
        solar_os_tui_draw_footer(&wifi_tui.tui, wifi_tui.status,
                                 "arrows select/change  enter apply  esc exits");
    }

    solar_os_tui_set_cursor_visible(tui, false);
    solar_os_tui_refresh(tui);
}

static solar_os_tui_rect_t wifi_tui_popup_bounds(size_t rows, size_t cols)
{
    const size_t height = solar_os_tui_screen_content_rows(
        &wifi_tui.tui, 1U, 1U);
    return (solar_os_tui_rect_t) {
        .row = rows > 2U ? 1U : 0U,
        .col = 0U,
        .height = rows > 2U ? height : rows,
        .width = cols,
    };
}

static void wifi_tui_draw_forget_popup(const char *title, const char *ssid)
{
    if (!wifi_tui.confirming_forget || ssid == NULL || ssid[0] == '\0') {
        return;
    }

    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    const solar_os_tui_rect_t bounds = wifi_tui_popup_bounds(rows, cols);
    char message[WIFI_TUI_STATUS_MAX];
    snprintf(message, sizeof(message), "Forget %s?\ny/N", ssid);
    (void)solar_os_tui_text_popup(tui, &bounds, title, message, NULL);
}

static void wifi_tui_render_saved_stations(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    if (rows == 0U || cols == 0U) {
        return;
    }

    solar_os_tui_clear(tui);
    char detail[24];
    snprintf(detail,
             sizeof(detail),
             "%u station%s",
             (unsigned)wifi_tui.saved_station_count,
             wifi_tui.saved_station_count == 1U ? "" : "s");
    solar_os_tui_draw_title(tui, "saved stations", detail);
    if (rows > 2U) {
        solar_os_tui_write_cell(tui, 1U, 0, cols, "P SSID", SOLAR_OS_TUI_ATTR_BOLD);
    }

    const size_t visible_rows = wifi_tui_scan_visible_rows(rows);
    solar_os_tui_viewport_reconcile(&wifi_tui.saved_station_viewport,
                                    wifi_tui.saved_station_count,
                                    visible_rows);
    for (size_t line_index = 0; line_index < visible_rows; line_index++) {
        const size_t profile_index = wifi_tui.saved_station_viewport.top + line_index;
        if (profile_index >= wifi_tui.saved_station_count) {
            break;
        }
        char line[SOLAR_OS_WIFI_SSID_MAX + 4U];
        snprintf(line,
                 sizeof(line),
                 "%c %s",
                 wifi_tui.saved_stations[profile_index].preferred ? '*' : '-',
                 wifi_tui.saved_stations[profile_index].ssid);
        solar_os_tui_write_cell(
            tui,
            2U + line_index,
            0,
            cols,
            line,
            profile_index == wifi_tui.saved_station_viewport.cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (wifi_tui.saved_station_count == 0U && rows > 2U) {
        solar_os_tui_write_cell(tui, 2U, 0, cols, "no saved stations", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 1U) {
        solar_os_tui_draw_footer(tui, wifi_tui.status,
                                 "enter/del forgets  esc back");
    }
    if (wifi_tui.saved_station_viewport.cursor < wifi_tui.saved_station_count) {
        wifi_tui_draw_forget_popup(
            "Forget station",
            wifi_tui.saved_stations[wifi_tui.saved_station_viewport.cursor].ssid);
    }
    solar_os_tui_set_cursor_visible(tui, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_render_saved_aps(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    if (rows == 0U || cols == 0U) {
        return;
    }

    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui,
                            "saved access points",
                            wifi_tui.has_saved_ap ? "1 access point" : "none");
    if (rows > 2U) {
        solar_os_tui_write_cell(tui, 1U, 0, cols, "AUTH       SSID", SOLAR_OS_TUI_ATTR_BOLD);
    }

    if (wifi_tui.has_saved_ap && rows > 2U) {
        char line[WIFI_TUI_STATUS_MAX];
        snprintf(line, sizeof(line), "%-10s %s", wifi_tui.saved_ap.auth, wifi_tui.saved_ap.ssid);
        solar_os_tui_write_cell(tui,
                                2U,
                                0,
                                cols,
                                line,
                                SOLAR_OS_TUI_ATTR_INVERSE);
    } else if (rows > 2U) {
        solar_os_tui_write_cell(tui,
                                2U,
                                0,
                                cols,
                                "+ add access point",
                                SOLAR_OS_TUI_ATTR_INVERSE);
    }
    if (rows > 1U) {
        solar_os_tui_draw_footer(tui, wifi_tui.status,
                                 "enter add/edit  del removes  esc back");
    }
    if (wifi_tui.has_saved_ap) {
        wifi_tui_draw_forget_popup("Remove access point", wifi_tui.saved_ap.ssid);
    }
    solar_os_tui_set_cursor_visible(tui, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_render_ap_input(bool password)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    if (rows == 0U || cols == 0U) {
        return;
    }

    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui,
                            wifi_tui.editing_ap ? "edit access point" : "add access point",
                            password ? wifi_tui.ap_ssid : "");
    if (rows > 1U) {
        const size_t content_end = solar_os_tui_screen_content_end(tui, 1U);
        const size_t input_row = content_end > 1U ? content_end - 1U : 1U;
        solar_os_tui_draw_footer(tui, wifi_tui.status,
                                 password ? "enter saves  esc back" :
                                            "enter continues  esc back");
        solar_os_tui_draw_input(tui,
                                input_row,
                                0,
                                cols,
                                password ? WIFI_TUI_PASSWORD_LABEL : WIFI_TUI_SSID_LABEL,
                                password ? wifi_tui.ap_password : wifi_tui.ap_ssid,
                                &wifi_tui.ap_input,
                                SOLAR_OS_TUI_ATTR_INVERSE);
        solar_os_tui_set_cursor_visible(tui, true);
    }
    solar_os_tui_refresh(tui);
}

static void wifi_tui_render_scan_progress(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    wifi_tui_render_main();
    const solar_os_tui_rect_t bounds = wifi_tui_popup_bounds(rows, cols);
    (void)solar_os_tui_text_popup(tui,
                                  &bounds,
                                  "Scanning",
                                  "Searching for Wi-Fi networks. Please wait.",
                                  NULL);
    solar_os_tui_set_cursor_visible(tui, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_render(void)
{
    switch (wifi_tui.view) {
    case WIFI_TUI_VIEW_SCANNING:
        wifi_tui_render_scan_progress();
        break;
    case WIFI_TUI_VIEW_SCAN:
        wifi_tui_render_scan();
        break;
    case WIFI_TUI_VIEW_STATION_PASSWORD:
        wifi_tui_render_station_password();
        break;
    case WIFI_TUI_VIEW_SAVED_STATIONS:
        wifi_tui_render_saved_stations();
        break;
    case WIFI_TUI_VIEW_SAVED_APS:
        wifi_tui_render_saved_aps();
        break;
    case WIFI_TUI_VIEW_AP_SSID:
        wifi_tui_render_ap_input(false);
        break;
    case WIFI_TUI_VIEW_AP_PASSWORD:
        wifi_tui_render_ap_input(true);
        break;
    case WIFI_TUI_VIEW_MAIN:
    default:
        wifi_tui_render_main();
        break;
    }
}

static void wifi_tui_open_scan(void)
{
    wifi_tui_set_status("");
    wifi_tui.scan_valid = false;
    wifi_tui.view = WIFI_TUI_VIEW_SCANNING;
    wifi_tui_render();

    const esp_err_t err = solar_os_wifi_scan_start_async();
    if (err != ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "scan failed: %s", solar_os_shell_error_text(err));
        wifi_tui.view = WIFI_TUI_VIEW_MAIN;
        wifi_tui_set_status(message);
        wifi_tui_render();
    }
}

static bool wifi_tui_poll_scan(void)
{
    size_t found = 0;
    const esp_err_t err = solar_os_wifi_scan_results(
        wifi_tui.scan_aps,
        sizeof(wifi_tui.scan_aps) / sizeof(wifi_tui.scan_aps[0]),
        &found);
    if (err == ESP_ERR_NOT_FINISHED) {
        return false;
    }
    if (err != ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "scan failed: %s", solar_os_shell_error_text(err));
        wifi_tui.view = WIFI_TUI_VIEW_MAIN;
        wifi_tui_set_status(message);
        return true;
    }

    size_t selectable_count = 0;
    for (size_t i = 0; i < found; i++) {
        if (wifi_tui.scan_aps[i].hidden) {
            continue;
        }
        bool duplicate = false;
        for (size_t existing = 0; existing < selectable_count; existing++) {
            if (strcmp(wifi_tui.scan_aps[existing].ssid, wifi_tui.scan_aps[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            if (selectable_count != i) {
                wifi_tui.scan_aps[selectable_count] = wifi_tui.scan_aps[i];
            }
            selectable_count++;
        }
    }

    wifi_tui.scan_count = selectable_count;
    wifi_tui.scan_valid = true;
    wifi_tui.scan_viewport.cursor = 0;
    wifi_tui.scan_viewport.top = 0;
    wifi_tui.view = WIFI_TUI_VIEW_SCAN;
    wifi_tui_set_status("");
    return true;
}

static void wifi_tui_open_station_password(void)
{
    if (wifi_tui.scan_viewport.cursor >= wifi_tui.scan_count) {
        return;
    }

    memset(wifi_tui.password, 0, sizeof(wifi_tui.password));
    memset(&wifi_tui.password_input, 0, sizeof(wifi_tui.password_input));
    wifi_tui.view = WIFI_TUI_VIEW_STATION_PASSWORD;
    wifi_tui_set_status("");
}

static void wifi_tui_connect_selected(void)
{
    if (wifi_tui.scan_viewport.cursor >= wifi_tui.scan_count) {
        wifi_tui.view = WIFI_TUI_VIEW_SCAN;
        return;
    }

    const solar_os_wifi_ap_t *ap = &wifi_tui.scan_aps[wifi_tui.scan_viewport.cursor];
    const esp_err_t err = solar_os_wifi_connect(ap->ssid, wifi_tui.password);
    memset(wifi_tui.password, 0, sizeof(wifi_tui.password));
    memset(&wifi_tui.password_input, 0, sizeof(wifi_tui.password_input));

    if (err == ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "connecting %s", ap->ssid);
        wifi_tui.view = WIFI_TUI_VIEW_MAIN;
        wifi_tui_set_status(message);
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "connect failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_load_saved_stations(void)
{
    size_t count = 0;
    const esp_err_t err = solar_os_wifi_known(
        wifi_tui.saved_stations,
        sizeof(wifi_tui.saved_stations) / sizeof(wifi_tui.saved_stations[0]),
        &count);
    if (err == ESP_OK) {
        wifi_tui.saved_station_count = count;
        solar_os_tui_viewport_reconcile(&wifi_tui.saved_station_viewport,
                                        count,
                                        wifi_tui_scan_visible_rows(
                                            solar_os_tui_rows(&wifi_tui.tui)));
    } else {
        wifi_tui.saved_station_count = 0U;
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "load failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_open_saved_stations(void)
{
    wifi_tui.saved_station_viewport.cursor = 0U;
    wifi_tui.saved_station_viewport.top = 0U;
    wifi_tui.confirming_forget = false;
    wifi_tui_set_status("");
    wifi_tui_load_saved_stations();
    wifi_tui.view = WIFI_TUI_VIEW_SAVED_STATIONS;
}

static void wifi_tui_forget_selected_station(void)
{
    if (wifi_tui.saved_station_viewport.cursor >= wifi_tui.saved_station_count) {
        return;
    }
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    strlcpy(ssid,
            wifi_tui.saved_stations[wifi_tui.saved_station_viewport.cursor].ssid,
            sizeof(ssid));
    const esp_err_t err = solar_os_wifi_forget_ssid(ssid);
    wifi_tui.confirming_forget = false;
    if (err == ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "forgot %s", ssid);
        wifi_tui_set_status(message);
        wifi_tui_load_saved_stations();
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "forget failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_load_saved_ap(void)
{
    memset(&wifi_tui.saved_ap, 0, sizeof(wifi_tui.saved_ap));
    const esp_err_t err = solar_os_wifi_ap_saved_get(&wifi_tui.saved_ap);
    wifi_tui.has_saved_ap = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "load failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_open_saved_aps(void)
{
    wifi_tui.confirming_forget = false;
    wifi_tui_set_status("");
    wifi_tui_load_saved_ap();
    wifi_tui.view = WIFI_TUI_VIEW_SAVED_APS;
}

static void wifi_tui_begin_ap_edit(bool editing)
{
    memset(wifi_tui.ap_ssid, 0, sizeof(wifi_tui.ap_ssid));
    memset(wifi_tui.ap_password, 0, sizeof(wifi_tui.ap_password));
    memset(wifi_tui.ap_auth, 0, sizeof(wifi_tui.ap_auth));
    if (editing && wifi_tui.has_saved_ap) {
        strlcpy(wifi_tui.ap_ssid, wifi_tui.saved_ap.ssid, sizeof(wifi_tui.ap_ssid));
        strlcpy(wifi_tui.ap_password, wifi_tui.saved_ap.password, sizeof(wifi_tui.ap_password));
        strlcpy(wifi_tui.ap_auth, wifi_tui.saved_ap.auth, sizeof(wifi_tui.ap_auth));
    }
    wifi_tui.editing_ap = editing;
    wifi_tui.ap_input.cursor = strlen(wifi_tui.ap_ssid);
    wifi_tui.ap_input.view = 0U;
    wifi_tui.view = WIFI_TUI_VIEW_AP_SSID;
    wifi_tui_set_status("");
}

static void wifi_tui_save_ap(void)
{
    const char *auth = wifi_tui.ap_password[0] == '\0' ? "open" :
        (wifi_tui.editing_ap && wifi_tui.ap_auth[0] != '\0' &&
         strcmp(wifi_tui.ap_auth, "open") != 0 ? wifi_tui.ap_auth : "wpa2");
    const esp_err_t err = solar_os_wifi_ap_save(
        wifi_tui.ap_ssid, wifi_tui.ap_password, auth);
    if (err == ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "saved %s", wifi_tui.ap_ssid);
        memset(wifi_tui.ap_password, 0, sizeof(wifi_tui.ap_password));
        memset(wifi_tui.ap_auth, 0, sizeof(wifi_tui.ap_auth));
        memset(&wifi_tui.ap_input, 0, sizeof(wifi_tui.ap_input));
        wifi_tui.view = WIFI_TUI_VIEW_SAVED_APS;
        wifi_tui_set_status(message);
        wifi_tui_load_saved_ap();
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "save failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_forget_saved_ap(void)
{
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    strlcpy(ssid, wifi_tui.saved_ap.ssid, sizeof(ssid));
    const esp_err_t err = solar_os_wifi_ap_forget();
    wifi_tui.confirming_forget = false;
    if (err == ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "removed %s", ssid);
        wifi_tui_set_status(message);
        wifi_tui_load_saved_ap();
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "remove failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_start_radio(void)
{
    solar_os_wifi_status_t status;
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "wifi on failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
        return;
    }

    solar_os_wifi_get_status(&status);
    if (status.connected || status.state == SOLAR_OS_WIFI_STATE_CONNECTING ||
        !status.has_saved_config) {
        wifi_tui_set_status("radio on");
        return;
    }

    err = solar_os_wifi_connect_saved();
    if (err == ESP_OK) {
        solar_os_wifi_get_status(&status);
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message,
                 sizeof(message),
                 "connecting %s",
                 status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
        wifi_tui_set_status(message);
    } else if (err == ESP_ERR_NOT_FOUND) {
        wifi_tui_set_status("radio on");
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "connect failed: %s", solar_os_shell_error_text(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_apply_selected(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    switch ((wifi_tui_item_t)wifi_tui.selected) {
    case WIFI_TUI_RADIO:
        if (status.started) {
            const esp_err_t err = solar_os_wifi_stop();
            wifi_tui_set_status(err == ESP_OK ? "radio off" : solar_os_shell_error_text(err));
        } else {
            wifi_tui_start_radio();
        }
        break;
    case WIFI_TUI_STATION: {
        const esp_err_t err = solar_os_wifi_connect_saved();
        if (err == ESP_OK) {
            solar_os_wifi_get_status(&status);
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message,
                     sizeof(message),
                     "connecting %s",
                     status.saved_ssid[0] != '\0' ? status.saved_ssid : status.ssid);
            wifi_tui_set_status(message);
        } else if (err == ESP_ERR_NOT_FOUND) {
            wifi_tui_set_status("no saved station");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "connect failed: %s", solar_os_shell_error_text(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_DISCONNECT: {
        const esp_err_t err = solar_os_wifi_disconnect();
        wifi_tui_set_status(err == ESP_OK ? "station disconnected" : solar_os_shell_error_text(err));
        break;
    }
    case WIFI_TUI_REPEATER: {
        const esp_err_t err = status.repeater_enabled ?
            solar_os_wifi_repeater_stop() : solar_os_wifi_repeater_start();
        if (err == ESP_OK) {
            wifi_tui_set_status(status.repeater_enabled ?
                                "repeater off; station retained" :
                                "repeater on");
        } else if (err == ESP_ERR_NOT_FOUND) {
            wifi_tui_set_status("no saved upstream");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message,
                     sizeof(message),
                     "repeater failed: %s",
                     solar_os_shell_error_text(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_AP: {
        const esp_err_t err =
            (status.ap_running || status.ap_enabled) ?
            solar_os_wifi_ap_stop() :
            solar_os_wifi_ap_start(NULL, NULL, NULL);
        if (err == ESP_OK) {
            wifi_tui_set_status(status.ap_running || status.ap_enabled ? "ap off" : "ap on");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "ap failed: %s", solar_os_shell_error_text(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SHARE: {
        const esp_err_t err = status.nat_enabled ?
            solar_os_wifi_share_stop() : solar_os_wifi_share_start();
        if (err == ESP_OK) {
            wifi_tui_set_status(status.nat_enabled ? "internet sharing off" :
                                "internet sharing on");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            wifi_tui_set_status("internet sharing unsupported");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message,
                     sizeof(message),
                     "internet sharing failed: %s",
                     solar_os_shell_error_text(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SCAN: {
        wifi_tui_open_scan();
        return;
    }
    case WIFI_TUI_SAVED_STA:
        wifi_tui_open_saved_stations();
        break;
    case WIFI_TUI_SAVED_AP:
        wifi_tui_open_saved_aps();
        break;
    default:
        break;
    }

    wifi_tui_render();
}

static esp_err_t wifi_tui_start(solar_os_context_t *ctx)
{
    memset(&wifi_tui, 0, sizeof(wifi_tui));
    wifi_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_screen_begin(&wifi_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    wifi_tui_set_status("enter acts, esc exits");
    solar_os_tui_set_cursor_visible(&wifi_tui.tui, false);
    wifi_tui_render();
    return ESP_OK;
}

static void wifi_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (wifi_tui.view == WIFI_TUI_VIEW_SCANNING) {
        (void)solar_os_wifi_scan_cancel_async();
    }
    memset(wifi_tui.password, 0, sizeof(wifi_tui.password));
    memset(wifi_tui.ap_password, 0, sizeof(wifi_tui.ap_password));
    memset(&wifi_tui.saved_ap, 0, sizeof(wifi_tui.saved_ap));
    solar_os_tui_set_cursor_visible(&wifi_tui.tui, true);
    solar_os_tui_clear(&wifi_tui.tui);
    solar_os_tui_refresh(&wifi_tui.tui);
    solar_os_tui_end(&wifi_tui.tui);
}

static bool wifi_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        wifi_tui_render();
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if (wifi_tui.view == WIFI_TUI_VIEW_SCANNING) {
            if (wifi_tui_poll_scan()) {
                wifi_tui.last_refresh_ms = now_ms;
                wifi_tui_render();
            }
            return true;
        }
        if (wifi_tui.last_refresh_ms == 0) {
            wifi_tui.last_refresh_ms = now_ms;
            return true;
        }
        if ((now_ms - wifi_tui.last_refresh_ms) >= WIFI_TUI_REFRESH_MS) {
            wifi_tui.last_refresh_ms = now_ms;
            wifi_tui_render();
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(wifi_tui.ctx, 0, NULL);
        return true;
    }

    if (wifi_tui.view == WIFI_TUI_VIEW_SCANNING) {
        if (key == SOLAR_OS_KEY_ESCAPE) {
            (void)solar_os_wifi_scan_cancel_async();
            wifi_tui.view = WIFI_TUI_VIEW_MAIN;
            wifi_tui_render();
        }
        return true;
    }

    if (wifi_tui.view == WIFI_TUI_VIEW_STATION_PASSWORD) {
        const size_t cols = solar_os_tui_cols(&wifi_tui.tui);
        const size_t label_width = strlen(WIFI_TUI_PASSWORD_LABEL);
        const solar_os_tui_input_action_t action = solar_os_tui_input_key(
            wifi_tui.password,
            sizeof(wifi_tui.password),
            &wifi_tui.password_input,
            key,
            cols > label_width ? cols - label_width : 1U);
        if (action == SOLAR_OS_TUI_INPUT_CANCEL) {
            memset(wifi_tui.password, 0, sizeof(wifi_tui.password));
            memset(&wifi_tui.password_input, 0, sizeof(wifi_tui.password_input));
            wifi_tui.view = WIFI_TUI_VIEW_SCAN;
            wifi_tui_set_status("");
        } else if (action == SOLAR_OS_TUI_INPUT_SUBMIT) {
            wifi_tui_connect_selected();
        }
        wifi_tui_render();
        return true;
    }

    if (wifi_tui.view == WIFI_TUI_VIEW_SCAN) {
        if (key == SOLAR_OS_KEY_ESCAPE) {
            wifi_tui.view = WIFI_TUI_VIEW_MAIN;
            wifi_tui_set_status("");
        } else if (key == SOLAR_OS_KEY_ENTER || key == '\r') {
            wifi_tui_open_station_password();
        } else {
            const size_t visible_rows = wifi_tui_scan_visible_rows(
                solar_os_tui_rows(&wifi_tui.tui));
            if (solar_os_tui_viewport_key(&wifi_tui.scan_viewport,
                                          key,
                                          wifi_tui.scan_count,
                                          visible_rows,
                                          false)) {
                wifi_tui_set_status("");
            }
        }
        wifi_tui_render();
        return true;
    }

    if (wifi_tui.view == WIFI_TUI_VIEW_SAVED_STATIONS) {
        if (wifi_tui.confirming_forget) {
            if (key == 'y' || key == 'Y') {
                wifi_tui_forget_selected_station();
            } else {
                wifi_tui.confirming_forget = false;
            }
        } else if (key == SOLAR_OS_KEY_ESCAPE) {
            wifi_tui.view = WIFI_TUI_VIEW_MAIN;
            wifi_tui_set_status("");
        } else if ((key == SOLAR_OS_KEY_ENTER || key == '\r' ||
                    key == SOLAR_OS_KEY_DELETE) &&
                   wifi_tui.saved_station_count > 0U) {
            wifi_tui.confirming_forget = true;
        } else {
            const size_t visible_rows = wifi_tui_scan_visible_rows(
                solar_os_tui_rows(&wifi_tui.tui));
            if (solar_os_tui_viewport_key(&wifi_tui.saved_station_viewport,
                                          key,
                                          wifi_tui.saved_station_count,
                                          visible_rows,
                                          false)) {
                wifi_tui_set_status("");
            }
        }
        wifi_tui_render();
        return true;
    }

    if (wifi_tui.view == WIFI_TUI_VIEW_SAVED_APS) {
        if (wifi_tui.confirming_forget) {
            if (key == 'y' || key == 'Y') {
                wifi_tui_forget_saved_ap();
            } else {
                wifi_tui.confirming_forget = false;
            }
        } else if (key == SOLAR_OS_KEY_ESCAPE) {
            wifi_tui.view = WIFI_TUI_VIEW_MAIN;
            wifi_tui_set_status("");
        } else if (key == SOLAR_OS_KEY_DELETE && wifi_tui.has_saved_ap) {
            wifi_tui.confirming_forget = true;
        } else if (key == SOLAR_OS_KEY_ENTER || key == '\r') {
            wifi_tui_begin_ap_edit(wifi_tui.has_saved_ap);
        }
        wifi_tui_render();
        return true;
    }

    if (wifi_tui.view == WIFI_TUI_VIEW_AP_SSID ||
        wifi_tui.view == WIFI_TUI_VIEW_AP_PASSWORD) {
        const bool password = wifi_tui.view == WIFI_TUI_VIEW_AP_PASSWORD;
        const size_t cols = solar_os_tui_cols(&wifi_tui.tui);
        const size_t label_width = strlen(
            password ? WIFI_TUI_PASSWORD_LABEL : WIFI_TUI_SSID_LABEL);
        char *text = password ? wifi_tui.ap_password : wifi_tui.ap_ssid;
        const size_t capacity = password ? sizeof(wifi_tui.ap_password) :
                                           sizeof(wifi_tui.ap_ssid);
        const solar_os_tui_input_action_t action = solar_os_tui_input_key(
            text,
            capacity,
            &wifi_tui.ap_input,
            key,
            cols > label_width ? cols - label_width : 1U);
        if (action == SOLAR_OS_TUI_INPUT_CANCEL) {
            if (password) {
                wifi_tui.view = WIFI_TUI_VIEW_AP_SSID;
                wifi_tui.ap_input.cursor = strlen(wifi_tui.ap_ssid);
                wifi_tui.ap_input.view = 0U;
            } else {
                memset(wifi_tui.ap_password, 0, sizeof(wifi_tui.ap_password));
                wifi_tui.view = WIFI_TUI_VIEW_SAVED_APS;
            }
            wifi_tui_set_status("");
        } else if (action == SOLAR_OS_TUI_INPUT_SUBMIT) {
            if (!password) {
                if (wifi_tui.ap_ssid[0] == '\0') {
                    wifi_tui_set_status("SSID is required");
                } else {
                    wifi_tui.view = WIFI_TUI_VIEW_AP_PASSWORD;
                    wifi_tui.ap_input.cursor = strlen(wifi_tui.ap_password);
                    wifi_tui.ap_input.view = 0U;
                    wifi_tui_set_status("");
                }
            } else {
                wifi_tui_save_ap();
            }
        } else if (action == SOLAR_OS_TUI_INPUT_CHANGED) {
            wifi_tui_set_status("");
        }
        wifi_tui_render();
        return true;
    }

    if (key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_finish(wifi_tui.ctx, 0, NULL);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (wifi_tui.selected > 0) {
            wifi_tui.selected--;
            wifi_tui_set_status("");
            wifi_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (wifi_tui.selected + 1 < WIFI_TUI_ITEM_COUNT) {
            wifi_tui.selected++;
            wifi_tui_set_status("");
            wifi_tui_render();
        }
        break;
    case '\r':
    case '\n':
        wifi_tui_apply_selected();
        break;
    default:
        break;
    }

    return true;
}

static const solar_os_app_t wifi_tui_app = {
    .name = "wifi",
    .summary = "Wi-Fi control",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .start = wifi_tui_start,
    .stop = wifi_tui_stop,
    .event = wifi_tui_event,
    .state_slot = &wifi_tui_state,
    .state_size = sizeof(wifi_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};

esp_err_t solar_os_shell_launch_wifi_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &wifi_tui_app, 0, NULL);
}
