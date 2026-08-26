#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "services/solar_os_chat.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const gateway_commands[] = {
    "status", "configure", "connect", "disconnect", "rooms", "join",
    "leave", "delete",
};

static void gateway_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  gateway status");
    solar_os_shell_io_writeln(
        io, "  gateway configure <url> [token]");
    solar_os_shell_io_writeln(io, "  gateway connect [url] [token]");
    solar_os_shell_io_writeln(io, "  gateway disconnect");
    solar_os_shell_io_writeln(io, "  gateway rooms");
    solar_os_shell_io_writeln(io, "  gateway join <room>");
    solar_os_shell_io_writeln(io, "  gateway leave <room>");
    solar_os_shell_io_writeln(io, "  gateway delete <room>");
}

static void gateway_status(solar_os_shell_io_t *io)
{
    solar_os_chat_status_t status;
    const esp_err_t error = solar_os_chat_get_status(&status);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "gateway: unavailable: %s\n",
                                 solar_os_shell_error_text(error));
        return;
    }
    solar_os_shell_io_printf(
        io,
        "State: %s\n"
        "Enabled: %s\n"
        "URL: %s\n"
        "Identity: %s@%s\n"
        "Token: %s\n"
        "Traffic: rx=%" PRIu32 " tx=%" PRIu32 " dropped=%" PRIu32 "\n",
        solar_os_chat_state_name(status.state),
        status.enabled ? "yes" : "no",
        status.url[0] != '\0' ? status.url : "(not configured)",
        status.user,
        status.device,
        status.token_set ? "set" : "not set",
        status.rx_count,
        status.tx_count,
        status.dropped_count);
    if (status.last_error[0] != '\0') {
        solar_os_shell_io_printf(io, "Last error: %s\n", status.last_error);
    }
}

static void gateway_rooms(solar_os_shell_io_t *io)
{
    solar_os_chat_channel_t rooms[SOLAR_OS_CHAT_CHANNEL_CAPACITY];
    const size_t count = solar_os_chat_channel_snapshot(
        rooms, SOLAR_OS_CHAT_CHANNEL_CAPACITY);
    for (size_t i = 0; i < count; i++) {
        const char *state = rooms[i].joined ? "joined" :
            (rooms[i].desired ? "joining" : "known");
        solar_os_shell_io_printf(io,
                                 "%-7s %s\n",
                                 state,
                                 rooms[i].name);
    }
    if (count == 0U) {
        solar_os_shell_io_writeln(io, "No gateway rooms");
    }
}

void solar_os_shell_cmd_gateway(solar_os_context_t *ctx,
                                int argc,
                                char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        gateway_status(io);
        return;
    }
    if (argc >= 3 && argc <= 4 && strcmp(argv[1], "configure") == 0) {
        const char *token = argc >= 4 ? argv[3] : NULL;
        const esp_err_t error = solar_os_chat_configure(argv[2], token);
        solar_os_shell_io_printf(io,
                                 "gateway: %s\n",
                                 error == ESP_OK ? "configured" :
                                     solar_os_shell_error_text(error));
        return;
    }
    if (argc >= 2 && argc <= 4 && strcmp(argv[1], "connect") == 0) {
        const char *url = argc >= 3 ? argv[2] : NULL;
        const char *token = argc >= 4 ? argv[3] : NULL;
        const esp_err_t error = solar_os_chat_connect(url, token);
        solar_os_shell_io_printf(io,
                                 "gateway: %s\n",
                                 error == ESP_OK ? "connection requested" :
                                     solar_os_shell_error_text(error));
        return;
    }
    if (argc == 2 && strcmp(argv[1], "disconnect") == 0) {
        const esp_err_t error = solar_os_chat_disconnect();
        solar_os_shell_io_printf(
            io,
            "gateway: %s\n",
            error == ESP_OK || error == ESP_ERR_INVALID_STATE ?
                "disconnected" : solar_os_shell_error_text(error));
        return;
    }
    if (argc == 2 && strcmp(argv[1], "rooms") == 0) {
        gateway_rooms(io);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "join") == 0) {
        const esp_err_t error = solar_os_chat_join(argv[2]);
        solar_os_shell_io_printf(io,
                                 "gateway: %s\n",
                                 error == ESP_OK ? "join queued" :
                                     solar_os_shell_error_text(error));
        return;
    }
    if (argc == 3 && strcmp(argv[1], "leave") == 0) {
        const esp_err_t error = solar_os_chat_leave(argv[2]);
        solar_os_shell_io_printf(io,
                                 "gateway: %s\n",
                                 error == ESP_OK ? "leave queued" :
                                     solar_os_shell_error_text(error));
        return;
    }
    if (argc == 3 && strcmp(argv[1], "delete") == 0) {
        const esp_err_t error = solar_os_chat_delete_channel(argv[2]);
        solar_os_shell_io_printf(io,
                                 "gateway: %s\n",
                                 error == ESP_OK ? "delete queued" :
                                     solar_os_shell_error_text(error));
        return;
    }
    if (argc <= 1) {
        gateway_usage(io);
        return;
    }
    solar_os_shell_diag_subcommand(
        io,
        "gateway",
        argc,
        argv,
        "gateway status|configure|connect|disconnect|rooms|join|leave|delete",
        gateway_commands,
        sizeof(gateway_commands) / sizeof(gateway_commands[0]));
}
