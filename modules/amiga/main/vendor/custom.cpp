/* Registri custom 0xDFF000. Perimetro Fase 0:
 *   - INTENA/INTREQ/DMACON/ADKCON con semantica SET/CLR (bit 15)
 *   - VPOSR/VHPOSR dal beam counter (i loop di attesa raster del KS non si bloccano)
 *   - Calcolo IPL: livello interrupt piu alto tra i bit attivi (INTENA & INTREQ)
 *   - Tutti gli altri registri: latch + log delle prime scritture (diagnostica)
 * Copper, Blitter e bitplane arrivano allo step successivo — una cosa alla volta.
 */
#include "a500.h"
#include <stdio.h>
#include <string.h>
extern "C" {
#include "m68k.h"
}

uint16_t intena, intreq, dmacon, adkcon;
int vpos, hpos;
uint32_t bpl_pt[6];               /* puntatori bitplane live (incrementati dal fetch) */

static uint16_t regs[0x100];      /* latch generico, indice = off>>1 */
static int wlogged[0x100];        /* log solo la prima scrittura per registro */
static int cop_wlog = 0;          /* budget log scritture COP1LC */
static int jmp_wlog = 0;          /* budget log strobe COPJMP (separato) */
static int blit_log = 0;          /* budget log richieste blitter */
uint32_t blit_count = 0;          /* blit totali richiesti */
FILE *blitlogf = NULL;            /* log completo blit */
FILE *rdlogf = NULL;              /* log letture registri */
extern int copper_writing;

uint16_t custom_get(uint32_t off) { return regs[off >> 1]; }
void custom_set_latch(uint32_t off, uint16_t v) { regs[off >> 1] = v; }

/* nomi dei registri piu rilevanti per la diagnostica */
static const char *regname(uint32_t off)
{
    switch (off) {
    case 0x02E: return "COPCON";  case 0x080: return "COP1LCH";
    case 0x082: return "COP1LCL"; case 0x084: return "COP2LCH";
    case 0x086: return "COP2LCL"; case 0x088: return "COPJMP1";
    case 0x08E: return "DIWSTRT"; case 0x090: return "DIWSTOP";
    case 0x092: return "DDFSTRT"; case 0x094: return "DDFSTOP";
    case 0x096: return "DMACON";  case 0x09A: return "INTENA";
    case 0x09C: return "INTREQ";  case 0x09E: return "ADKCON";
    case 0x0E0: return "BPL1PTH"; case 0x0E2: return "BPL1PTL";
    case 0x100: return "BPLCON0"; case 0x102: return "BPLCON1";
    case 0x104: return "BPLCON2"; case 0x108: return "BPL1MOD";
    case 0x10A: return "BPL2MOD"; case 0x180: return "COLOR00";
    case 0x182: return "COLOR01"; case 0x040: return "BLTCON0";
    case 0x042: return "BLTCON1"; case 0x058: return "BLTSIZE";
    case 0x024: return "DSKLEN";  case 0x07E: return "DSKSYNC";
    }
    return NULL;
}

/* mappa bit INTREQ -> livello IPL 68000 (HRM tab. interrupt) */
static const uint8_t int_level[14] = { 1,1,1,2, 3,3,3,4, 4,4,4,5, 5,6 };

void irq_update(void)
{
    int lvl = 0;
    if (intena & 0x4000) {                       /* INTEN master */
        uint16_t act = intena & intreq & 0x3FFF;
        for (int b = 0; b < 14; b++)
            if (act & (1 << b) && int_level[b] > lvl) lvl = int_level[b];
    }
    m68k_set_irq(lvl);
}

void intreq_set(int bit) { intreq |= (uint16_t)(1 << bit); irq_update(); }

static uint16_t setclr(uint16_t cur, uint16_t v)
{
    return (v & 0x8000) ? (cur | (v & 0x7FFF)) : (cur & ~(v & 0x7FFF));
}

void custom_reset(void)
{
    sprite_reset();
    intena = intreq = dmacon = adkcon = 0;
    vpos = hpos = 0;
    memset(regs, 0, sizeof regs);
    memset(wlogged, 0, sizeof wlogged);
}

static uint16_t custom_read_inner(uint32_t off);

uint16_t custom_read(uint32_t off)
{
    uint16_t v = custom_read_inner(off);
    /* finestra di osservazione: escludo i registri int (rumore del dispatcher) */
    if (rdlogf && cur_frame >= 140 && cur_frame <= 180 &&
        off != 0x01C && off != 0x01E && off != 0x006 && off != 0x002)
        fprintf(rdlogf, "f=%d CUST R %03X = %04X (PC=%06X)\n",
                cur_frame, off, v, m68k_get_reg(NULL, M68K_REG_PPC));
    return v;
}

static uint16_t custom_read_inner(uint32_t off)
{
    switch (off) {
    case 0x002: return dmacon & 0x07FF;                 /* DMACONR (no BBUSY/BZERO in fase 0) */
    case 0x004: return 0x8000 | (uint16_t)((vpos >> 8) & 7); /* VPOSR: LOF=1 (PAL non
                   interlacciato = sempre long frame) | V10-V8. Il server VERTB di
                   graphics sceglie la copper list in base a LOF: senza questo bit
                   LoadView non installa mai la view (SHFCprList = NULL). */
    case 0x006: return (uint16_t)(((vpos & 0xFF) << 8) | (hpos & 0xFF)); /* VHPOSR */
    case 0x010: return adkcon & 0x7FFF;                 /* ADKCONR */
    case 0x01A: return 0x0000;                          /* DSKBYTR: nessun dato disco */
    case 0x01C: return intena & 0x7FFF;                 /* INTENAR */
    case 0x01E: return intreq & 0x7FFF;                 /* INTREQR */
    case 0x00A: return input_joy0dat();                 /* JOY0DAT: contatori mouse */
    case 0x00C: return input_joy1dat();                 /* JOY1DAT: joystick porta 1 */
    case 0x016: return input_rmb() ? 0xFB00 : 0xFF00;   /* POTGOR: bit10 = destro */
    case 0x018: return 0x0000;                          /* SERDATR */
    case 0x088: copper_jmp(1); return 0;                /* strobe anche in lettura (HW reale) */
    case 0x08A: copper_jmp(2); return 0;
    }
    return regs[off >> 1];
}

void custom_write(uint32_t off, uint16_t v)
{
    const char *nm = regname(off);
    if (nm && !wlogged[off >> 1]) {
        wlogged[off >> 1] = 1;
        if (copper_writing)
            logmsg("[CUST] prima scrittura %-8s = %04X (COPPER)\n", nm, v);
        else
            logmsg("[CUST] prima scrittura %-8s = %04X (PC=%06X)\n",
                   nm, v, m68k_get_reg(NULL, M68K_REG_PPC));
    }
    /* PAULA audio registers: AUD0..AUD3 (0x0A0..0x0DA).
       Senza questo hook, il 68000 scrive i registri audio ma Paula resta muta. */
    if (off >= 0x0A0 && off <= 0x0DA) {
        regs[off >> 1] = v;
        paula_write(off, v);
        return;
    }

    switch (off) {
    case 0x096:
        dmacon = setclr(dmacon, v) & 0x87FF;
        paula_set_dma(dmacon);   /* aggiorna master DMA + AUD0..AUD3 */
        return;
    case 0x09A: intena = setclr(intena, v); irq_update(); return;
    case 0x09C: intreq = setclr(intreq, v); irq_update(); return;
    case 0x09E: adkcon = setclr(adkcon, v); return;
    case 0x088: if (jmp_wlog < 4) { logmsg("[CL ] COPJMP1 strobe%s\n", copper_writing ? " (COPPER)" : ""); jmp_wlog++; }
                copper_jmp(1); return;
    case 0x08A: if (jmp_wlog < 4) { logmsg("[CL ] COPJMP2 strobe%s\n", copper_writing ? " (COPPER)" : ""); jmp_wlog++; }
                copper_jmp(2); return;
    case 0x024: {  /* DSKLEN: doppia scrittura con bit15 = avvio DMA disco */
        static int dsklog = 0;
        if (dsklog < 12) {
            logmsg("[DSK] DSKLEN = %04X%s (PC=%06X) DSKPT=%04X%04X SYNC=%04X ADKCON=%04X\n",
                   v, (v & 0x8000) ? " (DMA ON)" : "",
                   m68k_get_reg(NULL, M68K_REG_PPC),
                   regs[0x020 >> 1], regs[0x022 >> 1], regs[0x07E >> 1], adkcon);
            dsklog++;
        }
        regs[off >> 1] = v;
        disk_dsklen_write(v);
        return;
    }
    case 0x080: case 0x082: case 0x084: case 0x086:
        if (cop_wlog < 12) {
            logmsg("[CL ] COP%cLC%c = %04X %s\n",
                   (off & 4) ? '2' : '1', (off & 2) ? 'L' : 'H', v,
                   copper_writing ? "(COPPER)" : "");
            cop_wlog++;
        }
        break;   /* prosegue: latch in regs[] */
    case 0x058: {  /* BLTSIZE: avvia il blit -> dump della richiesta completa */
        blit_count++;
        if (blit_log < 12) {
            int w = v & 0x3F;  if (!w) w = 64;
            int h = (v >> 6) & 0x3FF; if (!h) h = 1024;
            logmsg("[BLT] #%u BLTSIZE=%04X (w=%d word, h=%d) CON0=%04X CON1=%04X %s\n",
                   blit_count, v, w, h, regs[0x040 >> 1], regs[0x042 >> 1],
                   copper_writing ? "(COPPER)" : "");
            logmsg("[BLT]    APT=%04X%04X BPT=%04X%04X CPT=%04X%04X DPT=%04X%04X\n",
                   regs[0x050 >> 1], regs[0x052 >> 1], regs[0x04C >> 1], regs[0x04E >> 1],
                   regs[0x048 >> 1], regs[0x04A >> 1], regs[0x054 >> 1], regs[0x056 >> 1]);
            logmsg("[BLT]    AMOD=%04X BMOD=%04X CMOD=%04X DMOD=%04X FWM=%04X LWM=%04X\n",
                   regs[0x064 >> 1], regs[0x062 >> 1], regs[0x060 >> 1], regs[0x066 >> 1],
                   regs[0x044 >> 1], regs[0x046 >> 1]);
            blit_log++;
        }
        if (blitlogf) {
            int lw = v & 0x3F;  if (!lw) lw = 64;
            int lh = (v >> 6) & 0x3FF; if (!lh) lh = 1024;
            fprintf(blitlogf,
                "f=%d n=%u sz=%04X w=%d h=%d c0=%04X c1=%04X "
                "apt=%04X%04X bpt=%04X%04X cpt=%04X%04X dpt=%04X%04X "
                "am=%04X bm=%04X cm=%04X dm=%04X fwm=%04X lwm=%04X "
                "ad=%04X bd=%04X cd=%04X%s\n",
                cur_frame, blit_count, v, lw, lh,
                regs[0x040 >> 1], regs[0x042 >> 1],
                regs[0x050 >> 1], regs[0x052 >> 1], regs[0x04C >> 1], regs[0x04E >> 1],
                regs[0x048 >> 1], regs[0x04A >> 1], regs[0x054 >> 1], regs[0x056 >> 1],
                regs[0x064 >> 1], regs[0x062 >> 1], regs[0x060 >> 1], regs[0x066 >> 1],
                regs[0x044 >> 1], regs[0x046 >> 1],
                regs[0x074 >> 1], regs[0x072 >> 1], regs[0x070 >> 1],
                copper_writing ? " COPPER" : "");
            fprintf(blitlogf, "   ^pc=%06X\n", m68k_get_reg(NULL, M68K_REG_PPC));
        }
        regs[off >> 1] = v;
        blitter_do(v);          /* esegue il blit alla scrittura di BLTSIZE */
        return;
    }
    }
    /* BPL1PTH..BPL6PTL (0x0E0-0x0F6): aggiornano i puntatori live */
    if (off >= 0x0E0 && off <= 0x0F6) {
        int p = (int)(off - 0x0E0) >> 2;
        if (off & 2) bpl_pt[p] = (bpl_pt[p] & 0xFFFF0000u) | v;
        else         bpl_pt[p] = (bpl_pt[p] & 0x0000FFFFu) | ((uint32_t)v << 16);
    }
    /* SPRxPTL (0x122,0x12A,...,0x13E): il write della parola bassa arma il canale */
    if (off==0x122||off==0x12A||off==0x132||off==0x13A||
        off==0x126||off==0x12E||off==0x136||off==0x13E) {
        regs[off >> 1] = v;
        sprite_arm((int)((off - 0x122) / 8));
        return;
    }
    regs[off >> 1] = v;
}
