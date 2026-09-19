#include "solar_os_shell_tui_apps.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_netif.h"
#include "solar_os_config.h"
#include "solar_os_keys.h"
#include "solar_os_network.h"
#include "solar_os_shell_common.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"
#if SOLAR_OS_PACKAGE_SERVICE_MODEM
#include "solar_os_modem.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
#include "solar_os_wireguard.h"
#endif

#define NETWORK_TUI_REFRESH_MS 1000U
#define NETWORK_TUI_MIN_ROWS 7U
#define NETWORK_TUI_MIN_COLS 28U
#define NETWORK_TUI_ROW_MAX 24U
#define NETWORK_TUI_ROW_TEXT_MAX 96U
#define NETWORK_TUI_STATUS_MAX 96U
#define NETWORK_TUI_PRIORITY_STEP 10

typedef enum {
    NETWORK_TUI_TAB_STATUS,
    NETWORK_TUI_TAB_SETTINGS,
} network_tui_tab_t;

typedef struct {
    char text[NETWORK_TUI_ROW_TEXT_MAX];
    uint8_t attr;
} network_tui_row_t;

typedef enum {
    NETWORK_TUI_SETTING_HEADER,
    NETWORK_TUI_SETTING_WIFI,
    NETWORK_TUI_SETTING_MODEMS,
    NETWORK_TUI_SETTING_PATH,
    NETWORK_TUI_SETTING_ROUTING,
    NETWORK_TUI_SETTING_HELP,
} network_tui_setting_type_t;

typedef struct {
    network_tui_setting_type_t type;
    size_t path_index;
    bool selectable;
    char text[NETWORK_TUI_ROW_TEXT_MAX];
} network_tui_setting_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    network_tui_tab_t tab;
    solar_os_tui_viewport_t status_viewport;
    solar_os_tui_viewport_t settings_viewport;
    char status[NETWORK_TUI_STATUS_MAX];
    uint32_t last_refresh_ms;
} network_tui_state_t;

static void *network_tui_state;
#define network_tui (*(network_tui_state_t *)network_tui_state)

static void network_tui_set_status(const char *status)
{
    strlcpy(network_tui.status,
            status != NULL ? status : "",
            sizeof(network_tui.status));
}

static void network_tui_add_row(network_tui_row_t *rows,
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

static void network_tui_ip(const solar_os_network_path_info_t *path,
                           char *address,
                           size_t address_len)
{
    strlcpy(address, "-", address_len);
    esp_netif_ip_info_t ip = {0};
    if (path != NULL && path->ready &&
        esp_netif_get_ip_info(path->netif, &ip) == ESP_OK &&
        ip.ip.addr != 0U) {
        esp_ip4addr_ntoa(&ip.ip, address, address_len);
    }
}

static size_t network_tui_build_status_rows(network_tui_row_t *rows,
                                            size_t max_rows)
{
    size_t count = 0U;
    solar_os_network_path_info_t paths[SOLAR_OS_NETWORK_PATH_MAX];
    const size_t path_count = solar_os_network_path_list(
        paths,
        sizeof(paths) / sizeof(paths[0]));
    solar_os_network_path_info_t preferred = {0};
    const bool have_preferred = solar_os_network_path_get_preferred(&preferred);

    network_tui_add_row(rows, max_rows, &count,
                        SOLAR_OS_TUI_ATTR_BOLD, "Interfaces");
    if (path_count == 0U) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "  no route-capable interfaces");
    }
    for (size_t i = 0U;
         i < path_count && i < SOLAR_OS_NETWORK_PATH_MAX;
         i++) {
        char address[16];
        network_tui_ip(&paths[i], address, sizeof(address));
        network_tui_add_row(
            rows,
            max_rows,
            &count,
            SOLAR_OS_TUI_ATTR_NORMAL,
            "%c %-10s %-4s %-15s p%d",
            have_preferred && paths[i].netif == preferred.netif ? '*' : ' ',
            paths[i].name,
            paths[i].ready ? "up" : "down",
            address,
            paths[i].route_priority);
    }

    solar_os_network_router_status_t router = {0};
    solar_os_network_router_get_status(&router);
    if (router.available) {
        network_tui_add_row(
            rows,
            max_rows,
            &count,
            SOLAR_OS_TUI_ATTR_NORMAL,
            "  %-10s %-4s %-15s local",
            router.downstream,
            router.downstream_active ? "up" : "down",
            router.downstream_active && router.address[0] != '\0' ?
                router.address : "-");
    }

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    solar_os_wireguard_status_t wireguard = {0};
    solar_os_wireguard_get_status(&wireguard);
    if (wireguard.configured || wireguard.desired_up ||
        wireguard.routes_active) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "  %-10s %-4s %-15s tunnel",
                            "wireguard",
                            wireguard.routes_active ? "up" : "down",
                            wireguard.routes_active ? wireguard.address : "-");
    }
#endif

    network_tui_add_row(rows, max_rows, &count,
                        SOLAR_OS_TUI_ATTR_NORMAL, "");
    network_tui_add_row(rows, max_rows, &count,
                        SOLAR_OS_TUI_ATTR_BOLD, "Routing");
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    if (wireguard.default_route_active) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "default: wireguard over %s",
                            wireguard.underlay[0] != '\0' ?
                                wireguard.underlay : "uplink");
    } else
#endif
    if (have_preferred) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "default: %s (automatic)",
                            preferred.name);
    } else {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "default: unavailable");
    }

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    if (wireguard.routes_active) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "vpn: %u route%s%s",
                            (unsigned)wireguard.route_count,
                            wireguard.route_count == 1U ? "" : "s",
                            wireguard.full_tunnel ? " (full tunnel)" : "");
    } else if (wireguard.desired_up) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "vpn: waiting (%s)",
                            solar_os_wireguard_state_name(wireguard.state));
    }
#endif

    if (!router.available) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "clients: routing unavailable");
    } else if (!router.enabled) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "clients: routing off");
    } else if (router.active) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "clients: %s -> %s, NAT %u/%u",
                            router.downstream,
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
                            wireguard.default_route_active ? "wireguard" :
#endif
                            have_preferred ? preferred.name : "default",
                            (unsigned)router.client_count,
                            (unsigned)router.client_limit);
    } else if (router.last_error != ESP_OK) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "clients: error %s",
                            solar_os_shell_error_text(router.last_error));
    } else if (!have_preferred) {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "clients: waiting for default route");
    } else {
        network_tui_add_row(rows, max_rows, &count,
                            SOLAR_OS_TUI_ATTR_NORMAL,
                            "clients: starting %s",
                            router.downstream);
    }
    return count;
}

static const char *network_tui_routing_value(
    const solar_os_network_router_status_t *router)
{
    if (!router->available) {
        return "unavailable";
    }
    if (!router->enabled) {
        return "off";
    }
    if (router->active) {
        return "on (active)";
    }
    if (router->last_error != ESP_OK) {
        return "on (error)";
    }
    return "on (waiting)";
}

static void network_tui_draw_tabs(const solar_os_tui_screen_layout_t *layout)
{
    static const char status_label[] = " Status ";
    static const char settings_label[] = " Settings ";
    solar_os_tui_write_cell(&network_tui.tui,
                            layout->tabs.row,
                            0U,
                            layout->tabs.width,
                            "",
                            SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_draw_tab(&network_tui.tui,
                          layout->tabs.row,
                          0U,
                          sizeof(status_label) - 1U,
                          status_label,
                          network_tui.tab == NETWORK_TUI_TAB_STATUS);
    solar_os_tui_draw_tab(&network_tui.tui,
                          layout->tabs.row,
                          sizeof(status_label) - 1U,
                          sizeof(settings_label) - 1U,
                          settings_label,
                          network_tui.tab == NETWORK_TUI_TAB_SETTINGS);
}

static void network_tui_draw_status(const solar_os_tui_screen_layout_t *layout)
{
    network_tui_row_t rows[NETWORK_TUI_ROW_MAX];
    const size_t row_count = network_tui_build_status_rows(
        rows,
        sizeof(rows) / sizeof(rows[0]));
    solar_os_tui_viewport_reconcile(&network_tui.status_viewport,
                                    row_count,
                                    layout->body.height);
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = network_tui.status_viewport.top + row;
        solar_os_tui_write_cell(&network_tui.tui,
                                layout->body.row + row,
                                0U,
                                layout->body.width,
                                index < row_count ? rows[index].text : "",
                                index < row_count ? rows[index].attr :
                                                    SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static size_t network_tui_get_paths(
    solar_os_network_path_info_t paths[SOLAR_OS_NETWORK_PATH_MAX])
{
    size_t count = solar_os_network_path_list(paths,
                                              SOLAR_OS_NETWORK_PATH_MAX);
    return count < SOLAR_OS_NETWORK_PATH_MAX ? count :
                                               SOLAR_OS_NETWORK_PATH_MAX;
}

static void network_tui_add_setting(network_tui_setting_t *items,
                                    size_t max_items,
                                    size_t *count,
                                    network_tui_setting_type_t type,
                                    size_t path_index,
                                    bool selectable,
                                    const char *format,
                                    ...)
{
    if (items == NULL || count == NULL || *count >= max_items ||
        format == NULL) {
        return;
    }
    network_tui_setting_t *item = &items[*count];
    item->type = type;
    item->path_index = path_index;
    item->selectable = selectable;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(item->text, sizeof(item->text), format, args);
    va_end(args);
    (*count)++;
}

static size_t network_tui_build_settings(
    network_tui_setting_t *items,
    size_t max_items,
    solar_os_network_path_info_t paths[SOLAR_OS_NETWORK_PATH_MAX],
    size_t *path_count_out)
{
    const size_t path_count = network_tui_get_paths(paths);
    if (path_count_out != NULL) {
        *path_count_out = path_count;
    }
    size_t count = 0U;
#if SOLAR_OS_PACKAGE_SERVICE_WIFI || SOLAR_OS_PACKAGE_SERVICE_MODEM
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_HEADER, 0U, false,
                            "Configure");
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_WIFI, 0U, true,
                            "  Wi-Fi...");
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MODEM
    const size_t modem_count = solar_os_modem_count();
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_MODEMS, 0U, true,
                            "  Modems... (%u registered)",
                            (unsigned)modem_count);
#endif
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_HEADER, 0U, false,
                            "Routing");

    solar_os_network_path_info_t preferred = {0};
    const bool have_preferred = solar_os_network_path_get_preferred(&preferred);
    for (size_t i = 0U; i < path_count; i++) {
        network_tui_add_setting(
            items, max_items, &count,
            NETWORK_TUI_SETTING_PATH, i, true,
            "%c %-12s priority %d",
            have_preferred && paths[i].netif == preferred.netif ? '*' : ' ',
            paths[i].name,
            paths[i].route_priority);
    }

    solar_os_network_router_status_t router = {0};
    solar_os_network_router_get_status(&router);
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_ROUTING, 0U, true,
                            "  %-12s %s",
                            "routing",
                            network_tui_routing_value(&router));
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_HELP, 0U, false,
                            "");
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_HELP, 0U, false,
                            "Higher priority wins the default route.");
    network_tui_add_setting(items, max_items, &count,
                            NETWORK_TUI_SETTING_HELP, 0U, false,
                            "Routing forwards Wi-Fi AP clients through it.");
    return count;
}

static void network_tui_settings_reconcile(
    const network_tui_setting_t *items,
    size_t item_count,
    size_t visible_rows)
{
    if (item_count == 0U) {
        network_tui.settings_viewport = (solar_os_tui_viewport_t){0};
        return;
    }
    if (network_tui.settings_viewport.cursor >= item_count ||
        !items[network_tui.settings_viewport.cursor].selectable) {
        size_t first = 0U;
        while (first < item_count && !items[first].selectable) {
            first++;
        }
        network_tui.settings_viewport.cursor = first < item_count ? first : 0U;
    }
    solar_os_tui_viewport_reconcile(&network_tui.settings_viewport,
                                    item_count,
                                    visible_rows);
}

static void network_tui_settings_move(const network_tui_setting_t *items,
                                      size_t item_count,
                                      int direction,
                                      size_t visible_rows)
{
    network_tui_settings_reconcile(items, item_count, visible_rows);
    size_t cursor = network_tui.settings_viewport.cursor;
    while ((direction < 0 && cursor > 0U) ||
           (direction > 0 && cursor + 1U < item_count)) {
        cursor = direction < 0 ? cursor - 1U : cursor + 1U;
        if (items[cursor].selectable) {
            network_tui.settings_viewport.cursor = cursor;
            break;
        }
    }
    solar_os_tui_viewport_reconcile(&network_tui.settings_viewport,
                                    item_count,
                                    visible_rows);
}

static void network_tui_draw_settings(const solar_os_tui_screen_layout_t *layout)
{
    solar_os_network_path_info_t paths[SOLAR_OS_NETWORK_PATH_MAX];
    network_tui_setting_t items[NETWORK_TUI_ROW_MAX];
    const size_t item_count = network_tui_build_settings(
        items, sizeof(items) / sizeof(items[0]), paths, NULL);
    network_tui_settings_reconcile(items,
                                   item_count,
                                   layout->body.height);
    for (size_t row = 0U; row < layout->body.height; row++) {
        const size_t index = network_tui.settings_viewport.top + row;
        uint8_t attr = SOLAR_OS_TUI_ATTR_NORMAL;
        if (index < item_count && items[index].selectable &&
            index == network_tui.settings_viewport.cursor) {
            attr = SOLAR_OS_TUI_ATTR_INVERSE;
        } else if (index < item_count &&
                   items[index].type == NETWORK_TUI_SETTING_HEADER) {
            attr = SOLAR_OS_TUI_ATTR_BOLD;
        }
        solar_os_tui_write_cell(&network_tui.tui,
                                layout->body.row + row,
                                0U,
                                layout->body.width,
                                index < item_count ? items[index].text : "",
                                attr);
    }
}

static void network_tui_render(void)
{
    const size_t rows = solar_os_tui_rows(&network_tui.tui);
    const size_t cols = solar_os_tui_cols(&network_tui.tui);
    if (rows < NETWORK_TUI_MIN_ROWS || cols < NETWORK_TUI_MIN_COLS) {
        solar_os_tui_draw_too_small(&network_tui.tui, "network");
        solar_os_tui_refresh(&network_tui.tui);
        return;
    }

    solar_os_tui_screen_layout_t layout;
    if (!solar_os_tui_screen_layout(&network_tui.tui, 1U, 0U, 0U, &layout)) {
        solar_os_tui_draw_too_small(&network_tui.tui, "network");
        solar_os_tui_refresh(&network_tui.tui);
        return;
    }

    solar_os_network_path_info_t preferred = {0};
    char detail[32];
    if (solar_os_network_path_get_preferred(&preferred)) {
        snprintf(detail, sizeof(detail), "via %s", preferred.name);
    } else {
        strlcpy(detail, "offline", sizeof(detail));
    }

    solar_os_tui_clear(&network_tui.tui);
    solar_os_tui_draw_title(&network_tui.tui, "Network", detail);
    network_tui_draw_tabs(&layout);
    if (network_tui.tab == NETWORK_TUI_TAB_STATUS) {
        network_tui_draw_status(&layout);
    } else {
        network_tui_draw_settings(&layout);
    }
    solar_os_tui_draw_footer(
        &network_tui.tui,
        network_tui.status,
        network_tui.tab == NETWORK_TUI_TAB_STATUS ?
            "TAB settings  UP/DOWN scroll  ESC exit" :
            "TAB status  arrows select/change  ENTER opens/acts  ESC exit");
    solar_os_tui_set_cursor_visible(&network_tui.tui, false);
    solar_os_tui_refresh(&network_tui.tui);
}

static void network_tui_change_priority(
    const solar_os_network_path_info_t *path,
    int direction)
{
    int priority = path->route_priority + direction * NETWORK_TUI_PRIORITY_STEP;
    if (priority < SOLAR_OS_NETWORK_PRIORITY_MIN) {
        priority = SOLAR_OS_NETWORK_PRIORITY_MIN;
    } else if (priority > SOLAR_OS_NETWORK_PRIORITY_MAX) {
        priority = SOLAR_OS_NETWORK_PRIORITY_MAX;
    }
    if (priority == path->route_priority) {
        network_tui_set_status(direction < 0 ? "minimum priority" :
                                               "maximum priority");
        return;
    }
    const esp_err_t ret = solar_os_network_path_set_priority(path->name,
                                                             priority);
    if (ret == ESP_OK) {
        char message[NETWORK_TUI_STATUS_MAX];
        snprintf(message,
                 sizeof(message),
                 "%s priority %d saved",
                 path->name,
                 priority);
        network_tui_set_status(message);
    } else {
        char message[NETWORK_TUI_STATUS_MAX];
        snprintf(message,
                 sizeof(message),
                 "priority failed: %s",
                 solar_os_shell_error_text(ret));
        network_tui_set_status(message);
    }
}

static void network_tui_set_routing(bool enabled)
{
    const esp_err_t ret = enabled ? solar_os_network_router_start() :
                                    solar_os_network_router_stop();
    if (ret == ESP_OK) {
        network_tui_set_status(enabled ? "routing enabled" :
                                         "routing disabled");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        network_tui_set_status("routing unavailable");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        network_tui_set_status("another forwarding mode is active");
    } else {
        char message[NETWORK_TUI_STATUS_MAX];
        snprintf(message,
                 sizeof(message),
                 "routing failed: %s",
                 solar_os_shell_error_text(ret));
        network_tui_set_status(message);
    }
}

static void network_tui_handle_status_key(uint8_t key)
{
    network_tui_row_t rows[NETWORK_TUI_ROW_MAX];
    const size_t row_count = network_tui_build_status_rows(
        rows,
        sizeof(rows) / sizeof(rows[0]));
    const size_t visible = solar_os_tui_screen_content_rows(&network_tui.tui,
                                                            2U,
                                                            1U);
    if (solar_os_tui_viewport_key(&network_tui.status_viewport,
                                  key,
                                  row_count,
                                  visible,
                                  false)) {
        network_tui_set_status("");
    }
}

static void network_tui_handle_settings_key(uint8_t key)
{
    solar_os_network_path_info_t paths[SOLAR_OS_NETWORK_PATH_MAX];
    network_tui_setting_t items[NETWORK_TUI_ROW_MAX];
    size_t path_count = 0U;
    const size_t item_count = network_tui_build_settings(
        items,
        sizeof(items) / sizeof(items[0]),
        paths,
        &path_count);
    const size_t visible = solar_os_tui_screen_content_rows(&network_tui.tui,
                                                            2U,
                                                            1U);
    if (key == SOLAR_OS_KEY_UP) {
        network_tui_settings_move(items, item_count, -1, visible);
        network_tui_set_status("");
        return;
    }
    if (key == SOLAR_OS_KEY_DOWN) {
        network_tui_settings_move(items, item_count, 1, visible);
        network_tui_set_status("");
        return;
    }

    network_tui_settings_reconcile(items, item_count, visible);
    const network_tui_setting_t *selected =
        &items[network_tui.settings_viewport.cursor];
    const bool enter = key == SOLAR_OS_KEY_ENTER || key == '\r' || key == '\n';
    if (selected->type == NETWORK_TUI_SETTING_WIFI && enter) {
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
        const esp_err_t ret = solar_os_shell_launch_wifi_tui_ex(
            network_tui.ctx,
            SOLAR_OS_LAUNCH_CHILD_RETURN);
        if (ret != ESP_OK) {
            network_tui_set_status("Wi-Fi settings unavailable");
        }
#endif
        return;
    }
    if (selected->type == NETWORK_TUI_SETTING_MODEMS && enter) {
#if SOLAR_OS_PACKAGE_SERVICE_MODEM
        const esp_err_t ret = solar_os_shell_launch_modem_tui_ex(
            network_tui.ctx,
            SOLAR_OS_LAUNCH_CHILD_RETURN);
        if (ret != ESP_OK) {
            network_tui_set_status("modem settings unavailable");
        }
#endif
        return;
    }
    if (selected->type == NETWORK_TUI_SETTING_PATH &&
        selected->path_index < path_count) {
        const solar_os_network_path_info_t *path =
            &paths[selected->path_index];
        if (key == SOLAR_OS_KEY_LEFT) {
            network_tui_change_priority(path, -1);
        } else if (key == SOLAR_OS_KEY_RIGHT) {
            network_tui_change_priority(path, 1);
        } else if (enter) {
            network_tui_set_status("left/right changes priority");
        }
        return;
    }

    if (selected->type != NETWORK_TUI_SETTING_ROUTING) {
        return;
    }
    solar_os_network_router_status_t router = {0};
    solar_os_network_router_get_status(&router);
    if (key == SOLAR_OS_KEY_LEFT) {
        network_tui_set_routing(false);
    } else if (key == SOLAR_OS_KEY_RIGHT) {
        network_tui_set_routing(true);
    } else if (enter) {
        network_tui_set_routing(!router.enabled);
    }
}

static esp_err_t network_tui_start(solar_os_context_t *ctx)
{
    memset(&network_tui, 0, sizeof(network_tui));
    network_tui.ctx = ctx;
    const esp_err_t ret = solar_os_tui_screen_begin(&network_tui.tui, ctx);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_tui_set_cursor_visible(&network_tui.tui, false);
    network_tui_render();
    return ESP_OK;
}

static void network_tui_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&network_tui.tui, true);
    solar_os_tui_refresh(&network_tui.tui);
}

static void network_tui_resume(solar_os_context_t *ctx)
{
    network_tui.ctx = ctx;
    solar_os_tui_set_cursor_visible(&network_tui.tui, false);
    network_tui_render();
}

static void network_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&network_tui.tui, true);
    solar_os_tui_clear(&network_tui.tui);
    solar_os_tui_refresh(&network_tui.tui);
    solar_os_tui_end(&network_tui.tui);
}

static bool network_tui_event(solar_os_context_t *ctx,
                              const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        network_tui_render();
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if (network_tui.last_refresh_ms == 0U ||
            now_ms - network_tui.last_refresh_ms >= NETWORK_TUI_REFRESH_MS) {
            network_tui.last_refresh_ms = now_ms;
            network_tui_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_finish(network_tui.ctx, 0, NULL);
        return true;
    }
    if (key == '\t') {
        network_tui.tab = network_tui.tab == NETWORK_TUI_TAB_STATUS ?
            NETWORK_TUI_TAB_SETTINGS : NETWORK_TUI_TAB_STATUS;
        network_tui_set_status("");
        network_tui_render();
        return true;
    }
    if (network_tui.tab == NETWORK_TUI_TAB_STATUS) {
        network_tui_handle_status_key(key);
    } else {
        network_tui_handle_settings_key(key);
    }
    network_tui_render();
    return true;
}

static const solar_os_app_t network_tui_app = {
    .name = "network",
    .summary = "Network status and routing settings",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = network_tui_start,
    .suspend = network_tui_suspend,
    .resume = network_tui_resume,
    .stop = network_tui_stop,
    .event = network_tui_event,
    .state_slot = &network_tui_state,
    .state_size = sizeof(network_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};

esp_err_t solar_os_shell_launch_network_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &network_tui_app, 0, NULL);
}
