#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_MIDI_DATA_MAX 127U

typedef struct {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t length;
} solar_os_midi_message_t;

typedef struct {
    uint8_t running_status;
    uint8_t active_status;
    uint8_t data[2];
    uint8_t data_count;
    bool sysex;
} solar_os_midi_decoder_t;

typedef enum {
    SOLAR_OS_MIDI_DECODE_NONE,
    SOLAR_OS_MIDI_DECODE_MESSAGE,
    SOLAR_OS_MIDI_DECODE_UNSUPPORTED,
} solar_os_midi_decode_result_t;

void solar_os_midi_decoder_reset(solar_os_midi_decoder_t *decoder);
solar_os_midi_decode_result_t solar_os_midi_decode_byte(
    solar_os_midi_decoder_t *decoder,
    uint8_t byte,
    solar_os_midi_message_t *message);

size_t solar_os_midi_message_length(uint8_t status);
bool solar_os_midi_message_valid(const solar_os_midi_message_t *message);
size_t solar_os_midi_encode(const solar_os_midi_message_t *message,
                            uint8_t output[3]);
size_t solar_os_midi_format_monitor(const solar_os_midi_message_t *message,
                                    char *output,
                                    size_t output_len);
