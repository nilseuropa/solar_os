/* Screen-size ROM patches adapted from umac's rom.c and Mini vMac SCRNHACK.h.
 * Mini vMac cursor trampoline patches: Copyright (C) 2007 Paul C. Pratt.
 * This combined file is licensed under GPL-2.0; see ../COPYING.
 * Original umac permission notice follows:
 * Copyright 2024 Matt Evans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"

unsigned solar_os_mac_width = MAC_WIDTH, solar_os_mac_height = MAC_HEIGHT;
unsigned solar_os_mac_stride = MAC_WIDTH / 8U, solar_os_mac_screen = MAC_SCREEN;

static void word(unsigned offset, unsigned value)
{
    solar_os_mac_rom[offset] = value >> 8;
    solar_os_mac_rom[offset + 1] = value;
}
static void longword(unsigned offset, unsigned value)
{
    word(offset, value >> 16); word(offset + 2, value);
}
static unsigned coordinate(unsigned x, unsigned y)
{
    return solar_os_mac_screen + solar_os_mac_stride * y + x / 8U;
}
bool solar_os_mac_video_configure(unsigned width, unsigned height, bool patch_rom)
{
    solar_os_mac_width = MAC_WIDTH; solar_os_mac_height = MAC_HEIGHT;
    solar_os_mac_stride = MAC_WIDTH / 8U; solar_os_mac_screen = MAC_SCREEN;
    solar_os_mac_render_reset();
    if (width < 64U || width > 1024U || height < 64U || height > 1024U) return false;
    if (patch_rom && memcmp(solar_os_mac_rom, "\x4d\x1f\x81\x72", 4))
        return width == MAC_WIDTH && height == MAC_HEIGHT;
    solar_os_mac_width = width; solar_os_mac_height = height;
    /* QuickDraw needs word-aligned rows, independently of the visible width. */
    solar_os_mac_stride = ((width + 15U) / 16U) * 2U;
    unsigned size = (solar_os_mac_stride * height + 3U) & ~3U, distance = size + 0x380U;
    solar_os_mac_screen = MAC_RAM_SIZE - distance;
    if (!patch_rom || (width == MAC_WIDTH && height == MAC_HEIGHT)) return true;
    word(0xd92, 0xb381); /* checksum comparison, in-memory copy only */
    /* Borrow unused TestSoftware bytes for a 32-bit screen allocation delta. */
    word(0x42, 0x6000); word(0x44, 0x62 - 0x44);
    word(0x46, 0x9bfc); longword(0x48, distance);
    word(0x4c, 0x6000); word(0x4e, 0x3a4 - 0x4e);
    word(0x3a0, 0x6000); word(0x3a2, (0x46 - 0x3a2) & 0xffffU);
    longword(0x8a, solar_os_mac_screen); longword(0x146, solar_os_mac_screen);
    longword(0x164, coordinate(width / 2 - 24, height / 2 + 8));
    word(0x188, solar_os_mac_stride); word(0x194, solar_os_mac_stride);
    word(0x19c, 6 * solar_os_mac_stride - 1);
    longword(0x1a4, coordinate(width / 2 - 8, height / 2 + 16));
    word(0x1ee, size / 4 - 1);
    longword(0xf0c, coordinate(width / 2 - 16, height / 2 - 26));
    longword(0xf18, coordinate(width / 2 - 8, height / 2 - 20));
    longword(0x7e0, coordinate(width / 2 - 16, height / 2 - 26));
    longword(0x7f2, coordinate(width / 2 - 8, height / 2 - 11));
    word(0x474, solar_os_mac_stride);
    word(0x494, height); word(0x498, width);
    word(0xa0e, height); word(0xa10, width);
    word(0xee2, solar_os_mac_stride - 4); word(0xef2, solar_os_mac_stride);
    word(0xf36, solar_os_mac_stride - 2);
    if (solar_os_mac_stride < 128U) solar_os_mac_rom[0x1cd1] = solar_os_mac_stride;
    else {
        /* MOVEQ sign-extends 128. Use the replaced Sony driver's spare bytes
         * for full-width cursor strides; floppy_open installs that driver
         * even without an attached floppy, so no original Sony code is used. */
        word(0x1ccc, 0x4eb9); longword(0x1cce, 0x417f00);
        word(0x17f00, 0x41f8); word(0x17f02, 0x088c);
        word(0x17f04, 0x203c); longword(0x17f06, solar_os_mac_stride);
        word(0x17f0a, 0x4e75);
        word(0x1d92, 0x4eb9); longword(0x1d94, 0x417f0c);
        word(0x17f0c, 0x2a3c); longword(0x17f0e, solar_os_mac_stride);
        word(0x17f12, 0xc2c5); word(0x17f14, 0xd3c1); word(0x17f16, 0x4e75);
    }
    word(0x1d48, width - 32); word(0x1d4e, width - 32);
    word(0x1d6e, height - 16); word(0x1d74, height);
    if (solar_os_mac_stride < 128U) solar_os_mac_rom[0x1d93] = solar_os_mac_stride;
    word(0x1e68, height); word(0x1e6e, width); word(0x1e82, height);
    return true;
}
