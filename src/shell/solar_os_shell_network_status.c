#include "solar_os_shell_commands.h"

#include <stdio.h>
#include <string.h>

#include "esp_netif.h"
#include "solar_os_config.h"
#include "solar_os_network.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell_tui_apps.h"
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
#include "solar_os_wireguard.h"
#endif

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void network_interface_detail(
    const solar_os_network_interface_info_t *interface,
    char *detail,
    size_t detail_len)
{
    if (interface->role == SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK) {
        (void)snprintf(detail,
                       detail_len,
                       "uplink priority %d",
                       interface->route_priority);
    } else if (interface->role ==
               SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM) {
        (void)snprintf(detail,
                       detail_len,
                       "downstream%s",
                       interface->nat_enabled ? " NAT" : "");
    } else {
        strlcpy(detail,
                solar_os_network_interface_role_name(interface->role),
                detail_len);
    }
}

static void network_print_interfaces(solar_os_shell_io_t *term)
{
    solar_os_network_interface_info_t
        interfaces[SOLAR_OS_NETWORK_INTERFACE_MAX];
    const size_t interface_count = solar_os_network_interface_list(
        interfaces,
        sizeof(interfaces) / sizeof(interfaces[0]));
    solar_os_network_path_info_t preferred = {0};
    const bool have_preferred = solar_os_network_path_get_preferred(&preferred);

    solar_os_shell_io_writeln(term, "Interfaces:");
    if (interface_count == 0U) {
        solar_os_shell_io_writeln(term, "  no network interfaces");
    }
    for (size_t i = 0U;
         i < interface_count && i < SOLAR_OS_NETWORK_INTERFACE_MAX;
         i++) {
        char detail[40];
        network_interface_detail(&interfaces[i], detail, sizeof(detail));
        solar_os_shell_io_printf(
            term,
            "  %-10s %-10s %-15s %s%s\n",
            interfaces[i].name,
            solar_os_network_interface_state_name(interfaces[i].state),
            interfaces[i].address[0] != '\0' ? interfaces[i].address : "-",
            detail,
            have_preferred && interfaces[i].netif == preferred.netif ?
                " (preferred base path)" : "");
    }

    solar_os_network_router_status_t router = {0};
    solar_os_network_router_get_status(&router);
    if (router.available) {
        solar_os_shell_io_printf(term,
                                 "  %-10s %-10s %-15s downstream%s\n",
                                 router.downstream,
                                 router.downstream_active ? "up" : "down",
                                 router.downstream_active && router.address[0] != '\0' ?
                                     router.address : "-",
                                 router.nat_enabled ? " NAT" : "");
    }

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    solar_os_wireguard_status_t wireguard = {0};
    solar_os_wireguard_get_status(&wireguard);
    if (wireguard.configured || wireguard.desired_up || wireguard.routes_active) {
        solar_os_shell_io_printf(term,
                                 "  %-10s %-10s %-15s tunnel (%s)\n",
                                 "wireguard",
                                 wireguard.routes_active ? "up" : "down",
                                 wireguard.routes_active ? wireguard.address : "-",
                                 solar_os_wireguard_state_name(wireguard.state));
    }
#endif
}

static void network_print_routes(solar_os_shell_io_t *term)
{
    solar_os_network_interface_info_t
        interfaces[SOLAR_OS_NETWORK_INTERFACE_MAX];
    const size_t interface_count = solar_os_network_interface_list(
        interfaces,
        sizeof(interfaces) / sizeof(interfaces[0]));
    solar_os_network_path_info_t preferred = {0};
    const bool have_preferred = solar_os_network_path_get_preferred(&preferred);

    solar_os_shell_io_writeln(term, "Routes:");
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    solar_os_wireguard_status_t wireguard = {0};
    solar_os_wireguard_get_status(&wireguard);
    const char *underlay = wireguard.underlay[0] != '\0' ?
        wireguard.underlay : "uplink";
    if (wireguard.default_route_active) {
        solar_os_shell_io_printf(term,
                                 "  default via wireguard over %s\n",
                                 underlay);
    } else if (have_preferred) {
        solar_os_shell_io_printf(term,
                                 "  default via %s (automatic)\n",
                                 preferred.name);
    } else {
        solar_os_shell_io_writeln(term, "  default unavailable");
    }
    if (wireguard.routes_active) {
        solar_os_shell_io_printf(term,
                                 "  %u VPN route%s via wireguard over %s%s\n",
                                 (unsigned)wireguard.route_count,
                                 wireguard.route_count == 1U ? "" : "s",
                                 underlay,
                                 wireguard.full_tunnel ? " (full tunnel)" : "");
    } else if (wireguard.desired_up) {
        solar_os_shell_io_printf(term,
                                 "  WireGuard routes waiting: %s\n",
                                 solar_os_wireguard_state_name(wireguard.state));
    }
#else
    if (have_preferred) {
        solar_os_shell_io_printf(term,
                                 "  default via %s (automatic)\n",
                                 preferred.name);
    } else {
        solar_os_shell_io_writeln(term, "  default unavailable");
    }
#endif

    for (size_t i = 0U;
         i < interface_count && i < SOLAR_OS_NETWORK_INTERFACE_MAX;
         i++) {
        const solar_os_network_interface_info_t *interface = &interfaces[i];
        if (interface->role == SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM) {
            if (interface->state == SOLAR_OS_NETWORK_INTERFACE_STATE_UP &&
                have_preferred) {
                solar_os_shell_io_printf(
                    term,
                    "  %s -> %s%s\n",
                    interface->name,
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
                    wireguard.default_route_active ? "wireguard" : preferred.name,
#else
                    preferred.name,
#endif
                    interface->nat_enabled ? " with NAT" : "");
            } else if (interface->state == SOLAR_OS_NETWORK_INTERFACE_STATE_UP) {
                solar_os_shell_io_printf(term,
                                         "  %s waiting for a default route\n",
                                         interface->name);
            } else {
                solar_os_shell_io_printf(
                    term,
                    "  %s %s\n",
                    interface->name,
                    solar_os_network_interface_state_name(interface->state));
            }
        } else if (interface->role == SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER) {
            solar_os_shell_io_printf(
                term,
                "  %s peer %s\n",
                interface->name,
                interface->state == SOLAR_OS_NETWORK_INTERFACE_STATE_UP &&
                        interface->peer[0] != '\0' ?
                    interface->peer :
                    solar_os_network_interface_state_name(interface->state));
        }
    }
}

static void network_print_router(solar_os_shell_io_t *term)
{
    solar_os_network_router_status_t status = {0};
    solar_os_network_router_get_status(&status);
    if (!status.available) {
        solar_os_shell_io_writeln(term,
                                  "Router: unavailable (no managed downstream)");
        return;
    }
    if (!status.enabled) {
        solar_os_shell_io_printf(term,
                                 "Router %s: off\n",
                                 status.downstream);
        return;
    }

    if (status.active) {
        solar_os_shell_io_printf(term,
                                 "Router %s: active, clients %u/%u\n",
                                 status.downstream,
                                 (unsigned)status.client_count,
                                 (unsigned)status.client_limit);
    } else if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "Router %s: error %s\n",
                                 status.downstream,
                                 solar_os_shell_error_text(status.last_error));
    } else if (!solar_os_network_path_get_preferred(NULL)) {
        solar_os_shell_io_printf(term,
                                 "Router %s: waiting for a default route\n",
                                 status.downstream);
    } else {
        solar_os_shell_io_printf(term,
                                 "Router %s: starting\n",
                                 status.downstream);
    }

    solar_os_shell_io_printf(term,
                             "  downstream %s%s%s%s\n",
                             status.downstream,
                             status.label[0] != '\0' ? " (" : "",
                             status.label[0] != '\0' ? status.label : "",
                             status.label[0] != '\0' ? ")" : "");
    if (status.address[0] != '\0') {
        solar_os_shell_io_printf(term, "  address %s\n", status.address);
    }
    if (status.nat_enabled) {
        solar_os_shell_io_writeln(term,
                                  "  IPv4 forwarding with NAT through the route table");
    }
}

static void network_print_status(solar_os_shell_io_t *term)
{
    network_print_interfaces(term);
    network_print_routes(term);
    network_print_router(term);
}

static void network_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  network");
    solar_os_shell_io_writeln(term, "  network status");
    solar_os_shell_io_writeln(term, "  network interfaces");
    solar_os_shell_io_writeln(term, "  network routes");
    solar_os_shell_io_writeln(term, "  network router [status|on|off]");
}

void solar_os_shell_cmd_network(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1) {
        const esp_err_t ret = solar_os_shell_launch_network_tui(ctx);
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "network: launch failed: %s\n",
                                     solar_os_shell_error_text(ret));
        } else {
            solar_os_shell_session_prepare_foreground_launch(ctx, true);
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        network_print_status(term);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "interfaces") == 0) {
        network_print_interfaces(term);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "routes") == 0) {
        network_print_routes(term);
        return;
    }
    if (strcmp(argv[1], "router") == 0) {
        if (argc == 2 || (argc == 3 && strcmp(argv[2], "status") == 0)) {
            network_print_router(term);
            return;
        }
        if (argc == 3 &&
            (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
            const bool enable = strcmp(argv[2], "on") == 0;
            const esp_err_t ret = enable ? solar_os_network_router_start() :
                                           solar_os_network_router_stop();
            if (ret == ESP_OK) {
                network_print_router(term);
            } else if (ret == ESP_ERR_NOT_SUPPORTED) {
                solar_os_shell_io_writeln(
                    term,
                    "network router: no downstream interface is available");
            } else if (ret == ESP_ERR_INVALID_STATE) {
                solar_os_shell_io_writeln(
                    term,
                    "network router: another forwarding mode is active");
            } else {
                solar_os_shell_io_printf(term,
                                         "network router failed: %s\n",
                                         solar_os_shell_error_text(ret));
            }
            return;
        }
    }
    network_print_usage(term);
}
