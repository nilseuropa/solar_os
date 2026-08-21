#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_espnow.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static bool parse_link_id(const char *text, uint32_t *link_id)
{
    if (text == NULL || text[0] == '\0' || link_id == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed == 0UL || parsed >= UINT32_MAX) {
        return false;
    }
    *link_id = (uint32_t)parsed;
    return true;
}

static void print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  espnow [status]");
    solar_os_shell_io_writeln(term, "  espnow peers");
    solar_os_shell_io_writeln(term, "  espnow peer add <link-id> <mac>");
    solar_os_shell_io_writeln(term, "  espnow peer remove <link-id>");
}

static void print_status(solar_os_shell_io_t *term)
{
    solar_os_espnow_status_t status;
    solar_os_espnow_get_status(&status);
    solar_os_shell_io_printf(term,
                             "ESP-NOW: %s\n",
                             status.running ? "running" : "stopped");
    solar_os_shell_io_printf(term,
                             "Owner: %s\n",
                             status.owner[0] != '\0' ? status.owner : "-");
    if (status.running) {
        solar_os_shell_io_printf(term,
                                 "Channel: %u (%s)\n",
                                 (unsigned)status.channel,
                                 status.channel_auto ? "auto" : "fixed");
        solar_os_shell_io_printf(term,
                                 "PHY: %s\n",
                                 solar_os_espnow_phy_name(status.phy));
    } else {
        solar_os_shell_io_writeln(term, "Channel: -");
        solar_os_shell_io_writeln(term, "PHY: -");
    }
    solar_os_shell_io_printf(term,
                             "Peers: %u configured, %u learned\n",
                             (unsigned)status.configured_peer_count,
                             (unsigned)(status.peer_count - status.configured_peer_count));
    solar_os_shell_io_printf(term,
                             "TX: %" PRIu32 " sent, %" PRIu32 " errors%s\n",
                             status.tx_packets,
                             status.tx_errors,
                             status.send_inflight ? ", in flight" : "");
    solar_os_shell_io_printf(term,
                             "RX: %" PRIu32 " queued, %" PRIu32 " dropped\n",
                             status.rx_packets,
                             status.rx_dropped);
    solar_os_shell_io_printf(term,
                             "Peer conflicts: %" PRIu32 "\n",
                             status.peer_conflicts);
    if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "Last error: %s\n",
                                 solar_os_shell_error_text(status.last_error));
    }
}

static void print_peers(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_espnow_peer_count();
    if (count == 0U) {
        solar_os_shell_io_writeln(term, "no ESP-NOW peers");
        return;
    }
    solar_os_shell_io_writeln(term, "LINK ID    MAC                SOURCE      RSSI LAST MS");
    for (size_t i = 0U; i < count; i++) {
        solar_os_espnow_peer_t peer;
        if (!solar_os_espnow_peer_get(i, &peer)) {
            continue;
        }
        char mac[18];
        solar_os_espnow_format_mac(peer.mac, mac, sizeof(mac));
        solar_os_shell_io_printf(term,
                                 "0x%08" PRIx32 " %-17s %-10s %4d %7" PRIu32 "\n",
                                 peer.link_id,
                                 mac,
                                 peer.configured ? "configured" : "learned",
                                 (int)peer.rssi,
                                 peer.last_seen_ms);
    }
}

void solar_os_shell_cmd_espnow(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        print_status(term);
        return;
    }
    if (argc == 2 && (strcmp(argv[1], "peers") == 0 ||
                      strcmp(argv[1], "list") == 0)) {
        print_peers(term);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "peer") == 0) {
        if (strcmp(argv[2], "add") == 0 && argc == 5) {
            uint32_t link_id = 0U;
            uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN];
            if (!parse_link_id(argv[3], &link_id)) {
                solar_os_shell_diag_invalid(term,
                                            "espnow peer add",
                                            "<link-id>",
                                            argv[3],
                                            "decimal or 0x ID except 0 and 0xffffffff",
                                            "espnow peer add <link-id> <mac>",
                                            false);
                return;
            }
            if (!solar_os_espnow_parse_mac(argv[4], mac)) {
                solar_os_shell_diag_invalid(term,
                                            "espnow peer add",
                                            "<mac>",
                                            argv[4],
                                            "unicast xx:xx:xx:xx:xx:xx",
                                            "espnow peer add <link-id> <mac>",
                                            false);
                return;
            }
            const esp_err_t ret = solar_os_espnow_peer_set(link_id, mac);
            if (ret == ESP_OK) {
                solar_os_shell_io_printf(term,
                                         "ESP-NOW peer saved: 0x%08" PRIx32 " -> %s\n",
                                         link_id,
                                         argv[4]);
            } else {
                solar_os_shell_io_printf(term,
                                         "espnow peer add failed: %s\n",
                                         solar_os_shell_error_text(ret));
            }
            return;
        }
        if (strcmp(argv[2], "remove") == 0 && argc == 4) {
            uint32_t link_id = 0U;
            if (!parse_link_id(argv[3], &link_id)) {
                solar_os_shell_diag_invalid(term,
                                            "espnow peer remove",
                                            "<link-id>",
                                            argv[3],
                                            "decimal or 0x ID except 0 and 0xffffffff",
                                            "espnow peer remove <link-id>",
                                            false);
                return;
            }
            const esp_err_t ret = solar_os_espnow_peer_remove(link_id);
            if (ret == ESP_OK) {
                solar_os_shell_io_printf(term,
                                         "ESP-NOW peer removed: 0x%08" PRIx32 "\n",
                                         link_id);
            } else if (ret == ESP_ERR_NOT_FOUND) {
                solar_os_shell_io_printf(term,
                                         "espnow: peer not found: 0x%08" PRIx32 "\n",
                                         link_id);
            } else {
                solar_os_shell_io_printf(term,
                                         "espnow peer remove failed: %s\n",
                                         solar_os_shell_error_text(ret));
            }
            return;
        }
    }
    print_usage(term);
}
