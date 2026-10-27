/* DF0 minimale — Fase 0: drive presente, NESSUN disco inserito.
 * Ground truth (HRM + MAME):
 *   CIA-B PRB (output, attivi bassi):
 *     bit0 /STEP  bit1 /DIR  bit2 /SIDE  bit3 /SEL0  bit7 /MTR
 *     il motore è latchato nel drive sul fronte di discesa di /SEL0.
 *   CIA-A PRA (input, validi solo a drive selezionato):
 *     bit2 /CHNG  basso = latch "disk changed/nessun disco"; senza disco
 *                 resta basso anche dopo impulsi di step (il latch si
 *                 azzera solo steppando CON un disco inserito)
 *     bit3 /WPRO  bit4 /TK0 (basso a traccia 0)  bit5 /RDY (alto = non pronto)
 *   Drive non selezionato: linee in pull-up (tutte alte) — identico a prima,
 *   quindi la testrom sintetica non cambia comportamento (regressione zero).
 */
#include "a500.h"
#include <stdio.h>

#define ADF_SIZE 901120

static int sel = 0;        /* DF0 selezionato (/SEL0 basso)    */
static int motor = 0;      /* stato motore latchato            */
static int track = 0;      /* posizione testina 0..79          */
static int side = 0;       /* /SIDE da CIA-B PRB bit2 (0=faccia inferiore) */
static uint8_t prb_prev = 0xFF;
static int evlog = 0;      /* log limitato dei primi eventi    */

#ifdef SOLAR_OS_AMIGA
static uint8_t *adf = nullptr;   // 901KB in PSRAM (allocato da drive_set_adf)
#else
static uint8_t adf[ADF_SIZE];
#endif
static int disk_present = 0;
static int chng_latch = 1; /* asserito (/CHNG basso) finche' uno step
                              a disco INSERITO non lo azzera (HRM) */

#ifdef SOLAR_OS_AMIGA
#include "port.h"
#include <cstring>
// monta un ADF gia' in memoria (es. decompresso da header). Alloca in PSRAM.
int drive_set_adf(const uint8_t *data, uint32_t len)
{
    if (len != ADF_SIZE) { logmsg("[DRV] ERRORE: ADF %u != 901120\n", (unsigned)len); return -1; }
    if (!adf) adf = (uint8_t*) heap_caps_malloc(ADF_SIZE, MALLOC_CAP_SPIRAM);
    if (!adf) { logmsg("[DRV] ERRORE: alloc ADF PSRAM fallita\n"); return -1; }
    memcpy(adf, data, ADF_SIZE);
    disk_present = 1;
    chng_latch = 1;
    logmsg("[DRV] disco montato in PSRAM (901120 byte, write-protected)\n");
    return 0;
}

// alloca il buffer ADF e ne ritorna il puntatore, per decomprimerci DIRETTAMENTE
// (niente doppio buffer). Dopo la decompressione, chiamare drive_mount_ready().
uint8_t *drive_alloc_adf(void)
{
    if (!adf) adf = (uint8_t*) heap_caps_malloc(ADF_SIZE, MALLOC_CAP_SPIRAM);
    return adf;
}
void drive_mount_ready(void)
{
    disk_present = 1;
    chng_latch = 1;
    logmsg("[DRV] disco montato in PSRAM (901120 byte, write-protected)\n");
}
#endif

#ifndef SOLAR_OS_AMIGA
int drive_insert_adf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { logmsg("[DRV] ERRORE: impossibile aprire %s\n", path); return -1; }
    size_t n = fread(adf, 1, ADF_SIZE, f);
    fclose(f);
    if (n != ADF_SIZE) {
        logmsg("[DRV] ERRORE: dimensione ADF %zu != 901120\n", n);
        return -1;
    }
    disk_present = 1;
    chng_latch = 1;
    logmsg("[DRV] disco inserito: %s (901120 byte, write-protected)\n", path);
    return 0;
}
#endif

int  drive_track(void)    { return track; }
int  drive_side(void)     { return side; }
int  drive_motor_on(void) { return motor && disk_present; }
const uint8_t *drive_adf(void) { return disk_present ? adf : NULL; }

#define DRVLOG(...) do { if (evlog < 200) { logmsg(__VA_ARGS__); evlog++; } } while (0)

void drive_prb_write(uint8_t prb)
{
    int sel_now = !(prb & 0x08);              /* /SEL0 attivo basso */

    if (sel_now && !sel) {                    /* fronte di selezione: latch motore */
        int m = !(prb & 0x80);                /* /MTR attivo basso */
        if (m != motor)
            logmsg("[DRV] motore %s\n", m ? "ON" : "OFF");   /* sempre loggato */
        motor = m;
    }

    side = !(prb & 0x04);                     /* /SIDE attivo basso: 1 = faccia sup. */

    /* impulso di step: fronte di discesa di /STEP a drive selezionato */
    if (sel_now && (prb_prev & 0x01) && !(prb & 0x01)) {
        if (prb & 0x02) { if (track > 0) track--; }   /* /DIR alto: verso traccia 0 */
        else            { if (track < 79) track++; }  /* /DIR basso: verso il centro */
        if (disk_present && chng_latch) {
            chng_latch = 0;                   /* step a disco inserito: latch azzerato */
            logmsg("[DRV] step -> traccia %d: DISCO RILEVATO (/CHNG si alza)\n", track);
        } else {
            DRVLOG("[DRV] step -> traccia %d (%s)\n", track,
                   disk_present ? "disco presente" : "nessun disco: /CHNG resta basso");
        }
    }

    sel = sel_now;
    prb_prev = prb;
}

int drive_selected(void) { return sel; }

/* bit 2-5 di CIA-A PRA visti dal 68000 (maschera 0x3C) */
uint8_t drive_pra_bits(void)
{
    if (!sel) return 0x3C;                    /* non selezionato: pull-up */
    uint8_t v = 0;
    if (!chng_latch) v |= 0x04;               /* /CHNG alto = latch azzerato (disco ok) */
    if (!(disk_present))
        v |= 0x08;                            /* /WPRO alto solo senza disco; l'ADF
                                                 inserito e' write-protected (basso) */
    if (track != 0) v |= 0x10;                /* /TK0 basso solo a traccia 0 */
    if (!(motor && disk_present))
        v |= 0x20;                            /* /RDY basso = pronto (motore+disco) */
    return v;
}

void solar_os_amiga_drive_reset(void) { adf = nullptr; sel = motor = track = side = evlog = disk_present = 0; prb_prev = 0xff; chng_latch = 1; }
