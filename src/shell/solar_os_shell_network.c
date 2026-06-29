#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell_tui_apps.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_config.h"
#include "solar_os_keys.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_net.h"
#endif
#include "solar_os_port.h"
#include "solar_os_time.h"
#include "solar_os_wifi.h"

#define NETSCAN_MAX_PORTS 128
#define NETSCAN_MAX_HOSTS 256
#define NETSCAN_TIMEOUT_MS 350U

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static bool parse_size_arg(const char *text, size_t min, size_t max, size_t *value)
{
    return solar_os_shell_parse_size_arg(text, min, max, value);
}

static void wifi_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  wifi [status]");
    solar_os_shell_io_writeln(term, "  wifi on");
    solar_os_shell_io_writeln(term, "  wifi off");
    solar_os_shell_io_writeln(term, "  wifi scan");
    solar_os_shell_io_writeln(term, "  wifi connect [ssid [password]]");
    solar_os_shell_io_writeln(term, "  wifi disconnect");
    solar_os_shell_io_writeln(term, "  wifi known");
    solar_os_shell_io_writeln(term, "  wifi forget [ssid|all]");
    solar_os_shell_io_writeln(term, "  wifi nat [status|on|off]");
    solar_os_shell_io_writeln(term, "  wifi ap [status]");
    solar_os_shell_io_writeln(term, "  wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]");
    solar_os_shell_io_writeln(term, "  wifi ap off");
}

static void wifi_print_nat_status(solar_os_shell_io_t *term, const solar_os_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if (!status->nat_enabled) {
        solar_os_shell_io_writeln(term, "NAT: off");
        return;
    }
    if (status->nat_active) {
        solar_os_shell_io_writeln(term, "NAT: active");
        return;
    }
    if (status->nat_last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "NAT: error %s\n",
                                 esp_err_to_name(status->nat_last_error));
        return;
    }

    solar_os_shell_io_writeln(term, "NAT: waiting for APSTA link");
}

static void wifi_print_status(solar_os_shell_io_t *term)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    solar_os_shell_io_printf(term,
                             "WiFi: %s%s\n",
                             solar_os_wifi_state_name(status.state),
                             status.started ? "" : " (radio off)");
    if (status.ssid[0] != '\0') {
        solar_os_shell_io_printf(term, "SSID: %s\n", status.ssid);
    }
    if (status.has_ip) {
        solar_os_shell_io_printf(term, "IP: %s\n", status.ip);
        solar_os_shell_io_printf(term, "Gateway: %s\n", status.gateway);
        solar_os_shell_io_printf(term, "Netmask: %s\n", status.netmask);
    }
    if (status.connected) {
        solar_os_shell_io_printf(term,
                                 "Link: ch %u, RSSI %d dBm\n",
                                 (unsigned)status.channel,
                                 (int)status.rssi);
    }
    if (status.has_saved_config) {
        solar_os_shell_io_printf(term,
                                 "Saved: %u, preferred %s\n",
                                 (unsigned)status.saved_profile_count,
                                 status.saved_ssid);
    } else {
        solar_os_shell_io_writeln(term, "Saved: none");
    }
    if (status.has_saved_ap_config) {
        solar_os_shell_io_printf(term,
                                 "Saved AP: %s (%s)\n",
                                 status.saved_ap_ssid,
                                 status.saved_ap_auth[0] != '\0' ? status.saved_ap_auth : "open");
    } else {
        solar_os_shell_io_writeln(term, "Saved AP: none");
    }
    if (status.ap_enabled || status.ap_running) {
        solar_os_shell_io_printf(term, "AP: %s\n", status.ap_running ? "on" : "starting");
        if (status.ap_ssid[0] != '\0') {
            solar_os_shell_io_printf(term, "AP SSID: %s\n", status.ap_ssid);
        }
        if (status.ap_ip[0] != '\0') {
            solar_os_shell_io_printf(term, "AP IP: %s\n", status.ap_ip);
        }
        solar_os_shell_io_printf(term,
                                 "AP Link: ch %u, %s, clients %u/%u\n",
                                 (unsigned)status.ap_channel,
                                 status.ap_auth[0] != '\0' ? status.ap_auth : "open",
                                 (unsigned)status.ap_station_count,
                                 (unsigned)status.ap_max_connections);
    } else {
        solar_os_shell_io_writeln(term, "AP: off");
    }
    wifi_print_nat_status(term, &status);
    if (status.disconnect_reason != 0) {
        solar_os_shell_io_printf(term,
                                 "Last disconnect reason: %u\n",
                                 (unsigned)status.disconnect_reason);
    }
}

static void wifi_cmd_scan(solar_os_shell_io_t *term)
{
    solar_os_wifi_ap_t aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t found = 0;
    const esp_err_t err = solar_os_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);

    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi scan failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "RSSI CH AUTH       K SSID");
    for (size_t i = 0; i < found; i++) {
        const bool known = !aps[i].hidden && solar_os_wifi_is_known_ssid(aps[i].ssid);
        solar_os_shell_io_printf(term,
                                 "%4d %2u %-10s %c %s\n",
                                 (int)aps[i].rssi,
                                 (unsigned)aps[i].channel,
                                 aps[i].auth,
                                 known ? '*' : '-',
                                 aps[i].ssid);
    }
    solar_os_shell_io_printf(term,
                             "%u network%s shown\n",
                             (unsigned)found,
                             found == 1 ? "" : "s");
}

static void wifi_cmd_known(solar_os_shell_io_t *term)
{
    solar_os_wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;
    const esp_err_t err = solar_os_wifi_known(profiles,
                                              sizeof(profiles) / sizeof(profiles[0]),
                                              &count);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi known failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (count == 0) {
        solar_os_shell_io_writeln(term, "no known networks");
        return;
    }

    solar_os_shell_io_writeln(term, "P SSID");
    const size_t shown = count < SOLAR_OS_WIFI_PROFILE_MAX ? count : SOLAR_OS_WIFI_PROFILE_MAX;
    for (size_t i = 0; i < shown; i++) {
        solar_os_shell_io_printf(term,
                                 "%c %s\n",
                                 profiles[i].preferred ? '*' : '-',
                                 profiles[i].ssid);
    }
    if (count > shown) {
        solar_os_shell_io_printf(term, "%u more not shown\n", (unsigned)(count - shown));
    }
}

static void wifi_cmd_ap(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: wifi ap [status]");
            return;
        }
        wifi_print_status(term);
        return;
    }

    if (strcmp(argv[2], "on") == 0) {
        if (argc > 6) {
            solar_os_shell_io_writeln(
                term,
                "usage: wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]");
            return;
        }

        const char *ssid = argc >= 4 ? argv[3] : NULL;
        const char *password = argc >= 5 ? argv[4] : NULL;
        const char *auth = argc >= 6 ? argv[5] : NULL;
        const esp_err_t err = solar_os_wifi_ap_start(ssid, password, auth);
        if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "WiFi AP on: %s (%s)\n",
                                     status.ap_ssid,
                                     status.ap_auth[0] != '\0' ? status.ap_auth : "open");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "wifi ap: WEP is not supported in SoftAP mode");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "wifi ap: invalid SSID, password, or auth mode");
        } else {
            solar_os_shell_io_printf(term, "wifi ap on failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[2], "off") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: wifi ap off");
            return;
        }

        const esp_err_t err = solar_os_wifi_ap_stop();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi AP off");
        } else {
            solar_os_shell_io_printf(term, "wifi ap off failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    wifi_print_usage(term);
}

static void wifi_cmd_nat(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
            return;
        }
        solar_os_wifi_status_t status;
        solar_os_wifi_get_status(&status);
        wifi_print_nat_status(term, &status);
        return;
    }

    if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
            return;
        }

        const bool enabled = strcmp(argv[2], "on") == 0;
        const esp_err_t err = solar_os_wifi_nat_set(enabled);
        if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            wifi_print_nat_status(term, &status);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "wifi nat: NAT is not supported in this build");
        } else {
            solar_os_shell_io_printf(term, "wifi nat failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
}

static void wifi_cmd_connect(solar_os_shell_io_t *term, int argc, char **argv)
{
    esp_err_t err;

    if (argc == 2) {
        err = solar_os_wifi_connect_saved();
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "wifi: no saved network");
        } else if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "WiFi connecting to %s\n",
                                     status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
        } else {
            solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (argc < 3 || argc > 4) {
        solar_os_shell_io_writeln(term, "usage: wifi connect [ssid [password]]");
        return;
    }

    const char *ssid = argv[2];
    const char *password = argc == 4 ? argv[3] : "";
    err = solar_os_wifi_connect(ssid, password);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "WiFi connecting to %s\n", ssid);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "wifi: invalid SSID or password length");
    } else {
        solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
    }
}

static void wifi_cmd_on(solar_os_shell_io_t *term)
{
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi on failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    if (status.connected || status.state == SOLAR_OS_WIFI_STATE_CONNECTING) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
        return;
    }

    if (!status.has_saved_config) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
        return;
    }

    err = solar_os_wifi_connect_saved();
    if (err == ESP_OK) {
        solar_os_wifi_get_status(&status);
        solar_os_shell_io_printf(term,
                                 "WiFi radio on, connecting to %s\n",
                                 status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
    } else {
        solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
    }
}

void solar_os_shell_cmd_wifi(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1) {
        if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term, "wifi: TUI is only available on the display shell");
            solar_os_shell_io_writeln(term, "usage: wifi status|on|off|scan|connect|disconnect|known|forget|ap|nat");
            return;
        }
        const esp_err_t err = solar_os_shell_launch_wifi_tui(ctx);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "wifi: launch failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        wifi_print_status(term);
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        wifi_cmd_on(term);
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        const esp_err_t err = solar_os_wifi_stop();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi radio off");
        } else {
            solar_os_shell_io_printf(term, "wifi off failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "ap") == 0) {
        wifi_cmd_ap(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "nat") == 0) {
        wifi_cmd_nat(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: wifi scan");
            return;
        }
        wifi_cmd_scan(term);
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        wifi_cmd_connect(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        const esp_err_t err = solar_os_wifi_disconnect();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi disconnected");
        } else {
            solar_os_shell_io_printf(term, "wifi disconnect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "known") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: wifi known");
            return;
        }
        wifi_cmd_known(term);
        return;
    }

    if (strcmp(argv[1], "forget") == 0) {
        esp_err_t err;
        bool forgetting_all = false;
        if (argc == 2) {
            err = solar_os_wifi_forget();
        } else if (argc == 3 && strcmp(argv[2], "all") == 0) {
            forgetting_all = true;
            err = solar_os_wifi_forget_all();
        } else if (argc == 3) {
            err = solar_os_wifi_forget_ssid(argv[2]);
        } else {
            solar_os_shell_io_writeln(term, "usage: wifi forget [ssid|all]");
            return;
        }

        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term,
                                      forgetting_all ? "WiFi profiles forgotten" : "WiFi profile forgotten");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "wifi: profile not found");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "wifi: invalid SSID");
        } else {
            solar_os_shell_io_printf(term, "wifi forget failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    wifi_print_usage(term);
}

#if SOLAR_OS_PACKAGE_NET
static void ping_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: ping <host> [count]");
    solar_os_shell_io_printf(term,
                             "%s stops a running ping\n",
                             solar_os_shell_io_app_exit_key(term));
}

static bool shell_read_app_exit_key(void *user)
{
    solar_os_shell_io_t *term = (solar_os_shell_io_t *)user;
    char chars[8];
    size_t count;

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            const uint8_t ch = (uint8_t)chars[i];
            if (ch == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    }

    if (term == NULL ||
        solar_os_shell_io_kind(term) != SOLAR_OS_SHELL_IO_KIND_PORT ||
        !solar_os_port_handle_valid(&term->port)) {
        return false;
    }

    uint8_t port_chars[8];
    do {
        count = 0;
        const esp_err_t err = solar_os_port_read(&term->port,
                                                 port_chars,
                                                 sizeof(port_chars),
                                                 0,
                                                 &count);
        if (err != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (port_chars[i] == 0x1d ||
                port_chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    } while (count > 0);

    return false;
}

static void ping_print_event(const solar_os_net_ping_event_t *event, void *user)
{
    solar_os_shell_io_t *term = (solar_os_shell_io_t *)user;

    if (event == NULL || term == NULL) {
        return;
    }

    if (event->type == SOLAR_OS_NET_PING_REPLY) {
        solar_os_shell_io_printf(term,
                                 "%" PRIu32 "B from %s seq=%u ttl=%u time=%" PRIu32 "ms\n",
                                 event->bytes,
                                 event->from,
                                 (unsigned)event->seqno,
                                 (unsigned)event->ttl,
                                 event->elapsed_ms);
    } else {
        solar_os_shell_io_printf(term,
                                 "timeout from %s seq=%u\n",
                                 event->from,
                                 (unsigned)event->seqno);
    }
    solar_os_shell_io_flush(term);
}

void solar_os_shell_cmd_ping(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    size_t count = SOLAR_OS_NET_PING_FOREVER;

    if (argc < 2 || argc > 3) {
        ping_print_usage(term);
        return;
    }

    if (argc == 3 &&
        !parse_size_arg(argv[2], 1, SOLAR_OS_NET_PING_MAX_COUNT, &count)) {
        solar_os_shell_io_printf(term,
                                 "ping count: 1..%u\n",
                                 (unsigned)SOLAR_OS_NET_PING_MAX_COUNT);
        return;
    }

    const char *host = argv[1];
    solar_os_net_ping_options_t options = {
        .count = (uint32_t)count,
    };
    solar_os_net_ping_result_t result;

    if (count == SOLAR_OS_NET_PING_FOREVER) {
        solar_os_shell_io_printf(term,
                                 "ping %s, %s to stop\n",
                                 host,
                                 solar_os_shell_io_app_exit_key(term));
    } else {
        solar_os_shell_io_printf(term, "ping %s (%u packets)\n", host, (unsigned)count);
    }
    solar_os_shell_io_flush(term);

    const esp_err_t err = solar_os_net_ping(host,
                                            &options,
                                            ping_print_event,
                                            term,
                                            shell_read_app_exit_key,
                                            term,
                                            &result);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ping: WiFi not connected");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "ping: unknown host: %s\n", host);
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        ping_print_usage(term);
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ping failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (result.interrupted) {
        solar_os_shell_io_writeln(term, "ping: stopped");
    }
    solar_os_shell_io_printf(term,
                             "%" PRIu32 " tx, %" PRIu32 " rx, %" PRIu32 "%% loss, %" PRIu32 "ms\n",
                             result.transmitted,
                             result.received,
                             result.loss_percent,
                             result.total_time_ms);
    if (result.received > 0) {
        solar_os_shell_io_printf(term,
                                 "rtt min/avg/max %" PRIu32 "/%" PRIu32 "/%" PRIu32 " ms\n",
                                 result.min_time_ms,
                                 result.avg_time_ms,
                                 result.max_time_ms);
    }
}

static void netscan_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: netscan <host|range> [ports]");
    solar_os_shell_io_writeln(term, "  target: host, 192.168.1.20, 192.168.1.1-32, 192.168.1.0/24");
    solar_os_shell_io_writeln(term, "  ports: 22,80,443 or 1-128");
    solar_os_shell_io_writeln(term, "  default: 22,80,443,1883,8080");
    solar_os_shell_io_printf(term, "%s stops a running scan\n", solar_os_shell_io_app_exit_key(term));
}

typedef struct {
    bool range;
    char label[SOLAR_OS_NET_HOST_MAX];
    char single_ip[SOLAR_OS_NET_ADDR_MAX];
    uint8_t prefix[3];
    uint8_t first;
    uint8_t last;
    size_t count;
} netscan_target_spec_t;

static bool netscan_parse_ipv4_octet(const char **cursor, uint8_t *octet)
{
    if (cursor == NULL || *cursor == NULL || octet == NULL || !isdigit((unsigned char)**cursor)) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(*cursor, &end, 10);
    if (errno != 0 || end == *cursor || parsed > 255UL) {
        return false;
    }

    *octet = (uint8_t)parsed;
    *cursor = end;
    return true;
}

static bool netscan_parse_ipv4_address(const char *text, uint8_t octets[4], const char **end)
{
    if (text == NULL || octets == NULL) {
        return false;
    }

    const char *cursor = text;
    for (size_t i = 0; i < 4; i++) {
        if (!netscan_parse_ipv4_octet(&cursor, &octets[i])) {
            return false;
        }
        if (i < 3) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        }
    }

    if (end != NULL) {
        *end = cursor;
    }
    return true;
}

static void netscan_format_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    snprintf(buffer,
             len,
             "%u.%u.%u.%u",
             (unsigned)a,
             (unsigned)b,
             (unsigned)c,
             (unsigned)d);
}

static bool netscan_target_set_range(netscan_target_spec_t *target,
                                     const uint8_t octets[4],
                                     uint8_t first,
                                     uint8_t last)
{
    if (target == NULL || octets == NULL || last < first) {
        return false;
    }

    const size_t count = (size_t)last - (size_t)first + 1U;
    if (count == 0 || count > NETSCAN_MAX_HOSTS) {
        return false;
    }

    target->range = true;
    target->prefix[0] = octets[0];
    target->prefix[1] = octets[1];
    target->prefix[2] = octets[2];
    target->first = first;
    target->last = last;
    target->count = count;
    snprintf(target->label,
             sizeof(target->label),
             "%u.%u.%u.%u-%u",
             (unsigned)target->prefix[0],
             (unsigned)target->prefix[1],
             (unsigned)target->prefix[2],
             (unsigned)first,
             (unsigned)last);
    return true;
}

static esp_err_t netscan_parse_target(const char *text, netscan_target_spec_t *target)
{
    if (text == NULL || text[0] == '\0' || target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(target, 0, sizeof(*target));
    strlcpy(target->label, text, sizeof(target->label));

    uint8_t octets[4] = {0};
    const char *end = NULL;
    if (netscan_parse_ipv4_address(text, octets, &end)) {
        if (*end == '\0') {
            target->range = false;
            target->count = 1;
            netscan_format_ipv4(octets[0],
                                octets[1],
                                octets[2],
                                octets[3],
                                target->single_ip,
                                sizeof(target->single_ip));
            return ESP_OK;
        }

        if (*end == '/') {
            char *mask_end = NULL;
            errno = 0;
            unsigned long mask = strtoul(end + 1, &mask_end, 10);
            if (errno != 0 || mask_end == end + 1 || *mask_end != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            if (mask != 24UL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            return netscan_target_set_range(target, octets, 1, 254) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }

        if (*end == '-') {
            const char *range_end = NULL;
            uint8_t last_octets[4] = {0};
            if (netscan_parse_ipv4_address(end + 1, last_octets, &range_end)) {
                if (*range_end != '\0' ||
                    last_octets[0] != octets[0] ||
                    last_octets[1] != octets[1] ||
                    last_octets[2] != octets[2]) {
                    return ESP_ERR_INVALID_ARG;
                }
                return netscan_target_set_range(target, octets, octets[3], last_octets[3])
                           ? ESP_OK
                           : ESP_ERR_INVALID_SIZE;
            }

            const char *last_cursor = end + 1;
            uint8_t last = 0;
            if (!netscan_parse_ipv4_octet(&last_cursor, &last) || *last_cursor != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            return netscan_target_set_range(target, octets, octets[3], last)
                       ? ESP_OK
                       : ESP_ERR_INVALID_SIZE;
        }

        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t resolve_err = solar_os_net_resolve_host(text,
                                                            target->single_ip,
                                                            sizeof(target->single_ip));
    if (resolve_err != ESP_OK) {
        return resolve_err;
    }
    target->range = false;
    target->count = 1;
    return ESP_OK;
}

static void netscan_target_ip(const netscan_target_spec_t *target,
                              size_t index,
                              char *ip,
                              size_t ip_len)
{
    if (target == NULL || ip == NULL || ip_len == 0) {
        return;
    }

    if (!target->range) {
        strlcpy(ip, target->single_ip, ip_len);
        return;
    }

    const uint8_t host = (uint8_t)((size_t)target->first + index);
    netscan_format_ipv4(target->prefix[0],
                        target->prefix[1],
                        target->prefix[2],
                        host,
                        ip,
                        ip_len);
}

static bool netscan_parse_port_value(const char *text, char **end, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }

    errno = 0;
    unsigned long parsed = strtoul(text, end, 10);
    if (errno != 0 || *end == text || parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }

    *port = (uint16_t)parsed;
    return true;
}

static bool netscan_add_port(uint16_t *ports, size_t *count, uint16_t port)
{
    if (ports == NULL || count == NULL || port == 0) {
        return false;
    }

    for (size_t i = 0; i < *count; i++) {
        if (ports[i] == port) {
            return true;
        }
    }
    if (*count >= NETSCAN_MAX_PORTS) {
        return false;
    }

    ports[(*count)++] = port;
    return true;
}

static bool netscan_parse_ports(const char *text, uint16_t *ports, size_t *count)
{
    if (ports == NULL || count == NULL) {
        return false;
    }

    *count = 0;
    if (text == NULL || text[0] == '\0') {
        static const uint16_t defaults[] = {22, 80, 443, 1883, 8080};
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            if (!netscan_add_port(ports, count, defaults[i])) {
                return false;
            }
        }
        return true;
    }

    char buffer[192];
    if (strlcpy(buffer, text, sizeof(buffer)) >= sizeof(buffer)) {
        return false;
    }

    char *saveptr = NULL;
    char *token = strtok_r(buffer, ",", &saveptr);
    while (token != NULL) {
        uint16_t first = 0;
        uint16_t last = 0;
        char *end = NULL;
        if (!netscan_parse_port_value(token, &end, &first)) {
            return false;
        }
        if (*end == '-') {
            if (!netscan_parse_port_value(end + 1, &end, &last) || last < first) {
                return false;
            }
        } else {
            last = first;
        }
        if (*end != '\0') {
            return false;
        }

        for (uint32_t port = first; port <= last; port++) {
            if (!netscan_add_port(ports, count, (uint16_t)port)) {
                return false;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    return *count > 0;
}

static const char *netscan_service_name(uint16_t port)
{
    switch (port) {
    case 21:
        return "ftp";
    case 22:
        return "ssh";
    case 23:
        return "telnet";
    case 25:
        return "smtp";
    case 53:
        return "dns";
    case 80:
        return "http";
    case 110:
        return "pop3";
    case 143:
        return "imap";
    case 443:
        return "https";
    case 587:
        return "submission";
    case 993:
        return "imaps";
    case 995:
        return "pop3s";
    case 1883:
        return "mqtt";
    case 3306:
        return "mysql";
    case 5432:
        return "postgres";
    case 8080:
        return "http-alt";
    default:
        return "";
    }
}

static bool netscan_probe_tcp(const char *ip, uint16_t port, uint32_t timeout_ms, uint32_t *elapsed_ms)
{
    if (elapsed_ms != NULL) {
        *elapsed_ms = 0;
    }

    const TickType_t start = xTaskGetTickCount();
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {
            .s_addr = inet_addr(ip),
        },
    };

    bool open = false;
    int rc = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        open = true;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        struct timeval timeout = {
            .tv_sec = (time_t)(timeout_ms / 1000U),
            .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
        };

        rc = select(sock + 1, NULL, &writefds, NULL, &timeout);
        if (rc > 0 && FD_ISSET(sock, &writefds)) {
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);
            if (getsockopt(sock,
                           SOL_SOCKET,
                           SO_ERROR,
                           &so_error,
                           &so_error_len) == 0 &&
                so_error == 0) {
                open = true;
            }
        }
    }

    close(sock);
    if (elapsed_ms != NULL) {
        *elapsed_ms = (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
    }
    return open;
}

static void netscan_update_progress(solar_os_shell_io_t *term,
                                    size_t row,
                                    const char *ip,
                                    uint16_t port,
                                    size_t probe_index,
                                    size_t total_probes)
{
    static const char frames[] = "|/-\\";
    const char frame = frames[probe_index % (sizeof(frames) - 1U)];

    solar_os_shell_io_set_cursor(term, row, 0);
    solar_os_shell_io_clear_line_from(term, row, 0);
    solar_os_shell_io_printf(term,
                             "%c %s:%u %u/%u",
                             frame,
                             ip,
                             (unsigned)port,
                             (unsigned)(probe_index + 1U),
                             (unsigned)total_probes);
    solar_os_shell_io_flush(term);
}

static void netscan_clear_progress(solar_os_shell_io_t *term, size_t row)
{
    solar_os_shell_io_set_cursor(term, row, 0);
    solar_os_shell_io_clear_line_from(term, row, 0);
}

void solar_os_shell_cmd_netscan(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2 || argc > 3) {
        netscan_print_usage(term);
        return;
    }

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip) {
        solar_os_shell_io_writeln(term, "netscan: WiFi not connected");
        return;
    }

    uint16_t ports[NETSCAN_MAX_PORTS];
    size_t port_count = 0;
    if (!netscan_parse_ports(argc == 3 ? argv[2] : NULL, ports, &port_count)) {
        solar_os_shell_io_printf(term,
                                 "netscan: invalid ports or too many ports, max %u\n",
                                 (unsigned)NETSCAN_MAX_PORTS);
        return;
    }

    netscan_target_spec_t target;
    const esp_err_t target_err = netscan_parse_target(argv[1], &target);
    if (target_err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "netscan: unknown host: %s\n", argv[1]);
        return;
    }
    if (target_err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "netscan: only IPv4 /24 ranges are supported");
        return;
    }
    if (target_err == ESP_ERR_INVALID_SIZE) {
        solar_os_shell_io_printf(term,
                                 "netscan: too many hosts, max %u\n",
                                 (unsigned)NETSCAN_MAX_HOSTS);
        return;
    }
    if (target_err != ESP_OK) {
        solar_os_shell_io_printf(term, "netscan: invalid target: %s\n", esp_err_to_name(target_err));
        return;
    }

    char first_ip[SOLAR_OS_NET_ADDR_MAX];
    netscan_target_ip(&target, 0, first_ip, sizeof(first_ip));
    solar_os_shell_io_printf(term,
                             "netscan %s (%s), %u host%s, %u ports, %s to stop\n",
                             target.label,
                             first_ip,
                             (unsigned)target.count,
                             target.count == 1 ? "" : "s",
                             (unsigned)port_count,
                             solar_os_shell_io_app_exit_key(term));
    solar_os_shell_io_writeln(term, "HOST             PORT     STATE  SERVICE");
    solar_os_shell_io_flush(term);

    size_t open_count = 0;
    size_t probe_count = 0;
    size_t progress_row = solar_os_shell_io_cursor_row(term);
    const size_t total_probes = target.count * port_count;
    const bool cursor_was_visible = solar_os_shell_io_cursor_visible(term);
    solar_os_shell_io_set_cursor_visible(term, false);
    bool stopped = false;
    for (size_t host_index = 0; host_index < target.count; host_index++) {
        char ip[SOLAR_OS_NET_ADDR_MAX];
        netscan_target_ip(&target, host_index, ip, sizeof(ip));

        for (size_t port_index = 0; port_index < port_count; port_index++) {
            if (shell_read_app_exit_key(term)) {
                stopped = true;
                break;
            }

            uint32_t elapsed_ms = 0;
            const uint16_t port = ports[port_index];
            netscan_update_progress(term, progress_row, ip, port, probe_count, total_probes);
            probe_count++;
            if (netscan_probe_tcp(ip, port, NETSCAN_TIMEOUT_MS, &elapsed_ms)) {
                netscan_clear_progress(term, progress_row);
                solar_os_shell_io_printf(term,
                                         "%-15s %-8u open   %s",
                                         ip,
                                         (unsigned)port,
                                         netscan_service_name(port));
                if (elapsed_ms > 0) {
                    solar_os_shell_io_printf(term, " (%" PRIu32 "ms)", elapsed_ms);
                }
                solar_os_shell_io_put_char(term, '\n');
                solar_os_shell_io_flush(term);
                open_count++;
                progress_row = solar_os_shell_io_cursor_row(term);
            }

            vTaskDelay(1);
        }
        if (stopped) {
            break;
        }
    }

    netscan_clear_progress(term, progress_row);
    if (stopped) {
        solar_os_shell_io_writeln(term, "netscan: stopped");
    }
    solar_os_shell_io_printf(term,
                             "netscan: %u open, %u probes\n",
                             (unsigned)open_count,
                             (unsigned)probe_count);
    solar_os_shell_io_set_cursor_visible(term, cursor_was_visible);
}
#endif

static void print_datetime_line(solar_os_shell_io_t *term,
                                const char *label,
                                const solar_os_datetime_t *datetime)
{
    solar_os_shell_io_printf(term,
                             "%s: %04u-%02u-%02u %02u:%02u:%02u\n",
                             label,
                             (unsigned)datetime->year,
                             (unsigned)datetime->month,
                             (unsigned)datetime->day,
                             (unsigned)datetime->hour,
                             (unsigned)datetime->minute,
                             (unsigned)datetime->second);
}

void solar_os_shell_cmd_ntp(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc > 2) {
        solar_os_shell_io_writeln(term, "usage: ntp [server]");
        return;
    }

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.has_ip) {
        solar_os_shell_io_writeln(term, "ntp: WiFi is not connected");
        return;
    }

    const char *server = argc == 2 ? argv[1] : SOLAR_OS_NTP_DEFAULT_SERVER;
    solar_os_shell_io_printf(term, "ntp: syncing with %s\n", server);

    solar_os_datetime_t utc;
    solar_os_datetime_t local;
    const esp_err_t err = solar_os_time_ntp_sync(server,
                                                 SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS,
                                                 &utc,
                                                 &local);
    if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_writeln(term, "ntp: sync timed out");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "usage: ntp [server]");
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "ntp: RTC not available on this board");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ntp: sync failed: %s\n", esp_err_to_name(err));
        return;
    }

    char timezone[SOLAR_OS_TIMEZONE_NAME_MAX];
    solar_os_time_get_timezone(timezone, sizeof(timezone), NULL, 0);
    print_datetime_line(term, "UTC", &utc);
    print_datetime_line(term, timezone, &local);
}
