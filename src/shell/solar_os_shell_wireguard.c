#include "solar_os_shell_commands.h"

#include <string.h>

#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_wireguard.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void wireguard_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  wireguard [status]");
    solar_os_shell_io_writeln(term, "  wireguard import <file>");
    solar_os_shell_io_writeln(term, "  wireguard forget");
    solar_os_shell_io_writeln(term, "  wireguard up [fail-open|fail-closed]");
    solar_os_shell_io_writeln(term, "  wireguard down");
}

static void wireguard_print_status(solar_os_shell_io_t *term)
{
    solar_os_wireguard_status_t status;
    solar_os_wireguard_get_status(&status);

    solar_os_shell_io_printf(term,
                             "WireGuard: %s%s\n",
                             solar_os_wireguard_state_name(status.state),
                             status.desired_up ? " (requested)" : "");
    solar_os_shell_io_printf(term,
                             "Profile: %s\n",
                             status.configured ? "configured" : "none");
    if (!status.configured) {
        return;
    }
    solar_os_shell_io_printf(term, "Address: %s\n", status.address);
    solar_os_shell_io_printf(term,
                             "Underlay: %s\n",
                             status.underlay[0] != '\0' ?
                                 status.underlay : "none");
    if (status.endpoint_ip[0] != '\0') {
        solar_os_shell_io_printf(term,
                                 "Endpoint: %s:%u (%s)\n",
                                 status.endpoint,
                                 (unsigned)status.endpoint_port,
                                 status.endpoint_ip);
    } else {
        solar_os_shell_io_printf(term,
                                 "Endpoint: %s:%u\n",
                                 status.endpoint,
                                 (unsigned)status.endpoint_port);
    }
    solar_os_shell_io_printf(term,
                             "Peer key: %s (fingerprint only)\n",
                             status.peer_key_fingerprint[0] != '\0' ?
                                 status.peer_key_fingerprint : "-");
    solar_os_shell_io_printf(term,
                             "Routes: %u%s\n",
                             (unsigned)status.route_count,
                             status.full_tunnel ? " (full tunnel)" : "");
    solar_os_shell_io_printf(term,
                             "Policy: %s%s\n",
                             solar_os_wireguard_policy_name(status.policy),
                             status.kill_switch_active ? " (kill switch active)" : "");
    solar_os_shell_io_printf(term,
                             "Peer: %s, keepalive %u s, MTU %u\n",
                             status.peer_up ? "up" : "down",
                             (unsigned)status.keepalive_seconds,
                             (unsigned)status.mtu);
    if (status.dns_configured) {
        solar_os_shell_io_printf(term, "DNS: %s\n", status.dns);
    }
    if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "Last error: %s\n",
                                 solar_os_shell_error_text(status.last_error));
    }
}

void solar_os_shell_cmd_wireguard(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        wireguard_print_status(term);
        return;
    }

    if (argc == 3 && strcmp(argv[1], "import") == 0) {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        if (!solar_os_shell_resolve_path_for_command(ctx,
                                                     term,
                                                     "wireguard import",
                                                     argv[2],
                                                     path,
                                                     sizeof(path))) {
            return;
        }
        char detail[128] = {0};
        const esp_err_t error = solar_os_wireguard_import(path, detail, sizeof(detail));
        if (error == ESP_OK) {
            solar_os_shell_io_printf(term, "WireGuard: %s\n", detail);
        } else {
            solar_os_shell_io_printf(term,
                                     "wireguard import failed: %s%s%s\n",
                                     solar_os_shell_error_text(error),
                                     detail[0] != '\0' ? ": " : "",
                                     detail);
        }
        return;
    }

    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        const esp_err_t error = solar_os_wireguard_forget();
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(term, "WireGuard: profile forgotten");
        } else if (error == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "wireguard forget: bring the tunnel down first");
        } else {
            solar_os_shell_io_printf(term,
                                     "wireguard forget failed: %s\n",
                                     solar_os_shell_error_text(error));
        }
        return;
    }

    if ((argc == 2 || argc == 3) && strcmp(argv[1], "up") == 0) {
        solar_os_wireguard_policy_t policy = SOLAR_OS_WIREGUARD_POLICY_AUTO;
        if (argc == 3) {
            if (strcmp(argv[2], "fail-open") == 0) {
                policy = SOLAR_OS_WIREGUARD_POLICY_FAIL_OPEN;
            } else if (strcmp(argv[2], "fail-closed") == 0) {
                policy = SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED;
            } else {
                solar_os_shell_diag_invalid(term,
                                            "wireguard up",
                                            "policy",
                                            argv[2],
                                            "fail-open or fail-closed",
                                            "wireguard up [fail-open|fail-closed]",
                                            false);
                return;
            }
        }
        const esp_err_t error = solar_os_wireguard_up(policy);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(term, "WireGuard: connection requested");
        } else {
            solar_os_shell_io_printf(term,
                                     "wireguard up failed: %s\n",
                                     solar_os_shell_error_text(error));
        }
        return;
    }

    if (argc == 2 && strcmp(argv[1], "down") == 0) {
        const esp_err_t error = solar_os_wireguard_down();
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(term, "WireGuard: down");
        } else {
            solar_os_shell_io_printf(term,
                                     "wireguard down failed: %s\n",
                                     solar_os_shell_error_text(error));
        }
        return;
    }

    wireguard_print_usage(term);
}
