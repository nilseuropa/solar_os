#include "solar_os_shell_commands.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_irc.h"
#include "solar_os_memory.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static void irc_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "irc add <instance> <host> <port> <nick>");
    solar_os_shell_io_writeln(io, "irc set <instance> <host> <port> <nick>");
    solar_os_shell_io_writeln(io, "irc sasl <instance> <account> <password> | irc sasl <instance> off");
    solar_os_shell_io_writeln(io, "irc pass <instance> <password|off>");
    solar_os_shell_io_writeln(io, "irc ca <instance> <PEM-path|default>");
    solar_os_shell_io_writeln(io, "irc connect|disconnect <instance>");
    solar_os_shell_io_writeln(io, "irc remove <instance> (delete local history)");
    solar_os_shell_io_writeln(io, "irc join|query <instance> <channel|nick>");
    solar_os_shell_io_writeln(io, "irc leave <conversation-id>");
    solar_os_shell_io_writeln(io, "irc status; job start irc-sync");
}

static bool irc_port(const char *text, uint16_t *port)
{
    char *end; unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || *end || value == 0 || value > 65535) return false;
    *port = value; return true;
}

void solar_os_shell_cmd_irc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (!io) return;
    solar_os_irc_config_t *config = NULL;
    esp_err_t error = solar_os_irc_init();
    if (error != ESP_OK) goto done;
    if (argc == 1) { irc_usage(io); return; }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        config = solar_os_memory_calloc(1, sizeof(*config),
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "shell.irc.config");
        if (!config) { error = ESP_ERR_NO_MEM; goto done; }
        solar_os_messaging_provider_id_t cursor = 0;
        solar_os_messaging_provider_status_t status;
        while (solar_os_messaging_provider_next(&cursor, &status)) {
            if (strcmp(status.class_name, "irc") != 0) continue;
            solar_os_shell_io_printf(io, "%" PRIu32 " %s: %s, %s\n", cursor, status.name,
                status.connected ? "connected" : (status.running ? "running" : "stopped"), status.detail);
            if (solar_os_irc_get_config(cursor, config) == ESP_OK) {
                solar_os_shell_io_printf(io, "  %s:%u nick=%s enabled=%s SASL=%s TLS=%s\n",
                    config->host, config->port, config->nick, config->enabled ? "yes" : "no", config->account[0] ? "on" : "off", status.connected ? "verified" : "verification-required");
                memset(config, 0, sizeof(*config));
            }
        }
        solar_os_memory_free(config);
        return;
    }
    if (argc == 6 && strcmp(argv[1], "add") == 0) {
        uint16_t port; solar_os_messaging_provider_id_t id = 0;
        error = irc_port(argv[4], &port) ? solar_os_irc_create(argv[2], argv[3], port, argv[5], &id) : ESP_ERR_INVALID_ARG;
        if (error == ESP_OK) { solar_os_shell_io_printf(io, "irc: saved instance %s (%" PRIu32 ")\n", argv[2], id); return; }
        goto done;
    }
    if (argc == 3 && strcmp(argv[1], "leave") == 0) {
        char *end; unsigned long id = strtoul(argv[2], &end, 10);
        error = *end || !id || id > UINT32_MAX ? ESP_ERR_INVALID_ARG : solar_os_messaging_leave(id);
        goto done;
    }
    if (argc < 3) { irc_usage(io); return; }
    solar_os_messaging_provider_id_t id = solar_os_messaging_provider_find(argv[2]);
    if (!solar_os_messaging_provider_is_class(id, "irc")) { error = ESP_ERR_NOT_FOUND; goto done; }
    if (argc == 3 && strcmp(argv[1], "remove") == 0) {
        error = solar_os_messaging_instance_remove(id); goto done;
    }
    if (argc == 3 && (strcmp(argv[1], "connect") == 0 || strcmp(argv[1], "disconnect") == 0)) {
        error = solar_os_messaging_connect(id, strcmp(argv[1], "connect") == 0); goto done;
    }
    if (argc == 4 && (strcmp(argv[1], "join") == 0 || strcmp(argv[1], "query") == 0)) {
        solar_os_conversation_id_t conversation = 0;
        error = strcmp(argv[1], "join") == 0 ? solar_os_messaging_join(id, argv[3], &conversation) :
            solar_os_messaging_open_direct(id, argv[3], &conversation);
        if (error == ESP_OK) { solar_os_shell_io_printf(io, "irc: conversation %" PRIu32 "\n", conversation); return; }
        goto done;
    }
    config = solar_os_memory_calloc(1, sizeof(*config),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "shell.irc.config");
    if (!config) { error = ESP_ERR_NO_MEM; goto done; }
    error = solar_os_irc_get_config(id, config);
    if (error != ESP_OK) goto done;
    bool valid = false;
    if (argc == 6 && strcmp(argv[1], "set") == 0) {
        valid = irc_port(argv[4], &config->port) &&
            strlcpy(config->host, argv[3], sizeof(config->host)) < sizeof(config->host) &&
            strlcpy(config->nick, argv[5], sizeof(config->nick)) < sizeof(config->nick);
    } else if (argc == 5 && strcmp(argv[1], "sasl") == 0) {
        valid = strlcpy(config->account, argv[3], sizeof(config->account)) < sizeof(config->account) &&
            strlcpy(config->password, argv[4], sizeof(config->password)) < sizeof(config->password);
    } else if (argc == 4 && strcmp(argv[1], "sasl") == 0 && strcmp(argv[3], "off") == 0) {
        memset(config->account, 0, sizeof(config->account)); memset(config->password, 0, sizeof(config->password)); valid = true;
    } else if (argc == 4 && strcmp(argv[1], "pass") == 0) {
        valid = strlcpy(config->server_password, strcmp(argv[3], "off") == 0 ? "" : argv[3], sizeof(config->server_password)) < sizeof(config->server_password);
    } else if (argc == 4 && strcmp(argv[1], "ca") == 0) {
        valid = strlcpy(config->ca_file, strcmp(argv[3], "default") == 0 ? "" : argv[3], sizeof(config->ca_file)) < sizeof(config->ca_file);
    }
    error = valid ? solar_os_irc_configure(id, config) : ESP_ERR_INVALID_ARG;
done:
    if (config) {
        volatile unsigned char *secret = (volatile unsigned char *)config;
        for (size_t i = 0; i < sizeof(*config); i++) secret[i] = 0;
        solar_os_memory_free(config);
    }
    solar_os_shell_io_printf(io, "irc: %s\n", error == ESP_OK ? "ok" : solar_os_shell_error_text(error));
}
