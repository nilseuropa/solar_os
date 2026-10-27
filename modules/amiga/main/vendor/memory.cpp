/* Mappa memoria A500 (24 bit):
 *   0x000000-0x07FFFF  Chip RAM 512KB   (ROM in overlay finché OVL=1)
 *   0xBFD000-0xBFDF00  CIA-B (byte alto, indirizzi pari, reg = A8-A11)
 *   0xBFE001-0xBFEF01  CIA-A (byte basso, indirizzi dispari, reg = A8-A11)
 *   0xDFF000-0xDFF1FF  registri custom (word)
 *   0xF80000-0xFBFFFF  mirror Kickstart
 *   0xFC0000-0xFFFFFF  Kickstart 1.3 256KB (vettore reset -> 0xFC00D2)
 * OVL: CIA-A PA0. Al reset DDRA=0 -> pin in pull-up -> overlay attivo.
 * Il Kickstart imposta DDRA bit0=1 e PRA bit0=0 -> overlay rimosso.
 */
#include "a500.h"
#include "../rtc.h"
#include <string.h>
#include <stdarg.h>
extern "C" {
#include "m68k.h"
}

#ifdef SOLAR_OS_AMIGA
uint8_t *chip_ram = nullptr;   // 512KB in PSRAM (allocata in setup)
uint8_t *slow_ram = nullptr;   // 512KB slow RAM @ 0xC00000 (PSRAM)
uint8_t *kick_rom = nullptr;   // 256KB in PSRAM (caricata in setup)
#else
uint8_t chip_ram[CHIP_SIZE];
uint8_t kick_rom[ROM_SIZE];
#endif
int ovl = 1;

FILE *tracef = NULL;

void logmsg(const char *, ...) {}

#ifndef SOLAR_OS_AMIGA
int mem_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { logmsg("ERRORE: impossibile aprire %s\n", path); return -1; }
    size_t n = fread(kick_rom, 1, ROM_SIZE, f);
    fclose(f);
    if (n != ROM_SIZE) { logmsg("ERRORE: dimensione ROM %zu != 262144\n", n); return -1; }
    return 0;
}
#endif

void mem_reset(void)
{
    /* Su Arduino chip_ram e' un puntatore PSRAM: sizeof(chip_ram) resetterebbe
       solo 4/8 byte. Serve sempre la dimensione reale della Chip RAM. */
    if (chip_ram) memset(chip_ram, 0, CHIP_SIZE);
    if (slow_ram) memset(slow_ram, 0, SLOW_SIZE);
    solar_os_amiga_rtc_reset();
    ovl = 1;
}

/* log una-tantum per accessi a zone non mappate */
static void unmapped(const char *op, uint32_t a)
{
    static uint32_t seen[64]; static int nseen = 0;
    uint32_t page = a >> 16;
    for (int i = 0; i < nseen; i++) if (seen[i] == page) return;
    if (nseen < 64) seen[nseen++] = page;
    logmsg("[MEM] %s non mappato: %06X (PC=%06X)\n", op, a,
           m68k_get_reg(NULL, M68K_REG_PPC));
}

static inline uint8_t rom_b(uint32_t a) { return kick_rom[a & (ROM_SIZE - 1)]; }

static uint8_t read8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < CHIP_SIZE)  return ovl ? rom_b(a) : chip_ram[a];
    if (slow_ram && a >= SLOW_BASE && a < SLOW_BASE + SLOW_SIZE) return slow_ram[a - SLOW_BASE];
    if (a >= 0xF80000)  return rom_b(a);
    if ((a & 0xFFFFC0) == 0xDC0000) return (a & 1) ? solar_os_amiga_rtc_read((a >> 2) & 15) : 0;
    if ((a & 0xFFF000) == 0xBFE000 && (a & 1)) return cia_read(&cia_a, (a >> 8) & 0xF);
    if ((a & 0xFFF000) == 0xBFD000 && !(a & 1)) return cia_read(&cia_b, (a >> 8) & 0xF);
    if ((a & 0xFFF000) == 0xDFF000) {
        uint16_t w = custom_read(a & 0x1FE);
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    unmapped("R8", a);
    return 0;
}

static void write8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    if (a < CHIP_SIZE) { if (!ovl) chip_ram[a] = v; return; }  /* scritture in overlay ignorate */
    if (slow_ram && a >= SLOW_BASE && a < SLOW_BASE + SLOW_SIZE) { slow_ram[a - SLOW_BASE] = v; return; }
    if ((a & 0xFFFFC0) == 0xDC0000) { if (a & 1) solar_os_amiga_rtc_write((a >> 2) & 15, v); return; }
    if ((a & 0xFFF000) == 0xBFE000 && (a & 1)) { cia_write(&cia_a, (a >> 8) & 0xF, v); return; }
    if ((a & 0xFFF000) == 0xBFD000 && !(a & 1)) { cia_write(&cia_b, (a >> 8) & 0xF, v); return; }
    if ((a & 0xFFF000) == 0xDFF000) {
        /* scrittura byte su registro word: replica sul bus (comportamento reale indefinito,
           qui replichiamo il byte — il KS non lo fa mai sui registri critici) */
        custom_write(a & 0x1FE, (uint16_t)v << 8 | v);
        return;
    }
    if (a >= 0xF80000) return;  /* ROM: scritture ignorate */
    unmapped("W8", a);
}

extern "C" {

int solar_os_amiga_beam_wait(uint32_t pc, const unsigned int *address_regs)
{
    // MOVE.L 4(An),Dn; ANDI.L #$1ff00,Dn; CMPI.L #line,Dn;
    // BNE.S loop. Only this side-effect-free vertical-beam polling loop is
    // eligible: VPOSR/VHPOSR remain constant during one m68k_execute slice.
    pc &= 0xffffff;
    const uint8_t *p;
    if (!ovl && pc <= CHIP_SIZE - 18) p = chip_ram + pc;
    else if (pc >= 0xf80000 && (pc & (ROM_SIZE - 1)) <= ROM_SIZE - 18)
        p = kick_rom + (pc & (ROM_SIZE - 1));
    else return 0;
    auto word = [](const uint8_t *b) { return (unsigned)b[0] << 8 | b[1]; };
    unsigned move = word(p), reg = (move >> 9) & 7;
    return (move & 0xf1f8) == 0x2028 && word(p + 2) == 4 &&
        ((address_regs[move & 7] + 4) & 0xffffff) == 0xdff004 &&
        word(p + 4) == (0x0280 | reg) && word(p + 6) == 1 && word(p + 8) == 0xff00 &&
        word(p + 10) == (0x0c80 | reg) && word(p + 16) == 0x66ee;
}

unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); }

unsigned int m68k_read_memory_16(unsigned int a)
{
    a &= 0xFFFFFE;
    if (a < CHIP_SIZE) {
        const uint8_t *p = ovl ? &kick_rom[a & (ROM_SIZE - 1)] : &chip_ram[a];
        return (unsigned)p[0] << 8 | p[1];
    }
    if (a >= 0xF80000) {
        const uint8_t *p = &kick_rom[a & (ROM_SIZE - 1)];
        return (unsigned)p[0] << 8 | p[1];
    }
    if (slow_ram && a >= SLOW_BASE && a < SLOW_BASE + SLOW_SIZE) {
        const uint8_t *p = &slow_ram[a - SLOW_BASE];
        return (unsigned)p[0] << 8 | p[1];
    }
    if ((a & 0xFFF000) == 0xDFF000) return custom_read(a & 0x1FE);
    return (unsigned)read8(a) << 8 | read8(a + 1);
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_8(unsigned int a, unsigned int v) { write8(a, (uint8_t)v); }

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    a &= 0xFFFFFE;
    if (a < CHIP_SIZE) {
        if (!ovl) { chip_ram[a] = (uint8_t)(v >> 8); chip_ram[a + 1] = (uint8_t)v; }
        return;
    }
    if ((a & 0xFFF000) == 0xDFF000) { custom_write(a & 0x1FE, (uint16_t)v); return; }
    write8(a, (uint8_t)(v >> 8)); write8(a + 1, (uint8_t)v);
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

/* letture per il disassembler (senza effetti collaterali sui CIA/custom) */
unsigned int m68k_read_disassembler_16(unsigned int a)
{
    a &= 0xFFFFFE;
    if (a < CHIP_SIZE && !ovl) return (unsigned)chip_ram[a] << 8 | chip_ram[a + 1];
    const uint8_t *p = &kick_rom[a & (ROM_SIZE - 1)];
    return (unsigned)p[0] << 8 | p[1];
}
unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return m68k_read_disassembler_16(a) << 16 | m68k_read_disassembler_16(a + 2);
}

} /* extern C */
