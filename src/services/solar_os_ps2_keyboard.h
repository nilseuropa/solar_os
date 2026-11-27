#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "solar_os_native_bus_abi.h"

void solar_os_ps2_keyboard_decoder_reset(solar_os_ps2_keyboard_decoder_t *decoder);
solar_os_ps2_decode_result_t solar_os_ps2_keyboard_decode(
    solar_os_ps2_keyboard_decoder_t *decoder,
    uint8_t byte,
    solar_os_ps2_key_transition_t *transition);
