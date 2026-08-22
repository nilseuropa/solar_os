#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_credentials.h"
#include "solar_os_meshcore.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const meshcore_commands[] = {
    "status", "identity", "name", "advert", "channel",
};

static void meshcore_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  meshcore status");
    solar_os_shell_io_writeln(io, "  meshcore identity show");
    solar_os_shell_io_writeln(
        io, "  meshcore identity generate [--force]");
    solar_os_shell_io_writeln(
        io, "  meshcore identity import <private-key-hex>");
    solar_os_shell_io_writeln(
        io, "  meshcore identity export --private");
    solar_os_shell_io_writeln(io, "  meshcore name [name]");
    solar_os_shell_io_writeln(io, "  meshcore advert zero|flood");
    solar_os_shell_io_writeln(io, "  meshcore channel list");
    solar_os_shell_io_writeln(
        io, "  meshcore channel add <#hashtag>");
    solar_os_shell_io_writeln(
        io, "  meshcore channel add <name> <base64-psk>");
    solar_os_shell_io_writeln(
        io, "  meshcore channel remove <name>");
    solar_os_shell_io_writeln(
        io, "  meshcore channel public on|off");
    solar_os_shell_io_writeln(
        io, "start: job start meshcore <radio> <profile>");
}

static void meshcore_error(solar_os_shell_io_t *io,
                           const char *operation,
                           esp_err_t error)
{
    solar_os_shell_io_printf(
        io, "meshcore %s: %s\n", operation, solar_os_shell_error_text(error));
}

static void meshcore_status(solar_os_shell_io_t *io)
{
    solar_os_meshcore_status_t status;
    const esp_err_t error = solar_os_meshcore_get_status(&status);
    if (error != ESP_OK) {
        meshcore_error(io, "status", error);
        return;
    }
    solar_os_shell_io_printf(io,
                             "MeshCore: %s\n",
                             status.running ? "running" : "stopped");
    solar_os_shell_io_printf(io,
                             "Identity: %s, name: %s\n",
                             status.identity_set ? "set" : "not set",
                             status.name);
    solar_os_shell_io_printf(
        io,
        "Public key: %s\n",
        status.public_key_hex[0] != '\0' ?
            status.public_key_hex : "(none)");
    solar_os_shell_io_printf(io,
                             "Radio: %s, profile: %s\n",
                             status.running ? status.radio : "-",
                             status.running ? status.profile : "-");
    solar_os_shell_io_printf(io,
                             "Channels: %u, contacts loaded: %u\n",
                             (unsigned)status.channels,
                             (unsigned)status.contacts_loaded);
    solar_os_shell_io_printf(
        io,
        "Packets: free %u/%u, tx %" PRIu32 ", rx %" PRIu32 "\n",
        (unsigned)status.packet_pool_free,
        (unsigned)SOLAR_OS_MESHCORE_PACKET_POOL_SIZE,
        status.transmitted,
        status.received);
    solar_os_shell_io_printf(
        io,
        "Adverts: tx %" PRIu32 ", rx %" PRIu32
        "; messages: direct %" PRIu32 ", group %" PRIu32 "\n",
        status.adverts_sent,
        status.adverts_received,
        status.direct_received,
        status.group_received);
    solar_os_shell_io_printf(
        io,
        "ACKs: %" PRIu32 ", retries: %" PRIu32
        ", duplicates: direct %" PRIu32 ", flood %" PRIu32 "\n",
        status.acknowledgements,
        status.retries,
        status.duplicate_direct,
        status.duplicate_flood);
    solar_os_shell_io_printf(
        io,
        "Errors: send %" PRIu32 ", receive %" PRIu32 ", last %s\n",
        status.send_errors,
        status.receive_errors,
        solar_os_shell_error_text(status.last_error));
    solar_os_shell_io_printf(
        io,
        "Context in PSRAM: %s, stack watermark: %" PRIu32 " bytes\n",
        status.context_in_psram ? "yes" : "no",
        status.stack_watermark_bytes);
}

static bool meshcore_identity(solar_os_shell_io_t *io,
                              int argc,
                              char **argv)
{
    if (argc == 3 && strcmp(argv[2], "show") == 0) {
        char key[SOLAR_OS_MESHCORE_PUBLIC_KEY_HEX_LEN];
        const esp_err_t error = solar_os_meshcore_identity_public(key);
        if (error == ESP_OK) {
            solar_os_shell_io_printf(io, "%s\n", key);
        } else {
            meshcore_error(io, "identity show", error);
        }
        return true;
    }
    if ((argc == 3 || argc == 4) &&
        strcmp(argv[2], "generate") == 0 &&
        (argc == 3 || strcmp(argv[3], "--force") == 0)) {
        const esp_err_t error =
            solar_os_meshcore_identity_generate(argc == 4);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "MeshCore identity generated");
        } else {
            meshcore_error(io, "identity generate", error);
        }
        return true;
    }
    if (argc == 4 && strcmp(argv[2], "import") == 0) {
        const esp_err_t error =
            solar_os_meshcore_identity_import(argv[3]);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "MeshCore identity imported");
        } else {
            meshcore_error(io, "identity import", error);
        }
        return true;
    }
    if (argc == 4 && strcmp(argv[2], "export") == 0 &&
        strcmp(argv[3], "--private") == 0) {
        char key[SOLAR_OS_MESHCORE_PRIVATE_KEY_HEX_LEN];
        const esp_err_t error =
            solar_os_meshcore_identity_export_private(key);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(
                io, "WARNING: private identity; keep this secret");
            solar_os_shell_io_printf(io, "%s\n", key);
            solar_os_credentials_wipe(key, sizeof(key));
        } else {
            meshcore_error(io, "identity export", error);
        }
        return true;
    }
    return false;
}

static bool meshcore_channel(solar_os_shell_io_t *io,
                             int argc,
                             char **argv)
{
    if (argc == 3 && strcmp(argv[2], "list") == 0) {
        solar_os_meshcore_channel_t channels[
            SOLAR_OS_MESHCORE_GROUP_CAPACITY];
        const size_t count = solar_os_meshcore_channel_snapshot(
            channels, SOLAR_OS_MESHCORE_GROUP_CAPACITY);
        for (size_t index = 0; index < count; index++) {
            solar_os_shell_io_printf(
                io,
                "%" PRIu32 "  %s%s\n",
                channels[index].id,
                channels[index].name,
                channels[index].builtin ? " (built-in)" : "");
        }
        if (count == 0U) {
            solar_os_shell_io_writeln(io, "No MeshCore channels");
        }
        return true;
    }
    esp_err_t error = ESP_ERR_INVALID_ARG;
    const char *operation = "channel";
    if (argc == 4 && strcmp(argv[2], "add") == 0 &&
        argv[3][0] == '#') {
        operation = "channel add";
        error = solar_os_meshcore_channel_add(argv[3], NULL);
    } else if (argc == 5 && strcmp(argv[2], "add") == 0) {
        operation = "channel add";
        error = solar_os_meshcore_channel_add(argv[3], argv[4]);
    } else if (argc == 4 && strcmp(argv[2], "remove") == 0) {
        operation = "channel remove";
        error = solar_os_meshcore_channel_remove(argv[3]);
    } else if (argc == 4 && strcmp(argv[2], "public") == 0 &&
               (strcmp(argv[3], "on") == 0 ||
                strcmp(argv[3], "off") == 0)) {
        operation = "channel public";
        error = solar_os_meshcore_channel_public_set(
            strcmp(argv[3], "on") == 0);
    } else {
        return false;
    }
    if (error == ESP_OK) {
        solar_os_shell_io_printf(io, "MeshCore %s updated\n", operation);
    } else {
        meshcore_error(io, operation, error);
    }
    return true;
}

void solar_os_shell_cmd_meshcore(solar_os_context_t *ctx,
                                 int argc,
                                 char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        meshcore_status(io);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "identity") == 0 &&
        meshcore_identity(io, argc, argv)) {
        return;
    }
    if ((argc == 2 || argc == 3) &&
        strcmp(argv[1], "name") == 0) {
        if (argc == 3) {
            const esp_err_t error = solar_os_meshcore_name_set(argv[2]);
            if (error != ESP_OK) {
                meshcore_error(io, "name", error);
                return;
            }
        }
        char name[SOLAR_OS_MESHCORE_NAME_MAX + 1U];
        const esp_err_t error = solar_os_meshcore_name_get(name);
        if (error == ESP_OK) {
            solar_os_shell_io_printf(io, "%s\n", name);
        } else {
            meshcore_error(io, "name", error);
        }
        return;
    }
    if (argc == 3 && strcmp(argv[1], "advert") == 0 &&
        (strcmp(argv[2], "zero") == 0 ||
         strcmp(argv[2], "flood") == 0)) {
        const esp_err_t error = solar_os_meshcore_request_advert(
            strcmp(argv[2], "zero") == 0 ?
                SOLAR_OS_MESHCORE_ADVERT_ZERO :
                SOLAR_OS_MESHCORE_ADVERT_FLOOD);
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "MeshCore advert queued");
        } else {
            meshcore_error(io, "advert", error);
        }
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "channel") == 0 &&
        meshcore_channel(io, argc, argv)) {
        return;
    }
    solar_os_shell_diag_subcommand(io,
                                   "meshcore",
                                   argc,
                                   argv,
                                   "meshcore status|identity|name|advert|channel",
                                   meshcore_commands,
                                   sizeof(meshcore_commands) / sizeof(meshcore_commands[0]));
}
