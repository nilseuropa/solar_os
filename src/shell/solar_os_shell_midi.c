#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_midi.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void midi_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  midi status");
    solar_os_shell_io_writeln(term, "  midi note-on <channel> <note> [velocity]");
    solar_os_shell_io_writeln(term, "  midi note-off <channel> <note> [velocity]");
    solar_os_shell_io_writeln(term, "  midi cc <channel> <controller> <value>");
    solar_os_shell_io_writeln(term, "  midi program <channel> <program>");
    solar_os_shell_io_writeln(term, "  midi send <status> [data1] [data2]");
    solar_os_shell_io_writeln(term, "  midi stream list");
    solar_os_shell_io_writeln(term,
                              "  midi stream add|remove <channel> <controller>");
    solar_os_shell_io_writeln(term, "  midi stream clear");
}

static bool midi_parse_range(const char *text, uint8_t minimum, uint8_t maximum,
                             uint8_t *value)
{
    uint8_t parsed = 0U;
    if (!solar_os_shell_parse_u8(text, &parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static void midi_send_message(solar_os_shell_io_t *term,
                              const solar_os_midi_message_t *message)
{
    const esp_err_t error = solar_os_midi_send(message);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(term, "midi: send failed: %s\n", esp_err_to_name(error));
        return;
    }
    solar_os_shell_io_writeln(term, "MIDI message queued");
}

static void midi_cmd_status(solar_os_shell_io_t *term)
{
    solar_os_midi_status_t status;
    solar_os_midi_get_status(&status);
    solar_os_shell_io_printf(term, "MIDI: %s\n", status.running ? "running" : "stopped");
    if (status.bus_name[0] != '\0') {
        solar_os_shell_io_printf(term, "Bus: %s\n", status.bus_name);
    }
    solar_os_shell_io_printf(term,
                             "RX: %" PRIu32 " messages, %" PRIu32 " bytes\n",
                             status.rx_messages,
                             status.rx_bytes);
    solar_os_shell_io_printf(term,
                             "TX: %" PRIu32 " messages, %" PRIu32 " bytes\n",
                             status.tx_messages,
                             status.tx_bytes);
    solar_os_shell_io_printf(term,
                             "Unsupported: %" PRIu32 "  drops: rx=%" PRIu32
                             " tx=%" PRIu32 "\n",
                             status.parser_unsupported,
                             status.subscriber_drops,
                             status.tx_drops);
    if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term, "Last error: %s\n", esp_err_to_name(status.last_error));
    }
    solar_os_shell_io_printf(term, "CC streams: %u/%u\n",
                             (unsigned)solar_os_midi_cc_stream_count(),
                             (unsigned)SOLAR_OS_MIDI_CC_STREAM_MAX);
}

static void midi_cmd_stream_list(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_midi_cc_stream_count();
    if (count == 0U) {
        solar_os_shell_io_writeln(term, "No MIDI CC streams configured");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_midi_cc_stream_info_t info;
        if (!solar_os_midi_cc_stream_get(i, &info)) {
            continue;
        }
        solar_os_shell_io_printf(term, "%s: channel=%u cc=%u ", info.id,
                                 (unsigned)info.channel,
                                 (unsigned)info.controller);
        if (info.has_value) {
            solar_os_shell_io_printf(term, "value=%u ", (unsigned)info.value);
        } else {
            solar_os_shell_io_write(term, "value=waiting ");
        }
        solar_os_shell_io_printf(term, "updates=%" PRIu32 "\n", info.updates);
    }
}

static void midi_cmd_stream(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[2], "list") == 0) {
        midi_cmd_stream_list(term);
        return;
    }
    if (argc == 3 && strcmp(argv[2], "clear") == 0) {
        size_t removed = 0U;
        const esp_err_t error = solar_os_midi_cc_stream_clear(&removed);
        solar_os_shell_io_printf(term, "MIDI CC streams cleared: %u\n",
                                 (unsigned)removed);
        if (error != ESP_OK) {
            solar_os_shell_io_printf(term, "midi: stream clear failed: %s\n",
                                     solar_os_shell_error_text(error));
        }
        return;
    }
    if (argc == 5 && (strcmp(argv[2], "add") == 0 ||
                      strcmp(argv[2], "remove") == 0)) {
        uint8_t channel = 0U;
        uint8_t controller = 0U;
        if (!midi_parse_range(argv[3], 1U, 16U, &channel) ||
            !midi_parse_range(argv[4], 0U, 127U, &controller)) {
            solar_os_shell_io_writeln(
                term, "midi: channel must be 1..16 and controller 0..127");
            return;
        }
        const bool add = strcmp(argv[2], "add") == 0;
        const esp_err_t error = add ?
            solar_os_midi_cc_stream_add(channel, controller) :
            solar_os_midi_cc_stream_remove(channel, controller);
        if (error != ESP_OK) {
            solar_os_shell_io_printf(term, "midi: stream %s failed: %s\n",
                                     argv[2], solar_os_shell_error_text(error));
            return;
        }
        solar_os_shell_io_printf(term, "MIDI CC stream midi.cc.%u.%u %s\n",
                                 (unsigned)channel, (unsigned)controller,
                                 add ? "added" : "removed");
        return;
    }
    midi_print_usage(term);
}

static bool midi_parse_channel_message(solar_os_shell_io_t *term,
                                       int argc,
                                       char **argv,
                                       uint8_t kind,
                                       bool two_data,
                                       uint8_t default_data2,
                                       solar_os_midi_message_t *message)
{
    const int required = two_data && default_data2 == 0U ? 5 : 4;
    const int maximum = two_data ? 5 : 4;
    if (argc < required || argc > maximum) {
        midi_print_usage(term);
        return false;
    }
    uint8_t channel = 0U;
    if (!midi_parse_range(argv[2], 1U, 16U, &channel) ||
        !midi_parse_range(argv[3], 0U, 127U, &message->data1)) {
        solar_os_shell_io_writeln(term, "midi: channel must be 1..16 and data must be 0..127");
        return false;
    }
    message->status = kind | (uint8_t)(channel - 1U);
    message->length = two_data ? 3U : 2U;
    if (two_data) {
        message->data2 = default_data2;
        if (argc == 5 && !midi_parse_range(argv[4], 0U, 127U, &message->data2)) {
            solar_os_shell_io_writeln(term, "midi: data must be 0..127");
            return false;
        }
    }
    return true;
}

void solar_os_shell_cmd_midi(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        midi_cmd_status(term);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "stream") == 0) {
        midi_cmd_stream(term, argc, argv);
        return;
    }

    solar_os_midi_message_t message = {0};
    if (argc >= 2 && strcmp(argv[1], "note-on") == 0) {
        if (midi_parse_channel_message(term, argc, argv, 0x90U, true, 100U, &message)) {
            midi_send_message(term, &message);
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "note-off") == 0) {
        if (midi_parse_channel_message(term, argc, argv, 0x80U, true, 64U, &message)) {
            midi_send_message(term, &message);
        }
        return;
    }
    if (argc == 5 && strcmp(argv[1], "cc") == 0) {
        if (midi_parse_channel_message(term, argc, argv, 0xb0U, true, 0U, &message)) {
            midi_send_message(term, &message);
        }
        return;
    }
    if (argc == 4 && strcmp(argv[1], "program") == 0) {
        if (midi_parse_channel_message(term, argc, argv, 0xc0U, false, 0U, &message)) {
            midi_send_message(term, &message);
        }
        return;
    }
    if (argc >= 3 && argc <= 5 && strcmp(argv[1], "send") == 0) {
        if (!midi_parse_range(argv[2], 0x80U, 0xffU, &message.status)) {
            solar_os_shell_io_writeln(term, "midi: status must be 128..255 or 0x80..0xff");
            return;
        }
        message.length = (uint8_t)solar_os_midi_message_length(message.status);
        if (message.length == 0U || argc != (int)message.length + 2) {
            solar_os_shell_io_writeln(term, "midi: wrong number of data bytes for status");
            return;
        }
        if ((message.length > 1U &&
             !midi_parse_range(argv[3], 0U, 127U, &message.data1)) ||
            (message.length > 2U &&
             !midi_parse_range(argv[4], 0U, 127U, &message.data2))) {
            solar_os_shell_io_writeln(term, "midi: data must be 0..127");
            return;
        }
        midi_send_message(term, &message);
        return;
    }

    midi_print_usage(term);
}
