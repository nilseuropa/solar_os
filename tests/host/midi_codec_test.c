#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_midi_codec.h"

static solar_os_midi_message_t feed_message(solar_os_midi_decoder_t *decoder,
                                             const uint8_t *bytes,
                                             size_t count)
{
    solar_os_midi_message_t message = {0};
    solar_os_midi_decode_result_t result = SOLAR_OS_MIDI_DECODE_NONE;
    for (size_t i = 0; i < count; i++) {
        result = solar_os_midi_decode_byte(decoder, bytes[i], &message);
    }
    assert(result == SOLAR_OS_MIDI_DECODE_MESSAGE);
    return message;
}

int main(void)
{
    solar_os_midi_decoder_t decoder;
    solar_os_midi_decoder_reset(&decoder);

    const uint8_t note_on[] = {0x92U, 60U, 100U};
    solar_os_midi_message_t message = feed_message(&decoder, note_on, sizeof(note_on));
    assert(message.status == 0x92U);
    assert(message.data1 == 60U);
    assert(message.data2 == 100U);
    assert(message.length == 3U);

    const uint8_t running[] = {61U, 101U};
    message = feed_message(&decoder, running, sizeof(running));
    assert(message.status == 0x92U);
    assert(message.data1 == 61U);
    assert(message.data2 == 101U);

    solar_os_midi_message_t realtime;
    assert(solar_os_midi_decode_byte(&decoder, 62U, &realtime) ==
           SOLAR_OS_MIDI_DECODE_NONE);
    assert(solar_os_midi_decode_byte(&decoder, 0xf8U, &realtime) ==
           SOLAR_OS_MIDI_DECODE_MESSAGE);
    assert(realtime.status == 0xf8U && realtime.length == 1U);
    assert(solar_os_midi_decode_byte(&decoder, 102U, &message) ==
           SOLAR_OS_MIDI_DECODE_MESSAGE);
    assert(message.status == 0x92U && message.data1 == 62U && message.data2 == 102U);

    const uint8_t program[] = {0xc0U, 7U};
    message = feed_message(&decoder, program, sizeof(program));
    assert(message.status == 0xc0U && message.data1 == 7U && message.length == 2U);

    assert(solar_os_midi_decode_byte(&decoder, 0xf0U, &message) ==
           SOLAR_OS_MIDI_DECODE_UNSUPPORTED);
    assert(solar_os_midi_decode_byte(&decoder, 1U, &message) ==
           SOLAR_OS_MIDI_DECODE_NONE);
    assert(solar_os_midi_decode_byte(&decoder, 0xf7U, &message) ==
           SOLAR_OS_MIDI_DECODE_UNSUPPORTED);

    const solar_os_midi_message_t control = {
        .status = 0xb4U,
        .data1 = 64U,
        .data2 = 127U,
        .length = 3U,
    };
    uint8_t encoded[3] = {0};
    assert(solar_os_midi_message_valid(&control));
    assert(solar_os_midi_encode(&control, encoded) == 3U);
    assert(encoded[0] == 0xb4U && encoded[1] == 64U && encoded[2] == 127U);

    char monitor[32];
    const solar_os_midi_message_t monitor_cc = {
        .status = 0xb0U,
        .data1 = 74U,
        .data2 = 64U,
        .length = 3U,
    };
    assert(solar_os_midi_format_monitor(&monitor_cc, monitor,
                                        sizeof(monitor)) > 0U);
    assert(strcmp(monitor, "CC: 1 74 64") == 0);

    const solar_os_midi_message_t monitor_key = {
        .status = 0x92U,
        .data1 = 60U,
        .data2 = 100U,
        .length = 3U,
    };
    assert(solar_os_midi_format_monitor(&monitor_key, monitor,
                                        sizeof(monitor)) > 0U);
    assert(strcmp(monitor, "KEY: 3 60 100") == 0);

    const solar_os_midi_message_t monitor_release = {
        .status = 0x82U,
        .data1 = 60U,
        .data2 = 55U,
        .length = 3U,
    };
    assert(solar_os_midi_format_monitor(&monitor_release, monitor,
                                        sizeof(monitor)) > 0U);
    assert(strcmp(monitor, "KEY: 3 60 0") == 0);
    assert(solar_os_midi_format_monitor(&message, monitor, sizeof(monitor)) ==
           0U);
    assert(solar_os_midi_format_monitor(&monitor_cc, monitor, 4U) == 0U);

    puts("midi_codec_test: ok");
    return 0;
}
