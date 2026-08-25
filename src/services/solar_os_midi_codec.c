#include "solar_os_midi_codec.h"

#include <stdio.h>
#include <string.h>

static bool midi_status_supported(uint8_t status)
{
    if (status < 0x80U) {
        return false;
    }
    if (status < 0xf0U) {
        return true;
    }
    switch (status) {
    case 0xf1U:
    case 0xf2U:
    case 0xf3U:
    case 0xf6U:
    case 0xf7U:
    case 0xf8U:
    case 0xfaU:
    case 0xfbU:
    case 0xfcU:
    case 0xfeU:
    case 0xffU:
        return true;
    default:
        return false;
    }
}

size_t solar_os_midi_message_length(uint8_t status)
{
    if (!midi_status_supported(status)) {
        return 0;
    }
    if (status < 0xf0U) {
        const uint8_t kind = status & 0xf0U;
        return kind == 0xc0U || kind == 0xd0U ? 2U : 3U;
    }
    switch (status) {
    case 0xf1U:
    case 0xf3U:
        return 2U;
    case 0xf2U:
        return 3U;
    default:
        return 1U;
    }
}

bool solar_os_midi_message_valid(const solar_os_midi_message_t *message)
{
    if (message == NULL || message->length != solar_os_midi_message_length(message->status)) {
        return false;
    }
    if (message->length > 1U && message->data1 > SOLAR_OS_MIDI_DATA_MAX) {
        return false;
    }
    return message->length <= 2U || message->data2 <= SOLAR_OS_MIDI_DATA_MAX;
}

size_t solar_os_midi_encode(const solar_os_midi_message_t *message,
                            uint8_t output[3])
{
    if (output == NULL || !solar_os_midi_message_valid(message)) {
        return 0;
    }
    output[0] = message->status;
    if (message->length > 1U) {
        output[1] = message->data1;
    }
    if (message->length > 2U) {
        output[2] = message->data2;
    }
    return message->length;
}

size_t solar_os_midi_format_monitor(const solar_os_midi_message_t *message,
                                    char *output,
                                    size_t output_len)
{
    if (!solar_os_midi_message_valid(message) || output == NULL ||
        output_len == 0U || message->status >= 0xf0U) {
        return 0U;
    }
    const uint8_t kind = message->status & 0xf0U;
    const unsigned channel = (unsigned)(message->status & 0x0fU) + 1U;
    int written = 0;
    if (kind == 0xb0U) {
        written = snprintf(output, output_len, "CC: %u %u %u", channel,
                           (unsigned)message->data1,
                           (unsigned)message->data2);
    } else if (kind == 0x90U) {
        written = snprintf(output, output_len, "KEY: %u %u %u", channel,
                           (unsigned)message->data1,
                           (unsigned)message->data2);
    } else if (kind == 0x80U) {
        written = snprintf(output, output_len, "KEY: %u %u 0", channel,
                           (unsigned)message->data1);
    }
    return written > 0 && (size_t)written < output_len ? (size_t)written : 0U;
}

void solar_os_midi_decoder_reset(solar_os_midi_decoder_t *decoder)
{
    if (decoder != NULL) {
        memset(decoder, 0, sizeof(*decoder));
    }
}

static solar_os_midi_decode_result_t midi_emit_status(
    solar_os_midi_decoder_t *decoder,
    uint8_t status,
    solar_os_midi_message_t *message)
{
    const size_t length = solar_os_midi_message_length(status);
    if (length == 0U) {
        decoder->active_status = 0U;
        decoder->data_count = 0U;
        return SOLAR_OS_MIDI_DECODE_UNSUPPORTED;
    }
    decoder->active_status = status;
    decoder->data_count = 0U;
    if (status < 0xf0U) {
        decoder->running_status = status;
    } else {
        decoder->running_status = 0U;
    }
    if (length != 1U) {
        return SOLAR_OS_MIDI_DECODE_NONE;
    }
    *message = (solar_os_midi_message_t) {
        .status = status,
        .length = 1U,
    };
    decoder->active_status = 0U;
    return SOLAR_OS_MIDI_DECODE_MESSAGE;
}

solar_os_midi_decode_result_t solar_os_midi_decode_byte(
    solar_os_midi_decoder_t *decoder,
    uint8_t byte,
    solar_os_midi_message_t *message)
{
    if (decoder == NULL || message == NULL) {
        return SOLAR_OS_MIDI_DECODE_UNSUPPORTED;
    }
    memset(message, 0, sizeof(*message));

    /* Realtime messages may appear between any two MIDI bytes. */
    if (byte >= 0xf8U) {
        const size_t length = solar_os_midi_message_length(byte);
        if (length == 0U) {
            return SOLAR_OS_MIDI_DECODE_UNSUPPORTED;
        }
        message->status = byte;
        message->length = 1U;
        return SOLAR_OS_MIDI_DECODE_MESSAGE;
    }

    if (decoder->sysex) {
        if (byte == 0xf7U) {
            decoder->sysex = false;
            return SOLAR_OS_MIDI_DECODE_UNSUPPORTED;
        }
        if ((byte & 0x80U) == 0U) {
            return SOLAR_OS_MIDI_DECODE_NONE;
        }
        decoder->sysex = false;
    }

    if ((byte & 0x80U) != 0U) {
        if (byte == 0xf0U) {
            decoder->running_status = 0U;
            decoder->active_status = 0U;
            decoder->data_count = 0U;
            decoder->sysex = true;
            return SOLAR_OS_MIDI_DECODE_UNSUPPORTED;
        }
        return midi_emit_status(decoder, byte, message);
    }

    if (decoder->active_status == 0U) {
        decoder->active_status = decoder->running_status;
        decoder->data_count = 0U;
    }
    const size_t length = solar_os_midi_message_length(decoder->active_status);
    if (length < 2U || decoder->data_count >= sizeof(decoder->data)) {
        decoder->active_status = 0U;
        decoder->data_count = 0U;
        return SOLAR_OS_MIDI_DECODE_UNSUPPORTED;
    }

    decoder->data[decoder->data_count++] = byte;
    if (decoder->data_count + 1U < length) {
        return SOLAR_OS_MIDI_DECODE_NONE;
    }

    *message = (solar_os_midi_message_t) {
        .status = decoder->active_status,
        .data1 = decoder->data[0],
        .data2 = length > 2U ? decoder->data[1] : 0U,
        .length = (uint8_t)length,
    };
    decoder->data_count = 0U;
    decoder->active_status = decoder->running_status;
    return SOLAR_OS_MIDI_DECODE_MESSAGE;
}
