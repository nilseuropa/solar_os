#pragma once

#include <stdint.h>

#include "solar_os_native_bus_abi.h"

void solar_os_ps2_mouse_decoder_reset(solar_os_ps2_mouse_decoder_t *decoder);
solar_os_ps2_mouse_decode_result_t solar_os_ps2_mouse_decode(
    solar_os_ps2_mouse_decoder_t *decoder,
    uint8_t byte,
    solar_os_ps2_mouse_packet_t *packet);
