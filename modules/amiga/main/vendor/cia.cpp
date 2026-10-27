/* CIA 8520 (MOS). Perimetro Fase 0:
 *   - PRA/PRB/DDR (OVL su CIA-A PA0, LED su PA1, linee drive su CIA-B PRB)
 *   - Timer A/B in modalità continua/one-shot, clock E (CPU/10)
 *   - ICR con maschera -> INTREQ bit 3 (CIA-A/PORTS) o bit 13 (CIA-B/EXTER)
 *   - TOD contatore 24 bit con latch di lettura (per KS1.3 basta che avanzi:
 *     CIA-A TOD è agganciato al tick 50Hz, CIA-B TOD all'HSYNC)
 * Fuori perimetro: SDR/tastiera, FLAG, linee handshake.
 */
#include "a500.h"
#include "../keyboard.h"
#include <stdio.h>
#include <string.h>
extern "C" {
#include "m68k.h"
}

cia_t cia_a, cia_b;

static void cia_init(cia_t *c, const char *name, int irqbit)
{
    memset(c, 0, sizeof *c);
    c->name = name;
    c->irq_intreq_bit = irqbit;
    c->ta = c->tb = c->ta_latch = c->tb_latch = 0xFFFF;
    c->tod_running = 1;
    /* pull-up: linee di porta lette alte finché DDR=0 */
    c->pra = 0xFF; c->prb = 0xFF;
}

void cia_reset(void)
{
    cia_init(&cia_a, "CIA-A", 3);
    cia_init(&cia_b, "CIA-B", 13);
    ovl = 1;
    solar_os_amiga_keyboard_reset();
}

static void icr_set(cia_t *c, uint8_t bit)
{
    c->icr_data |= bit;
    if (c->icr_mask & bit & 0x1F) {
        c->icr_data |= 0x80;
        intreq_set(c->irq_intreq_bit);
    }
}

/* valore visto sul pin: bit di output dal PRA, bit di input in pull-up (=1).
 * Fase 0: nessuna periferica pilota le linee -> ingressi alti. */
static inline uint8_t port_in(uint8_t pr, uint8_t ddr) { return (pr & ddr) | ~ddr; }

static void ovl_update(void)
{
    int new_ovl = port_in(cia_a.pra, cia_a.ddra) & 1;
    if (new_ovl != ovl) {
        ovl = new_ovl;
        logmsg("[OVL] overlay %s (PC=%06X)\n", ovl ? "ATTIVO" : "RIMOSSO",
               m68k_get_reg(NULL, M68K_REG_PPC));
    }
}

static uint8_t cia_read_inner(cia_t *c, int reg);

extern int drive_selected(void);

uint8_t cia_read(cia_t *c, int reg)
{
    uint8_t v = cia_read_inner(c, reg);
    /* sonda temporale click: ogni lettura del gameport nella finestra 6435-6460 */
    if (rdlogf && c == &cia_a && reg == 0 && cur_frame >= 6435 && cur_frame <= 6460)
        fprintf(rdlogf, "f=%d PRA=%02X FIR0=%d (PC=%06X)\n",
                cur_frame, v, (v >> 6) & 1, m68k_get_reg(NULL, M68K_REG_PPC));
    /* cacciatore /CHNG fantasma: TD legge PRA con bit2 alto = "disco presente" */
    if (rdlogf && c == &cia_a && reg == 0 && (v & 0x04)) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PPC);
        if (pc >= 0xFE9000 && pc < 0xFEA000)
            fprintf(rdlogf, "f=%d PHANTOM /CHNG=1 PRA=%02X (PC=%06X, sel=%d)\n",
                    cur_frame, v, pc, drive_selected());
    }
    if (rdlogf && cur_frame >= 140 && cur_frame <= 180 && reg != 0x8 && reg != 0x9)
        fprintf(rdlogf, "f=%d %s R %X = %02X (PC=%06X)\n",
                cur_frame, c->name, reg, v, m68k_get_reg(NULL, M68K_REG_PPC));
    return v;
}

static uint8_t cia_read_inner(cia_t *c, int reg)
{
    switch (reg) {
    case 0x0: {
        uint8_t v = port_in(c->pra, c->ddra);
        if (c == &cia_a) {
            /* bit 2-5: linee drive (solo i bit configurati come input) */
            uint8_t drv = drive_pra_bits() & 0x3C & ~c->ddra;
            v = (v & (uint8_t)~(0x3C & ~c->ddra)) | drv;
            if (input_lmb() && !(c->ddra & 0x40))
                v &= (uint8_t)~0x40;      /* /FIR0: tasto sinistro attivo basso */
            if (input_joy_fire() && !(c->ddra & 0x80))
                v &= (uint8_t)~0x80;      /* /FIR1: fuoco joystick porta 1 (bit 7) */
        }
        return v;
    }
    case 0x1: return port_in(c->prb, c->ddrb);
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return (uint8_t)c->ta;
    case 0x5: return (uint8_t)(c->ta >> 8);
    case 0x6: return (uint8_t)c->tb;
    case 0x7: return (uint8_t)(c->tb >> 8);
    case 0x8: {  /* TOD low: rilascia il latch */
        uint32_t v = c->tod_latched ? c->tod_latch : c->tod;
        c->tod_latched = 0;
        return (uint8_t)v;
    }
    case 0x9: return (uint8_t)((c->tod_latched ? c->tod_latch : c->tod) >> 8);
    case 0xA: {  /* TOD high: latch */
        if (!c->tod_latched) { c->tod_latch = c->tod; c->tod_latched = 1; }
        return (uint8_t)(c->tod_latch >> 16);
    }
    case 0xB: return 0;  /* 8520: non usato */
    case 0xC: return c->sdr;
    case 0xD: {          /* ICR: lettura azzera */
        uint8_t v = c->icr_data;
        c->icr_data = 0;
        return v;
    }
    case 0xE: return c->cra;
    case 0xF: return c->crb;
    }
    return 0;
}

void cia_write(cia_t *c, int reg, uint8_t v)
{
    switch (reg) {
    case 0x0: c->pra = v; if (c == &cia_a) ovl_update(); break;
    case 0x1: c->prb = v;
              if (c == &cia_b) drive_prb_write(port_in(c->prb, c->ddrb));
              break;
    case 0x2: c->ddra = v; if (c == &cia_a) ovl_update(); break;
    case 0x3: c->ddrb = v; break;
    case 0x4: c->ta_latch = (c->ta_latch & 0xFF00) | v; break;
    case 0x5: c->ta_latch = (uint16_t)(c->ta_latch & 0x00FF) | ((uint16_t)v << 8);
              if (!(c->cra & 1)) c->ta = c->ta_latch;         /* fermo: ricarica */
              if (c->cra & 8)  { c->ta = c->ta_latch; c->cra |= 1; } /* one-shot: parte */
              break;
    case 0x6: c->tb_latch = (c->tb_latch & 0xFF00) | v; break;
    case 0x7: c->tb_latch = (uint16_t)(c->tb_latch & 0x00FF) | ((uint16_t)v << 8);
              if (!(c->crb & 1)) c->tb = c->tb_latch;
              if (c->crb & 8)  { c->tb = c->tb_latch; c->crb |= 1; }
              break;
    case 0x8: if (c->crb & 0x80) c->tod_alarm = (c->tod_alarm & 0xFFFF00) | v;
              else { c->tod = (c->tod & 0xFFFF00) | v; c->tod_running = 1;
                     if (c->tod == c->tod_alarm) icr_set(c, 0x04); }
              break;
    case 0x9: if (c->crb & 0x80) c->tod_alarm = (c->tod_alarm & 0xFF00FF) | ((uint32_t)v << 8);
              else c->tod = (c->tod & 0xFF00FF) | ((uint32_t)v << 8);
              break;
    case 0xA: if (c->crb & 0x80) c->tod_alarm = (c->tod_alarm & 0x00FFFF) | ((uint32_t)v << 16);
              else { c->tod = (c->tod & 0x00FFFF) | ((uint32_t)v << 16); c->tod_running = 0; }
              break;
    case 0xC: c->sdr = v; break;
    case 0xD: if (v & 0x80) c->icr_mask |= (v & 0x1F); else c->icr_mask &= ~(v & 0x1F);
              break;
    case 0xE: if (c == &cia_a && (c->cra & 0x40) && !(v & 0x40)) solar_os_amiga_keyboard_ack();
              c->cra = v & 0xEF; if (v & 0x10) c->ta = c->ta_latch; break; /* force load */
    case 0xF: c->crb = v & 0xEF; if (v & 0x10) c->tb = c->tb_latch; break;
    }
}

static void timers_tick(cia_t *c, int e)
{
    if ((c->cra & 0x21) == 0x01) {  /* timer A attivo, clock E */
        int t = c->ta - e;
        while (t < 0) {
            icr_set(c, 0x01);
            if (c->cra & 8) { c->cra &= ~1; t = c->ta_latch; break; } /* one-shot */
            t += c->ta_latch + 1;
        }
        c->ta = (uint16_t)t;
    }
    if ((c->crb & 0x61) == 0x01) {  /* timer B attivo, clock E */
        int t = c->tb - e;
        while (t < 0) {
            icr_set(c, 0x02);
            if (c->crb & 8) { c->crb &= ~1; t = c->tb_latch; break; }
            t += c->tb_latch + 1;
        }
        c->tb = (uint16_t)t;
    }
}

void cia_tick(int eclocks)
{
    timers_tick(&cia_a, eclocks);
    timers_tick(&cia_b, eclocks);
    solar_os_amiga_keyboard_tick(eclocks);
}

bool solar_os_amiga_keyboard_receive(uint8_t serial)
{
    if (!(cia_a.icr_mask & 8) || (cia_a.cra & 0x40)) return false;
    cia_a.sdr = serial;
    icr_set(&cia_a, 8);
    return true;
}

/* chiamate dal main loop: TOD CIA-B avanza a ogni HSYNC, TOD CIA-A a 50Hz */
void cia_tod_hsync(void) { if (cia_b.tod_running) { cia_b.tod = (cia_b.tod + 1) & 0xFFFFFF;
                             if (cia_b.tod == cia_b.tod_alarm) icr_set(&cia_b, 0x04); } }
void cia_tod_vsync(void) { if (cia_a.tod_running) { cia_a.tod = (cia_a.tod + 1) & 0xFFFFFF;
                             if (cia_a.tod == cia_a.tod_alarm) icr_set(&cia_a, 0x04); } }
