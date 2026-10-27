/*
 * Macintosh memory and device glue, based on epatel/esp32_mac's tme/emu.c.
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote the original file. As long as
 * you retain this notice you can do whatever you want with this stuff. If we
 * meet some day, and you think this stuff is worth it, you can buy me a beer
 * in return.
 */
#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"
#include "m68k.h"
#include "via.h"
#include "scc.h"
#include "ncr.h"
#include "iwm.h"
#include "rtc.h"
#include "mouse.h"

uint8_t *solar_os_mac_ram, *solar_os_mac_rom;
static bool overlay, alternate_screen;
static int via_irq, scc_irq;
static unsigned frame_cycles, frame_count;
void viaInit(void);
void mouseInit(void);
void iwmInit(void);

static uint8_t *memory_at(uint32_t address, bool write)
{
    address &= 0xffffffU;
    if (address < 0x400000U) {
        if (overlay) return !write && address < MAC_ROM_SIZE ? solar_os_mac_rom + address : NULL;
        return solar_os_mac_ram + address;
    }
    if (address < 0x400000U + MAC_ROM_SIZE) return !write ? solar_os_mac_rom + address - 0x400000U : NULL;
    if (address >= 0x600000U && address < 0x700000U)
        return solar_os_mac_ram + (address & (MAC_RAM_SIZE - 1U));
    return NULL;
}

static unsigned peripheral(uint32_t address, unsigned value, bool write)
{
    address &= 0xffffffU;
    /* Writes to overlaid ROM are ignored. No guest access can address host RAM. */
    if (address < 0x500000U) return (address ^ (address >> 8) ^ (address >> 16)) & 255U;
    if (address == 0xc00069U && write && solar_os_mac_floppy_hook(value)) return 0;
    if (address >= 0x580000U && address < 0x600000U) {
        unsigned reg = (address >> 4) & 7U, dack = (address >> 9) & 1U;
        if (write) ncrWrite(reg, dack, value); else return ncrRead(reg, dack);
    } else if (address >= 0x800000U && address < 0xc00000U) {
        if (write) sccWrite(address, value); else return sccRead(address);
    } else if (address >= 0xc00000U && address < 0xe00000U) {
        if (write) iwmWrite((address >> 9) & 15U, value); else return iwmRead((address >> 9) & 15U);
    } else if (address >= 0xe80000U && address < 0xf00000U) {
        if (write) viaWrite((address >> 9) & 15U, value); else return viaRead((address >> 9) & 15U);
    } else if (address >= 0xf00000U && address < 0xf80000U) {
        if (!write) return sccRead(address);
    }
    return 0xff;
}

unsigned int m68k_read_memory_8(unsigned int address)
{
    const uint8_t *p = memory_at(address, false);
    return p ? *p : peripheral(address, 0, false);
}
unsigned int m68k_read_memory_16(unsigned int address)
{
    const uint8_t *p = memory_at(address, false);
    if (p && !(address & 1U)) return __builtin_bswap16(*(const uint16_t *)p);
    return (m68k_read_memory_8(address) << 8) | m68k_read_memory_8(address + 1U);
}
unsigned int m68k_read_memory_32(unsigned int address)
{
    const uint8_t *p = memory_at(address, false);
    if (p && !(address & 3U)) return __builtin_bswap32(*(const uint32_t *)p);
    return (m68k_read_memory_16(address) << 16) | m68k_read_memory_16(address + 2U);
}
void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    uint8_t *p = memory_at(address, true);
    if (p) *p = (uint8_t)value; else (void)peripheral(address, value & 255U, true);
}
void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    uint8_t *p = memory_at(address, true);
    if (p && !(address & 1U)) { *(uint16_t *)p = __builtin_bswap16((uint16_t)value); return; }
    m68k_write_memory_8(address, value >> 8);
    m68k_write_memory_8(address + 1U, value);
}
void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    uint8_t *p = memory_at(address, true);
    if (p && !(address & 3U)) { *(uint32_t *)p = __builtin_bswap32(value); return; }
    m68k_write_memory_16(address, value >> 16);
    m68k_write_memory_16(address + 2U, value);
}

void viaIrq(int req) { via_irq = req; m68k_set_irq(scc_irq ? 2U : via_irq ? 1U : 0U); }
void sccIrq(int req) { scc_irq = req; m68k_set_irq(scc_irq ? 2U : via_irq ? 1U : 0U); }
void viaCbPortAWrite(unsigned int val)
{
    alternate_screen = (val & 0x40U) != 0;
    overlay = (val & 0x10U) != 0;
    iwmSetHeadSel(val & 0x20U);
}
void viaCbPortBWrite(unsigned int val)
{
    if (rtcCom(val & 4U, val & 1U, val & 2U)) viaSet(VIA_PORTB, 1);
    else viaClear(VIA_PORTB, 1);
}
void solar_os_mac_machine_init(void)
{
    overlay = true;
    alternate_screen = false;
    via_irq = scc_irq = 0;
    frame_cycles = frame_count = 0;
    solar_os_mac_render_reset();
    viaInit(); mouseInit(); iwmInit(); ncrInit();
    rtcInit((char *)solar_os_mac_pram);
    viaSet(VIA_PORTB, 8);
    sccInit();
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
}

void solar_os_mac_step(void)
{
    unsigned cycles = (unsigned)m68k_execute(1000);
    viaStep((int)(cycles / 10U));
    sccTick((int)(cycles / 10U));
    int mouse = mouseTick();
    if (mouse & MOUSE_BTN) viaClear(VIA_PORTB, 8); else viaSet(VIA_PORTB, 8);
    if (mouse & MOUSE_QXB) viaClear(VIA_PORTB, 16); else viaSet(VIA_PORTB, 16);
    if (mouse & MOUSE_QYB) viaClear(VIA_PORTB, 32); else viaSet(VIA_PORTB, 32);
    sccSetDcd(SCC_CHANA, mouse & MOUSE_QXA);
    sccSetDcd(SCC_CHANB, mouse & MOUSE_QYA);
    frame_cycles += cycles;
    if (frame_cycles >= 130560U) { /* 7.8336 MHz / 60 */
        frame_cycles -= 130560U;
        viaControlWrite(VIA_CA1, 1); viaControlWrite(VIA_CA1, 0);
        if (++frame_count == 60U) {
            frame_count = 0;
            viaControlWrite(VIA_CA2, 1); viaControlWrite(VIA_CA2, 0);
            rtcTick();
        }
    }
}

const uint8_t *solar_os_mac_framebuffer(void)
{
    return solar_os_mac_ram + solar_os_mac_screen - (alternate_screen ? 0x8000U : 0);
}
bool solar_os_mac_rom_valid(const uint8_t *rom)
{
    uint32_t identity = ((uint32_t)rom[0] << 24) | ((uint32_t)rom[1] << 16) |
        ((uint32_t)rom[2] << 8) | rom[3];
    if (identity != 0x4d1eeee1U && identity != 0x4d1eeae1U && identity != 0x4d1f8172U) return false;
    uint32_t sum = 0;
    for (unsigned i = 4; i < MAC_ROM_SIZE; i += 2) sum += ((unsigned)rom[i] << 8) | rom[i + 1];
    return sum == identity && rom[4] == 0 && rom[5] == 0x40 && rom[6] == 0 && rom[7] == 0x2a;
}

void solar_os_mac_diagnostic_rom(uint8_t *rom)
{
    /* Original diagnostic, no Apple code: disable ROM overlay, fill the
     * framebuffer with stripes, increment RAM $1000 forever. IRQs masked. */
    static const uint8_t program[] = {
        0x00,0x3f,0x00,0x00, 0x00,0x40,0x00,0x08,
        0x13,0xfc,0x00,0x00,0x00,0xef,0xe3,0xfe, /* move.b #0,$efe3fe */
        0x20,0x7c,0x00,0x3f,0xa7,0x00,           /* movea.l #screen,a0 */
        0x30,0x3c,0x15,0x5f,                     /* move.w #5471,d0 */
        0x20,0xfc,0xaa,0x55,0xaa,0x55,           /* move.l #pattern,(a0)+ */
        0x51,0xc8,0xff,0xf8,                     /* dbra d0,fill */
        0x52,0xb9,0x00,0x00,0x10,0x00,           /* addq.l #1,$1000 */
        0x60,0xf8,                               /* bra increment */
    };
    memset(rom, 0, MAC_ROM_SIZE);
    memcpy(rom, program, sizeof(program));
    unsigned screen = solar_os_mac_screen, stack = screen - 4096U;
    unsigned count = (solar_os_mac_stride * solar_os_mac_height + 3U) / 4U - 1U;
    for (unsigned i = 0; i < 4; ++i) {
        rom[i] = stack >> (24U - 8U * i);
        rom[18 + i] = screen >> (24U - 8U * i);
    }
    rom[24] = count >> 8; rom[25] = count;
}

static struct {
    uint8_t previous[1024U * 1024U / 8U];
    uint16_t source_byte[1024];
    uint32_t source_row[1024];
    uint8_t source_mask[1024];
    uint16_t width, height;
    unsigned guest_width, guest_height;
    bool valid;
} render;

void solar_os_mac_render_reset(void) { render.valid = false; }

bool solar_os_mac_render(uint8_t *pixels, uint16_t width, uint16_t height)
{
    if (!width || !height || width > 1024U || height > 1024U)
        solar_os_mac_fail("display dimensions unsupported");
    if (width != solar_os_mac_width || height != solar_os_mac_height)
        solar_os_mac_fail("display and guest resolutions differ");
    const uint8_t *fb = solar_os_mac_framebuffer();
    if (width != render.width || height != render.height ||
        render.guest_width != solar_os_mac_width || render.guest_height != solar_os_mac_height) {
        render.width = width; render.height = height;
        render.guest_width = solar_os_mac_width; render.guest_height = solar_os_mac_height;
        for (unsigned x = 0; x < width; ++x) {
            unsigned sx = x;
            render.source_byte[x] = sx / 8U;
            render.source_mask[x] = 0x80U >> (sx & 7U);
        }
        for (unsigned y = 0; y < height; ++y)
            render.source_row[y] = y * solar_os_mac_stride;
        render.valid = false;
    }
    /* The worker owns guest RAM: no guest writes race the comparison or copy.
     * Compare the packed screen before expanding or submitting it. */
    size_t size = solar_os_mac_stride * solar_os_mac_height;
    if (render.valid && !memcmp(render.previous, fb, size)) return false;
    memcpy(render.previous, fb, size);
    /* Expand each guest bit to one palette index, without resampling. */
    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *source = render.previous + render.source_row[y];
        uint8_t *out = pixels + y * width;
        for (unsigned x = 0; x < width; ++x)
            out[x] = (source[render.source_byte[x]] & render.source_mask[x]) != 0;
    }
    render.valid = true;
    return true;
}
