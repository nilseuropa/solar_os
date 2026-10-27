/* Mouse porta 0: contatori liberi 8 bit (JOY0DAT: Y nei bit 15-8, X nei 7-0),
 * tasto sinistro su CIA-A PRA bit 6 (attivo basso), destro su POTGOR bit 10.
 * Gli eventi arrivano da uno script (--mousescript FILE), una riga per evento:
 *   FRAME move DX DY NFRAMES   movimento totale (DX,DY) spalmato su NFRAMES
 *   FRAME lmb 0|1              tasto sinistro giu'/su
 *   FRAME rmb 0|1              tasto destro
 *   # commento
 * input.device legge i contatori a ogni vblank e calcola i delta (8 bit con
 * segno): il movimento spalmato resta sotto il limite di +-127 per frame.
 */
#include "a500.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t mx, my;               /* contatori liberi */
static int lmb = 0, rmb = 0;

/* ===== JOYSTICK DIGITALE porta 1 (giochi) =====
 * Stato logico: 5 bool (su/giu/sx/dx/fuoco). Convertito nel formato
 * JOY1DAT che i giochi leggono. Fuoco su CIA-A PRA bit 7 (attivo basso). */
static int joy_up = 0, joy_down = 0, joy_left = 0, joy_right = 0, joy_fire = 0;

void input_set_joy(int up, int down, int left, int right, int fire)
{
    joy_up = up; joy_down = down; joy_left = left; joy_right = right; joy_fire = fire;
}

/* JOY1DAT: encoding Amiga. Destra=bit1, Sinistra=bit9,
 * Giu'=bit1 XOR bit0, Su'=bit9 XOR bit8. Ricostruisco i bit grezzi. */
uint16_t input_joy1dat(void)
{
    int x1 = joy_right ? 1 : 0;
    int x9 = joy_left  ? 1 : 0;
    int x0 = x1 ^ (joy_down ? 1 : 0);   /* bit0 = bit1 XOR giu' */
    int x8 = x9 ^ (joy_up   ? 1 : 0);   /* bit8 = bit9 XOR su'  */
    return (uint16_t)((x9 << 9) | (x8 << 8) | (x1 << 1) | x0);
}

int input_joy_fire(void) { return joy_fire; }   /* 1 = premuto */


#define MAXEV 64
typedef struct { int f, type, a, b, n; } ev_t;   /* type: 0=move 1=lmb 2=rmb */
static ev_t evs[MAXEV];
static int nev = 0;

/* movimento in corso: distribuzione bresenham-like dei delta */
static int mv_n = 0, mv_dx, mv_dy, mv_ax, mv_ay, mv_tot;

#ifndef SOLAR_OS_AMIGA
int input_load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { logmsg("[MOU] ERRORE: impossibile aprire %s\n", path); return -1; }
    char line[128];
    while (fgets(line, sizeof line, f) && nev < MAXEV) {
        char cmd[16];
        int fr, a = 0, b = 0, n = 1;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "%d %15s %d %d %d", &fr, cmd, &a, &b, &n) < 2) continue;
        ev_t *e = &evs[nev];
        e->f = fr; e->a = a; e->b = b; e->n = (n > 0) ? n : 1;
        if      (!strcmp(cmd, "move")) e->type = 0;
        else if (!strcmp(cmd, "lmb"))  e->type = 1;
        else if (!strcmp(cmd, "rmb"))  e->type = 2;
        else if (!strcmp(cmd, "joy"))  e->type = 3;   /* a=bitmask UDLR+fire */
        else continue;
        nev++;
    }
    fclose(f);
    logmsg("[MOU] script caricato: %d eventi\n", nev);
    return 0;
}
#endif

void input_frame(int frame)
{
    for (int i = 0; i < nev; i++) {
        if (evs[i].f != frame) continue;
        switch (evs[i].type) {
        case 0:
            mv_dx = evs[i].a; mv_dy = evs[i].b;
            mv_tot = mv_n = evs[i].n; mv_ax = mv_ay = 0;
            logmsg("[MOU] f=%d move %+d,%+d in %d frame\n",
                   frame, mv_dx, mv_dy, mv_tot);
            break;
        case 1:
            lmb = evs[i].a;
            logmsg("[MOU] f=%d tasto sinistro %s\n", frame, lmb ? "GIU'" : "SU");
            break;
        case 2:
            rmb = evs[i].a;
            logmsg("[MOU] f=%d tasto destro %s\n", frame, rmb ? "GIU'" : "SU");
            break;
        case 3: {   /* joystick: bitmask a = U(1) D(2) L(4) R(8) FIRE(16) */
            int m = evs[i].a;
            input_set_joy(m&1, (m>>1)&1, (m>>2)&1, (m>>3)&1, (m>>4)&1);
            logmsg("[JOY] f=%d joy=%c%c%c%c fire=%d\n", frame,
                   (m&1)?'U':'-',(m&2)?'D':'-',(m&4)?'L':'-',(m&8)?'R':'-',(m>>4)&1);
            break;
        }
        }
    }
    if (mv_n > 0) {
        mv_ax += mv_dx; mv_ay += mv_dy;
        int sx = mv_ax / mv_tot; mv_ax -= sx * mv_tot;
        int sy = mv_ay / mv_tot; mv_ay -= sy * mv_tot;
        mx = (uint8_t)(mx + sx);
        my = (uint8_t)(my + sy);
        mv_n--;
    }
}

uint16_t input_joy0dat(void) { return (uint16_t)(my << 8 | mx); }
int input_lmb(void) { return lmb; }
void input_set_lmb(int v) { lmb = v; }
void input_set_rmb(int v) { rmb = v; }
int input_rmb(void) { return rmb; }

void solar_os_amiga_input_reset(void) { joy_up = joy_down = joy_left = joy_right = joy_fire = lmb = rmb = 0; mv_n = nev = 0; }
void solar_os_amiga_mouse_move(int dx, int dy) { mx = (uint8_t)(mx + dx); my = (uint8_t)(my + dy); }
