#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_credentials.h"
#include "solar_os_meshcore.h"
#include "solar_os_meshcore_stream.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const meshcore_commands[] = {
    "status", "identity", "name", "advert", "channel", "stream",
};

static const char * const meshcore_stream_commands[] = {
    "status", "list", "create", "remove",
};

static void meshcore_error(solar_os_shell_io_t *io,
                           const char *operation,
                           esp_err_t error);

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
    solar_os_shell_io_writeln(io, "  meshcore stream status [port]");
    solar_os_shell_io_writeln(io, "  meshcore stream list");
    solar_os_shell_io_writeln(
        io, "  meshcore stream create <port> <endpoint-id>");
    solar_os_shell_io_writeln(io, "  meshcore stream remove <port>");
    solar_os_shell_io_writeln(
        io, "start: job start meshcore <radio> <profile>");
}

static bool meshcore_parse_id(const char *text, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed == 0UL || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static const char *meshcore_stream_decode_issue_name(
    solar_os_link_stream_decode_issue_t issue)
{
    switch (issue) {
    case SOLAR_OS_LINK_STREAM_DECODE_NONE:
        return "none";
    case SOLAR_OS_LINK_STREAM_DECODE_STRUCTURE:
        return "structure";
    case SOLAR_OS_LINK_STREAM_DECODE_MAGIC:
        return "magic";
    case SOLAR_OS_LINK_STREAM_DECODE_VERSION:
        return "version";
    case SOLAR_OS_LINK_STREAM_DECODE_OPCODE:
        return "opcode";
    case SOLAR_OS_LINK_STREAM_DECODE_SESSION:
        return "session";
    case SOLAR_OS_LINK_STREAM_DECODE_DATA:
        return "data";
    case SOLAR_OS_LINK_STREAM_DECODE_CONTROL_SEQUENCE:
        return "control-sequence";
    case SOLAR_OS_LINK_STREAM_DECODE_CONTROL_DATA:
        return "control-data";
    case SOLAR_OS_LINK_STREAM_DECODE_ACKNOWLEDGEMENT:
        return "acknowledgement";
    default:
        return "unknown";
    }
}

static void meshcore_stream_print(
    solar_os_shell_io_t *io,
    const solar_os_meshcore_stream_status_t *status)
{
    const char *state = status->stream.port_open
        ? (status->stream.connected ? "connected" : "connecting")
        : "closed";
    solar_os_shell_io_printf(
        io,
        "%s endpoint=%" PRIu32 " peer=0x%08" PRIx32
        " state=%s proto=%u mtu=%u rx-queued=%u tx-queued=%u tx-inflight=%u\n",
        status->stream.port,
        status->endpoint_id,
        status->peer_id,
        state,
        (unsigned)status->stream.protocol_version,
        (unsigned)status->stream.data_mtu,
        (unsigned)status->stream.rx_queued,
        (unsigned)status->stream.tx_queued,
        (unsigned)status->stream.tx_inflight);
    solar_os_shell_io_printf(
        io,
        "  tx-bytes=%" PRIu32 " rx-bytes=%" PRIu32
        " tx-frames=%" PRIu32 " rx-frames=%" PRIu32
        " ack-sent=%" PRIu32 " ack-received=%" PRIu32
        " retries=%" PRIu32 " reconnects=%" PRIu32 "\n",
        status->stream.bytes_sent,
        status->stream.bytes_received,
        status->stream.frames_sent,
        status->stream.frames_received,
        status->stream.acknowledgements_sent,
        status->stream.acknowledgements_received,
        status->stream.retries,
        status->stream.reconnects);
    solar_os_shell_io_printf(
        io,
        "  mesh-tx=%" PRIu32 " mesh-rx=%" PRIu32
        " transport-errors=%" PRIu32 " dropped=%" PRIu32
        " decode-errors=%" PRIu32 " decode-issue=%s"
        " opcode=%u sequence=%u data=%u retry=%" PRIu32
        "-%" PRIu32 "ms peer-timeout=%" PRIu32 "ms last=%s\n",
        status->mesh_packets_sent,
        status->mesh_packets_received,
        status->transport_errors,
        status->stream.dropped,
        status->stream.decode_errors,
        meshcore_stream_decode_issue_name(status->stream.last_decode_issue),
        (unsigned)status->stream.last_decode_opcode,
        (unsigned)status->stream.last_decode_sequence,
        (unsigned)status->stream.last_decode_data_len,
        status->stream.retry_ms,
        status->stream.retry_ms + status->stream.retry_jitter_ms,
        status->stream.peer_timeout_ms,
        solar_os_shell_error_text(
            status->transport_last_error != ESP_OK
                ? status->transport_last_error
                : status->stream.last_error));
}

static void meshcore_stream_status(solar_os_shell_io_t *io,
                                   int argc,
                                   char **argv)
{
    if (argc == 3) {
        const size_t count = solar_os_meshcore_stream_count();
        if (count == 0U) {
            solar_os_shell_io_writeln(io, "No MeshCore streams");
            return;
        }
        for (size_t index = 0U; index < count; index++) {
            solar_os_meshcore_stream_status_t status;
            if (solar_os_meshcore_stream_get(index, &status)) {
                meshcore_stream_print(io, &status);
            }
        }
        return;
    }
    if (argc == 4 && strcmp(argv[2], "status") == 0) {
        solar_os_meshcore_stream_status_t status;
        const esp_err_t error =
            solar_os_meshcore_stream_get_status(argv[3], &status);
        if (error == ESP_OK) {
            meshcore_stream_print(io, &status);
        } else {
            meshcore_error(io, "stream status", error);
        }
        return;
    }
    solar_os_shell_diag_unexpected(
        io,
        "meshcore stream status",
        argc > 3 ? argv[3] : NULL,
        "meshcore stream status [port]");
}

static void meshcore_stream(solar_os_shell_io_t *io,
                            int argc,
                            char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "status") == 0) {
        meshcore_stream_status(io, argc, argv);
        return;
    }
    if (argc == 3 && strcmp(argv[2], "list") == 0) {
        meshcore_stream_status(io, argc, argv);
        return;
    }
    if (argc >= 3 && strcmp(argv[2], "create") == 0) {
        if (argc != 5) {
            if (argc < 5) {
                solar_os_shell_diag_missing(
                    io,
                    "meshcore stream create",
                    argc < 4 ? "<port>" : "<endpoint-id>",
                    "meshcore stream create <port> <endpoint-id>");
            } else {
                solar_os_shell_diag_unexpected(
                    io,
                    "meshcore stream create",
                    argv[5],
                    "meshcore stream create <port> <endpoint-id>");
            }
            return;
        }
        uint32_t endpoint_id = 0U;
        if (!meshcore_parse_id(argv[4], &endpoint_id)) {
            solar_os_shell_diag_invalid(
                io,
                "meshcore stream create",
                "endpoint-id",
                argv[4],
                "a decimal trusted MeshCore endpoint ID",
                "meshcore stream create <port> <endpoint-id>",
                false);
            return;
        }
        const esp_err_t error = solar_os_meshcore_stream_create(
            argv[3], (solar_os_endpoint_id_t)endpoint_id);
        if (error == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(
                io,
                "meshcore stream create: start MeshCore and use a trusted MeshCore endpoint");
        } else if (error != ESP_OK) {
            meshcore_error(io, "stream create", error);
        } else {
            solar_os_shell_io_printf(
                io,
                "MeshCore stream %s registered for endpoint %" PRIu32 "\n",
                argv[3],
                endpoint_id);
        }
        return;
    }
    if (argc >= 3 && strcmp(argv[2], "remove") == 0) {
        if (argc != 4) {
            if (argc < 4) {
                solar_os_shell_diag_missing(
                    io,
                    "meshcore stream remove",
                    "<port>",
                    "meshcore stream remove <port>");
            } else {
                solar_os_shell_diag_unexpected(
                    io,
                    "meshcore stream remove",
                    argv[4],
                    "meshcore stream remove <port>");
            }
            return;
        }
        const esp_err_t error = solar_os_meshcore_stream_remove(argv[3]);
        if (error == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_printf(
                io,
                "meshcore stream remove: %s is in use; close its shell or bridge first\n",
                argv[3]);
        } else if (error != ESP_OK) {
            meshcore_error(io, "stream remove", error);
        } else {
            solar_os_shell_io_printf(
                io, "MeshCore stream removed: %s\n", argv[3]);
        }
        return;
    }
    solar_os_shell_diag_subcommand(
        io,
        "meshcore stream",
        argc - 1,
        &argv[1],
        "meshcore stream status|list|create|remove",
        meshcore_stream_commands,
        sizeof(meshcore_stream_commands) /
            sizeof(meshcore_stream_commands[0]));
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
    if (argc >= 2 && strcmp(argv[1], "stream") == 0) {
        meshcore_stream(io, argc, argv);
        return;
    }
    solar_os_shell_diag_subcommand(io,
                                   "meshcore",
                                   argc,
                                   argv,
                                   "meshcore status|identity|name|advert|channel|stream",
                                   meshcore_commands,
                                   sizeof(meshcore_commands) / sizeof(meshcore_commands[0]));
}
