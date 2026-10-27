#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_link.h"
#include "solar_os_link_stream.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const link_commands[] = {
    "status", "list", "send", "send-binary", "receive", "recv", "stream",
};

static const char * const link_stream_commands[] = {
    "status", "list", "create", "remove",
};

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void link_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  link status|list");
    solar_os_shell_io_writeln(term, "  link status <link>");
    solar_os_shell_io_writeln(term, "  link send <link> <broadcast|destination-id> <text>");
    solar_os_shell_io_writeln(term,
                              "  link send-binary <link> <broadcast|destination-id> <byte...>");
    solar_os_shell_io_writeln(term, "  link receive <link> [timeout-ms]");
    solar_os_shell_io_writeln(term, "  link stream status [port]");
    solar_os_shell_io_writeln(term, "  link stream list");
    solar_os_shell_io_writeln(term,
                              "  link stream create <link> <port> <peer-id>");
    solar_os_shell_io_writeln(term, "  link stream remove <port>");
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_destination(const char *text, uint32_t *destination)
{
    if (strcmp(text, "broadcast") == 0) {
        *destination = SOLAR_OS_LINK_BROADCAST;
        return true;
    }
    return parse_u32(text, destination) && *destination != 0 &&
           *destination != SOLAR_OS_LINK_BROADCAST;
}

static bool parse_byte(const char *text, uint8_t *value)
{
    uint32_t parsed = 0;
    if (!parse_u32(text, &parsed) || parsed > UINT8_MAX) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static void link_print_error(solar_os_shell_io_t *term, const char *operation, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        solar_os_shell_io_printf(term, "%s: link not found\n", operation);
        break;
    case ESP_ERR_INVALID_SIZE:
        solar_os_shell_io_printf(term, "%s: payload exceeds transport MTU\n", operation);
        break;
    case ESP_ERR_NO_MEM:
        solar_os_shell_io_printf(term, "%s: queue full\n", operation);
        break;
    case ESP_ERR_TIMEOUT:
        solar_os_shell_io_printf(term, "%s: no message\n", operation);
        break;
    default:
        solar_os_shell_io_printf(term, "%s failed: %s\n", operation, solar_os_shell_error_text(err));
        break;
    }
}

static void link_print_status(solar_os_shell_io_t *term, const solar_os_link_status_t *status)
{
    const size_t payload_mtu =
        status->frame_mtu >= SOLAR_OS_LINK_HEADER_SIZE + SOLAR_OS_LINK_CRC_SIZE
            ? status->frame_mtu - SOLAR_OS_LINK_HEADER_SIZE - SOLAR_OS_LINK_CRC_SIZE
            : 0;
    solar_os_shell_io_printf(term,
                             "%s id=0x%08" PRIx32 " mtu=%u payload=%u rx-queued=%u tx-queued=%u"
                             " ack-pending=%u\n",
                             status->name,
                             status->local_id,
                             (unsigned)status->frame_mtu,
                             (unsigned)payload_mtu,
                             (unsigned)status->rx_queued,
                             (unsigned)status->tx_queued,
                             (unsigned)status->acknowledgements_pending);
    solar_os_shell_io_printf(term,
                             "  tx=%" PRIu32 " rx=%" PRIu32 " duplicates=%" PRIu32
                             " crc-errors=%" PRIu32 " ack-sent=%" PRIu32 " ack-received=%" PRIu32
                             " dropped=%" PRIu32 " last=%s\n",
                             status->tx_messages,
                             status->rx_messages,
                             status->duplicates,
                             status->crc_errors,
                             status->acknowledgements_sent,
                             status->acknowledgements_received,
                             status->dropped,
                             solar_os_shell_error_text(status->last_error));
}

static void link_status(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2) {
        const size_t count = solar_os_link_count();
        if (count == 0) {
            solar_os_shell_io_writeln(term, "no active links");
            return;
        }
        for (size_t i = 0; i < count; i++) {
            solar_os_link_status_t status;
            if (solar_os_link_get(i, &status)) {
                link_print_status(term, &status);
            }
        }
        return;
    }
    if (argc != 3) {
        solar_os_shell_diag_unexpected(term, "link status", argv[3],
                                       "link status [link]");
        return;
    }
    solar_os_link_status_t status;
    const esp_err_t ret = solar_os_link_get_status(argv[2], &status);
    if (ret != ESP_OK) {
        link_print_error(term, "link status", ret);
        return;
    }
    link_print_status(term, &status);
}

static void link_send_text(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 5) {
        const char *missing = argc < 3 ? "<link>" :
                              argc < 4 ? "<destination>" : "<text>";
        solar_os_shell_diag_missing(term, "link send", missing,
                                    "link send <link> <broadcast|destination-id> <text>");
        return;
    }
    uint32_t destination = 0;
    if (!parse_destination(argv[3], &destination)) {
        solar_os_shell_diag_invalid(term, "link send", "destination", argv[3],
                                    "broadcast or a numeric destination ID",
                                    "link send <link> <broadcast|destination-id> <text>", false);
        return;
    }

    uint8_t payload[SOLAR_OS_LINK_PAYLOAD_MAX];
    size_t used = 0;
    for (int i = 4; i < argc; i++) {
        const size_t len = strlen(argv[i]);
        if (used + len + (i > 4 ? 1U : 0U) > sizeof(payload)) {
            link_print_error(term, "link send", ESP_ERR_INVALID_SIZE);
            return;
        }
        if (i > 4) {
            payload[used++] = ' ';
        }
        memcpy(&payload[used], argv[i], len);
        used += len;
    }

    uint16_t sequence = 0;
    const esp_err_t ret = solar_os_link_send(
        argv[2], SOLAR_OS_LINK_MESSAGE_TEXT, destination, payload, used, &sequence);
    if (ret != ESP_OK) {
        link_print_error(term, "link send", ret);
        return;
    }
    solar_os_shell_io_printf(term, "queued sequence=%u\n", sequence);
}

static void link_send_binary(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 5) {
        const char *missing = argc < 3 ? "<link>" :
                              argc < 4 ? "<destination>" : "<byte>";
        solar_os_shell_diag_missing(
            term, "link send-binary", missing,
            "link send-binary <link> <broadcast|destination-id> <byte...>");
        return;
    }
    uint32_t destination = 0;
    if (!parse_destination(argv[3], &destination)) {
        solar_os_shell_diag_invalid(
            term, "link send-binary", "destination", argv[3],
            "broadcast or a numeric destination ID",
            "link send-binary <link> <broadcast|destination-id> <byte...>", false);
        return;
    }

    uint8_t payload[SOLAR_OS_LINK_PAYLOAD_MAX];
    size_t len = 0;
    for (int i = 4; i < argc; i++) {
        if (len >= sizeof(payload) || !parse_byte(argv[i], &payload[len++])) {
            solar_os_shell_diag_invalid(
                term, "link send-binary", "byte", argv[i], "a byte from 0 to 255",
                "link send-binary <link> <broadcast|destination-id> <byte...>", false);
            return;
        }
    }
    uint16_t sequence = 0;
    const esp_err_t ret = solar_os_link_send(
        argv[2], SOLAR_OS_LINK_MESSAGE_BINARY, destination, payload, len, &sequence);
    if (ret != ESP_OK) {
        link_print_error(term, "link send-binary", ret);
        return;
    }
    solar_os_shell_io_printf(term, "queued sequence=%u\n", sequence);
}

static void link_receive(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "link receive", "<link>",
                                        "link receive <link> [timeout-ms]");
        } else {
            solar_os_shell_diag_unexpected(term, "link receive", argv[4],
                                           "link receive <link> [timeout-ms]");
        }
        return;
    }
    uint32_t timeout_ms = 0;
    if (argc == 4 && (!parse_u32(argv[3], &timeout_ms) || timeout_ms > 1000U)) {
        solar_os_shell_diag_invalid(term, "link receive", "timeout-ms", argv[3],
                                    "an integer from 0 to 1000",
                                    "link receive <link> [timeout-ms]", false);
        return;
    }

    solar_os_link_message_t message;
    const esp_err_t ret = solar_os_link_receive(argv[2], &message, timeout_ms);
    if (ret != ESP_OK) {
        link_print_error(term, "link receive", ret);
        return;
    }

    if (message.destination == SOLAR_OS_LINK_BROADCAST) {
        solar_os_shell_io_printf(
            term,
            "type=%s sequence=%u source=0x%08" PRIx32
            " destination=broadcast length=%u\n",
            solar_os_link_message_type_name(message.type),
            message.sequence,
            message.source,
            (unsigned)message.payload_len);
    } else {
        solar_os_shell_io_printf(
            term,
            "type=%s sequence=%u source=0x%08" PRIx32
            " destination=0x%08" PRIx32 " length=%u\n",
            solar_os_link_message_type_name(message.type),
            message.sequence,
            message.source,
            message.destination,
            (unsigned)message.payload_len);
    }
    if (message.type == SOLAR_OS_LINK_MESSAGE_TEXT) {
        solar_os_shell_io_printf(
            term, "%.*s\n", (int)message.payload_len, (const char *)message.payload);
    } else {
        for (size_t i = 0; i < message.payload_len; i++) {
            solar_os_shell_io_printf(term, "%s%02x", i == 0 ? "" : " ", message.payload[i]);
        }
        solar_os_shell_io_put_char(term, '\n');
    }
}

static void link_stream_print_status(solar_os_shell_io_t *term,
                                     const solar_os_link_stream_status_t *status)
{
    const char *state = status->port_open
        ? (status->connected ? "connected" : "connecting")
        : "closed";
    solar_os_shell_io_printf(term,
                             "%s link=%s peer=0x%08" PRIx32
                             " state=%s proto=%u mtu=%u rx-queued=%u tx-queued=%u tx-inflight=%u window=%u credit=%u\n",
                             status->port,
                             status->link,
                             status->peer_id,
                             state,
                             (unsigned)status->protocol_version,
                             (unsigned)status->data_mtu,
                             (unsigned)status->rx_queued,
                             (unsigned)status->tx_queued,
                             (unsigned)status->tx_inflight,
                             (unsigned)status->window_frames,
                             (unsigned)status->tx_credit);
    solar_os_shell_io_printf(term,
                             "  tx-bytes=%" PRIu32 " rx-bytes=%" PRIu32
                             " tx-frames=%" PRIu32 " rx-frames=%" PRIu32
                             " ack-sent=%" PRIu32 " ack-received=%" PRIu32
                             " retries=%" PRIu32 " reconnects=%" PRIu32
                             " dropped=%" PRIu32 " decode-errors=%" PRIu32 " last=%s\n",
                             status->bytes_sent,
                             status->bytes_received,
                             status->frames_sent,
                             status->frames_received,
                             status->acknowledgements_sent,
                             status->acknowledgements_received,
                             status->retries,
                             status->reconnects,
                             status->dropped,
                             status->decode_errors,
                             solar_os_shell_error_text(status->last_error));
}

static void link_stream_status(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || argc == 3) {
        const size_t count = solar_os_link_stream_count();
        if (count == 0U) {
            solar_os_shell_io_writeln(term, "no Link streams");
            return;
        }
        for (size_t i = 0; i < count; i++) {
            solar_os_link_stream_status_t status;
            if (solar_os_link_stream_get(i, &status)) {
                link_stream_print_status(term, &status);
            }
        }
        return;
    }
    if (argc == 4 && strcmp(argv[2], "status") == 0) {
        solar_os_link_stream_status_t status;
        const esp_err_t error = solar_os_link_stream_get_status(argv[3], &status);
        if (error != ESP_OK) {
            link_print_error(term, "link stream status", error);
            return;
        }
        link_stream_print_status(term, &status);
        return;
    }
    solar_os_shell_diag_unexpected(term,
                                   "link stream status",
                                   argc > 3 ? argv[3] : NULL,
                                   "link stream status [port]");
}

static void link_stream_create(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 6) {
        const char *missing = argc < 4 ? "<link>" :
                              argc < 5 ? "<port>" : "<peer-id>";
        if (argc < 6) {
            solar_os_shell_diag_missing(term,
                                        "link stream create",
                                        missing,
                                        "link stream create <link> <port> <peer-id>");
        } else {
            solar_os_shell_diag_unexpected(term,
                                           "link stream create",
                                           argv[6],
                                           "link stream create <link> <port> <peer-id>");
        }
        return;
    }
    uint32_t peer_id = 0U;
    if (!parse_u32(argv[5], &peer_id) || peer_id == 0U ||
        peer_id == SOLAR_OS_LINK_BROADCAST) {
        solar_os_shell_diag_invalid(term,
                                    "link stream create",
                                    "peer-id",
                                    argv[5],
                                    "a unicast numeric Link ID",
                                    "link stream create <link> <port> <peer-id>",
                                    false);
        return;
    }
    const esp_err_t error = solar_os_link_stream_create(argv[3], argv[4], peer_id);
    if (error != ESP_OK) {
        link_print_error(term, "link stream create", error);
        return;
    }
    solar_os_shell_io_printf(term,
                             "Link stream %s registered on %s for peer 0x%08" PRIx32 "\n",
                             argv[4],
                             argv[3],
                             peer_id);
}

static void link_stream_remove(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 4) {
        if (argc < 4) {
            solar_os_shell_diag_missing(term,
                                        "link stream remove",
                                        "<port>",
                                        "link stream remove <port>");
        } else {
            solar_os_shell_diag_unexpected(term,
                                           "link stream remove",
                                           argv[4],
                                           "link stream remove <port>");
        }
        return;
    }
    const esp_err_t error = solar_os_link_stream_remove(argv[3]);
    if (error == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(term,
                                 "link stream remove: %s is in use; close its shell or bridge first\n",
                                 argv[3]);
        return;
    }
    if (error != ESP_OK) {
        link_print_error(term, "link stream remove", error);
        return;
    }
    solar_os_shell_io_printf(term, "Link stream removed: %s\n", argv[3]);
}

static void link_stream(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0 ||
        strcmp(argv[2], "list") == 0) {
        link_stream_status(term, argc, argv);
    } else if (strcmp(argv[2], "create") == 0) {
        link_stream_create(term, argc, argv);
    } else if (strcmp(argv[2], "remove") == 0) {
        link_stream_remove(term, argc, argv);
    } else {
        solar_os_shell_diag_subcommand(
            term,
            "link stream",
            argc - 1,
            &argv[1],
            "link stream status|list|create|remove",
            link_stream_commands,
            sizeof(link_stream_commands) / sizeof(link_stream_commands[0]));
    }
}

void solar_os_shell_cmd_link(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 ||
        (argc == 2 && (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "list") == 0))) {
        link_status(term, 2, argv);
    } else if (strcmp(argv[1], "status") == 0) {
        link_status(term, argc, argv);
    } else if (strcmp(argv[1], "send") == 0) {
        link_send_text(term, argc, argv);
    } else if (strcmp(argv[1], "send-binary") == 0) {
        link_send_binary(term, argc, argv);
    } else if (strcmp(argv[1], "receive") == 0 || strcmp(argv[1], "recv") == 0) {
        link_receive(term, argc, argv);
    } else if (strcmp(argv[1], "stream") == 0) {
        link_stream(term, argc, argv);
    } else {
        solar_os_shell_diag_subcommand(term,
                                       "link",
                                       argc,
                                       argv,
                                       "link status|list|send|send-binary|receive|recv|stream",
                                       link_commands,
                                       sizeof(link_commands) / sizeof(link_commands[0]));
    }
}
