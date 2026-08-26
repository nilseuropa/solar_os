#include <assert.h>
#include <stdio.h>

#include "solar_os_ps2_mouse.h"

static solar_os_ps2_mouse_packet_t feed_packet(
    solar_os_ps2_mouse_decoder_t *decoder,
    uint8_t flags,
    uint8_t x,
    uint8_t y)
{
    solar_os_ps2_mouse_packet_t packet = {0};
    assert(solar_os_ps2_mouse_decode(decoder, flags, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_NONE);
    assert(solar_os_ps2_mouse_decode(decoder, x, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_NONE);
    assert(solar_os_ps2_mouse_decode(decoder, y, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_PACKET);
    return packet;
}

int main(void)
{
    solar_os_ps2_mouse_decoder_t decoder;
    solar_os_ps2_mouse_decoder_reset(&decoder);

    solar_os_ps2_mouse_packet_t packet = feed_packet(&decoder, 0x09, 12, 7);
    assert(packet.delta_x == 12);
    assert(packet.delta_y == -7);
    assert(packet.buttons == 1);

    packet = feed_packet(&decoder, 0x38, 0xf6, 0xfb);
    assert(packet.delta_x == -10);
    assert(packet.delta_y == 5);
    assert(packet.buttons == 0);

    assert(solar_os_ps2_mouse_decode(&decoder, 0x00, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED);
    packet = feed_packet(&decoder, 0x08, 0, 0);
    assert(packet.delta_x == 0 && packet.delta_y == 0);

    assert(solar_os_ps2_mouse_decode(&decoder, 0x48, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_NONE);
    assert(solar_os_ps2_mouse_decode(&decoder, 0xff, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_NONE);
    assert(solar_os_ps2_mouse_decode(&decoder, 0, &packet) ==
           SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED);

    puts("ps2 mouse decoder tests: ok");
    return 0;
}
