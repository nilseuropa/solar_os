#include "solar_os_shell_tui_apps.h"
#include "solar_os_shell_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_wifi.h"

#define WIFI_TUI_STATUS_MAX 96
#define WIFI_TUI_REFRESH_MS 1000

static solar_os_terminal_t *display_terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_display_terminal(ctx);
}

typedef enum {
    WIFI_TUI_RADIO,
    WIFI_TUI_STATION,
    WIFI_TUI_DISCONNECT,
    WIFI_TUI_AP,
    WIFI_TUI_NAT,
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
    size_t selected;
    char status[WIFI_TUI_STATUS_MAX];
    solar_os_wifi_ap_t scan_aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t scan_count;
    bool scan_valid;
    uint32_t last_refresh_ms;
} wifi_tui_state_t;

static wifi_tui_state_t wifi_tui;

static const wifi_tui_item_def_t wifi_tui_items[] = {
    [WIFI_TUI_RADIO] = {.label = "radio"},
    [WIFI_TUI_STATION] = {.label = "station"},
    [WIFI_TUI_DISCONNECT] = {.label = "disconnect"},
    [WIFI_TUI_AP] = {.label = "ap"},
    [WIFI_TUI_NAT] = {.label = "nat"},
    [WIFI_TUI_SCAN] = {.label = "scan"},
    [WIFI_TUI_SAVED_STA] = {.label = "saved sta"},
    [WIFI_TUI_SAVED_AP] = {.label = "saved ap"},
};

static size_t wifi_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void wifi_tui_set_status(const char *status)
{
    strlcpy(wifi_tui.status, status != NULL ? status : "", sizeof(wifi_tui.status));
}

static void wifi_tui_write_cell(size_t row,
                                size_t col,
                                size_t width,
                                const char *text,
                                uint8_t attr)
{
    char clipped[WIFI_TUI_STATUS_MAX];
    size_t len = 0;

    if (width == 0) {
        return;
    }

    solar_os_tui_fill(&wifi_tui.tui, row, col, 1, width, ' ', attr);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    while (text[len] != '\0' && len + 1 < sizeof(clipped) && len < width) {
        clipped[len] = text[len];
        len++;
    }
    clipped[len] = '\0';
    solar_os_tui_addstr(&wifi_tui.tui, row, col, clipped, attr);
}

static void wifi_tui_nat_value(const solar_os_wifi_status_t *status,
                               char *buffer,
                               size_t buffer_len)
{
    if (!status->nat_enabled) {
        strlcpy(buffer, "off", buffer_len);
    } else if (status->nat_active) {
        strlcpy(buffer, "active", buffer_len);
    } else if (status->nat_last_error != ESP_OK) {
        snprintf(buffer, buffer_len, "error %s", esp_err_to_name(status->nat_last_error));
    } else {
        strlcpy(buffer, "waiting", buffer_len);
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
    case WIFI_TUI_NAT:
        wifi_tui_nat_value(status, buffer, buffer_len);
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

static void wifi_tui_render_scan(size_t start_row, size_t rows, size_t cols)
{
    if (start_row >= rows || cols == 0 || !wifi_tui.scan_valid) {
        return;
    }

    wifi_tui_write_cell(start_row,
                        0,
                        cols,
                        wifi_tui.scan_count == 0 ? "scan: no networks" : "scan: rssi ch auth k ssid",
                        SOLAR_OS_TUI_ATTR_BOLD);

    for (size_t i = 0; i < wifi_tui.scan_count && start_row + i + 1 < rows; i++) {
        char line[WIFI_TUI_STATUS_MAX];
        const bool known = !wifi_tui.scan_aps[i].hidden &&
            solar_os_wifi_is_known_ssid(wifi_tui.scan_aps[i].ssid);
        snprintf(line,
                 sizeof(line),
                 "%4d %2u %-10s %c %s",
                 (int)wifi_tui.scan_aps[i].rssi,
                 (unsigned)wifi_tui.scan_aps[i].channel,
                 wifi_tui.scan_aps[i].auth,
                 known ? '*' : '-',
                 wifi_tui.scan_aps[i].ssid);
        wifi_tui_write_cell(start_row + i + 1, 0, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void wifi_tui_render(void)
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

    wifi_tui_write_cell(0,
                        0,
                        split,
                        "wifi",
                        SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        wifi_tui_write_cell(0,
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
        wifi_tui_write_cell(i + 1, 0, split, wifi_tui_items[i].label, label_attr);
        if (value_width > 0) {
            wifi_tui_write_cell(i + 1, value_col, value_width, value, value_attr);
        }
    }

    const size_t scan_row = WIFI_TUI_ITEM_COUNT + 2;
    const size_t status_row = rows > 1 ? rows - 1 : 0;
    if (scan_row < status_row) {
        wifi_tui_render_scan(scan_row, status_row, cols);
    }

    if (wifi_tui.status[0] != '\0' && rows > 1) {
        wifi_tui_write_cell(status_row, 0, cols, wifi_tui.status, SOLAR_OS_TUI_ATTR_INVERSE);
    }

    solar_os_terminal_set_cursor_visible(tui->terminal, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_start_radio(void)
{
    solar_os_wifi_status_t status;
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "wifi on failed: %s", esp_err_to_name(err));
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
        snprintf(message, sizeof(message), "connect failed: %s", esp_err_to_name(err));
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
            wifi_tui_set_status(err == ESP_OK ? "radio off" : esp_err_to_name(err));
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
            snprintf(message, sizeof(message), "connect failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_DISCONNECT: {
        const esp_err_t err = solar_os_wifi_disconnect();
        wifi_tui_set_status(err == ESP_OK ? "station disconnected" : esp_err_to_name(err));
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
            snprintf(message, sizeof(message), "ap failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_NAT: {
        const esp_err_t err = solar_os_wifi_nat_set(!status.nat_enabled);
        if (err == ESP_OK) {
            wifi_tui_set_status(status.nat_enabled ? "nat off" : "nat on");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            wifi_tui_set_status("nat unsupported");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "nat failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SCAN: {
        size_t found = 0;
        wifi_tui_set_status("scanning...");
        wifi_tui.scan_valid = false;
        wifi_tui_render();
        const esp_err_t err = solar_os_wifi_scan(wifi_tui.scan_aps,
                                                 sizeof(wifi_tui.scan_aps) / sizeof(wifi_tui.scan_aps[0]),
                                                 &found);
        if (err == ESP_OK) {
            wifi_tui.scan_count = found;
            wifi_tui.scan_valid = true;
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "%u network%s", (unsigned)found, found == 1 ? "" : "s");
            wifi_tui_set_status(message);
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "scan failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SAVED_STA:
    case WIFI_TUI_SAVED_AP:
        wifi_tui_set_status("read only");
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
    const esp_err_t err = solar_os_tui_begin(&wifi_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    wifi_tui_set_status("enter acts, esc exits");
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), false);
    wifi_tui_render();
    return ESP_OK;
}

static void wifi_tui_stop(solar_os_context_t *ctx)
{
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), true);
    solar_os_terminal_clear(display_terminal(ctx));
    solar_os_context_request_terminal_preserve(ctx);
}

static bool wifi_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
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
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(wifi_tui.ctx);
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
    .start = wifi_tui_start,
    .stop = wifi_tui_stop,
    .event = wifi_tui_event,
};

esp_err_t solar_os_shell_launch_wifi_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &wifi_tui_app, 0, NULL);
}
