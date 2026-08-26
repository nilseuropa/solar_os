#pragma once

#include <stdint.h>

typedef enum {
    SOLAR_OS_PS2_MOUSE_DECODE_NONE,
    SOLAR_OS_PS2_MOUSE_DECODE_PACKET,
    SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED,
} solar_os_ps2_mouse_decode_result_t;

typedef struct {
    uint8_t packet[3];
    uint8_t count;
} solar_os_ps2_mouse_decoder_t;

typedef struct {
    int16_t delta_x;
    int16_t delta_y;
    uint8_t buttons;
} solar_os_ps2_mouse_packet_t;

void solar_os_ps2_mouse_decoder_reset(solar_os_ps2_mouse_decoder_t *decoder);
solar_os_ps2_mouse_decode_result_t solar_os_ps2_mouse_decode(
    solar_os_ps2_mouse_decoder_t *decoder,
    uint8_t byte,
    solar_os_ps2_mouse_packet_t *packet);
