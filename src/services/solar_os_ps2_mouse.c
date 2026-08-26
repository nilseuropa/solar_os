#include "solar_os_ps2_mouse.h"

#include <string.h>

#define PS2_MOUSE_ALWAYS_ONE (1U << 3)
#define PS2_MOUSE_X_SIGN (1U << 4)
#define PS2_MOUSE_Y_SIGN (1U << 5)
#define PS2_MOUSE_X_OVERFLOW (1U << 6)
#define PS2_MOUSE_Y_OVERFLOW (1U << 7)

void solar_os_ps2_mouse_decoder_reset(solar_os_ps2_mouse_decoder_t *decoder)
{
    if (decoder != NULL) {
        memset(decoder, 0, sizeof(*decoder));
    }
}

solar_os_ps2_mouse_decode_result_t solar_os_ps2_mouse_decode(
    solar_os_ps2_mouse_decoder_t *decoder,
    uint8_t byte,
    solar_os_ps2_mouse_packet_t *packet)
{
    if (decoder == NULL || packet == NULL) {
        return SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED;
    }
    if (decoder->count == 0 && (byte & PS2_MOUSE_ALWAYS_ONE) == 0) {
        return SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED;
    }
    decoder->packet[decoder->count++] = byte;
    if (decoder->count < sizeof(decoder->packet)) {
        return SOLAR_OS_PS2_MOUSE_DECODE_NONE;
    }

    decoder->count = 0;
    const uint8_t flags = decoder->packet[0];
    if ((flags & (PS2_MOUSE_X_OVERFLOW | PS2_MOUSE_Y_OVERFLOW)) != 0) {
        return SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED;
    }
    int16_t delta_x = decoder->packet[1];
    int16_t device_delta_y = decoder->packet[2];
    if ((flags & PS2_MOUSE_X_SIGN) != 0 && delta_x != 0) {
        delta_x -= 256;
    }
    if ((flags & PS2_MOUSE_Y_SIGN) != 0 && device_delta_y != 0) {
        device_delta_y -= 256;
    }
    *packet = (solar_os_ps2_mouse_packet_t) {
        .delta_x = delta_x,
        /* PS/2 positive Y is up; SolarOS display coordinates grow downward. */
        .delta_y = (int16_t)-device_delta_y,
        .buttons = flags & 0x07U,
    };
    return SOLAR_OS_PS2_MOUSE_DECODE_PACKET;
}
