#include "solar_os_shell_commands.h"

#include <string.h>

#include "esp_netif.h"
#include "solar_os_config.h"
#include "solar_os_network.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
#include "solar_os_wireguard.h"
#endif

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void network_print_interfaces(solar_os_shell_io_t *term)
{
    solar_os_network_path_info_t paths[SOLAR_OS_NETWORK_PATH_MAX];
    const size_t path_count = solar_os_network_path_list(
        paths,
        sizeof(paths) / sizeof(paths[0]));
    solar_os_network_path_info_t preferred = {0};
    const bool have_preferred = solar_os_network_path_get_preferred(&preferred);

    solar_os_shell_io_writeln(term, "Interfaces:");
    if (path_count == 0U) {
        solar_os_shell_io_writeln(term, "  no route-capable interfaces");
    }
    for (size_t i = 0; i < path_count && i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        esp_netif_ip_info_t ip = {0};
        char address[16] = "-";
        if (paths[i].ready &&
            esp_netif_get_ip_info(paths[i].netif, &ip) == ESP_OK &&
            ip.ip.addr != 0U) {
            esp_ip4addr_ntoa(&ip.ip, address, sizeof(address));
        }
        solar_os_shell_io_printf(
            term,
            "  %-10s %-4s %-15s priority %d%s\n",
            paths[i].name,
            paths[i].ready ? "up" : "down",
            address,
            paths[i].route_priority,
            have_preferred && paths[i].netif == preferred.netif ?
                " (preferred base path)" : "");
    }

    solar_os_network_router_status_t router = {0};
    solar_os_network_router_get_status(&router);
    if (router.available) {
        solar_os_shell_io_printf(term,
                                 "  %-10s %-4s %-15s local downstream\n",
                                 router.downstream,
                                 router.downstream_active ? "up" : "down",
                                 router.downstream_active && router.address[0] != '\0' ?
                                     router.address : "-");
    }

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    solar_os_wireguard_status_t wireguard = {0};
    solar_os_wireguard_get_status(&wireguard);
    if (wireguard.configured || wireguard.desired_up || wireguard.routes_active) {
        solar_os_shell_io_printf(term,
                                 "  %-10s %-4s %-15s tunnel (%s)\n",
                                 "wireguard",
                                 wireguard.routes_active ? "up" : "down",
                                 wireguard.routes_active ? wireguard.address : "-",
                                 solar_os_wireguard_state_name(wireguard.state));
    }
#endif
}

static void network_print_routes(solar_os_shell_io_t *term)
{
    solar_os_network_path_info_t preferred = {0};
    const bool have_preferred = solar_os_network_path_get_preferred(&preferred);

    solar_os_shell_io_writeln(term, "Routes:");
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    solar_os_wireguard_status_t wireguard = {0};
    solar_os_wireguard_get_status(&wireguard);
    if (wireguard.default_route_active) {
        solar_os_shell_io_writeln(term,
                                  "  default via wireguard over wifi-sta");
    } else if (have_preferred) {
        solar_os_shell_io_printf(term,
                                 "  default via %s (automatic)\n",
                                 preferred.name);
    } else {
        solar_os_shell_io_writeln(term, "  default unavailable");
    }
    if (wireguard.routes_active) {
        solar_os_shell_io_printf(term,
                                 "  %u VPN route%s via wireguard over wifi-sta%s\n",
                                 (unsigned)wireguard.route_count,
                                 wireguard.route_count == 1U ? "" : "s",
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
}

static void network_print_router(solar_os_shell_io_t *term)
{
    solar_os_network_router_status_t status = {0};
    solar_os_network_router_get_status(&status);
    if (!status.available) {
        solar_os_shell_io_writeln(term, "Router: unavailable (no downstream interface)");
        return;
    }
    if (!status.enabled) {
        solar_os_shell_io_writeln(term, "Router: off");
        return;
    }

    if (status.active) {
        solar_os_shell_io_printf(term,
                                 "Router: active, clients %u/%u\n",
                                 (unsigned)status.client_count,
                                 (unsigned)status.client_limit);
    } else if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "Router: error %s\n",
                                 solar_os_shell_error_text(status.last_error));
    } else if (!solar_os_network_path_get_preferred(NULL)) {
        solar_os_shell_io_writeln(term, "Router: waiting for a default route");
    } else {
        solar_os_shell_io_writeln(term, "Router: starting downstream interface");
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
    solar_os_shell_io_writeln(term, "  network [status]");
    solar_os_shell_io_writeln(term, "  network interfaces");
    solar_os_shell_io_writeln(term, "  network routes");
    solar_os_shell_io_writeln(term, "  network router [status|on|off]");
}

void solar_os_shell_cmd_network(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
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
