#pragma once

#include "solar_os_native_midi_abi.h"

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
