// SPDX-License-Identifier: GPL-2.0-or-later
#include "port.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "g_game.h"
#include "i_video.h"
#include "i_system.h"
#include "lprintf.h"
#include "v_video.h"
#include "r_main.h"
#include "r_draw.h"
#include "w_wad.h"
#include "st_stuff.h"

int use_fullscreen, use_doublebuffer, desired_fullscreen;
int usejoystick, joyleft, joyright, joyup, joydown;
static bool held[512];
static bool tap_release[512];
static uint16_t palette[256];
static uint8_t color_map[256];
static uint8_t *frame;
static bool video_ready;
static bool exchanged_frames;
static uint64_t last_yield;

static void post(int key, bool down)
{
    if (key <= 0 || key >= sizeof(held) / sizeof(held[0]) || held[key] == down) return;
    held[key] = down;
    event_t event = { .type = down ? ev_keydown : ev_keyup, .data1 = key };
    D_PostEvent(&event);
}
void solar_os_doom_reset_keys(void)
{
    for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) if (held[i]) post(i, false);
    memset(tap_release, 0, sizeof(tap_release));
}
static int translate(const solar_os_native_foreground_key_v1_t *key)
{
    switch (key->usage) {
    case 0x28: return KEYD_ENTER;
    case 0x29: return KEYD_ESCAPE;
    case 0x2a: return KEYD_BACKSPACE;
    case 0x2b: return KEYD_TAB;
    case 0x4f: return KEYD_RIGHTARROW;
    case 0x50: return KEYD_LEFTARROW;
    case 0x51: return KEYD_DOWNARROW;
    case 0x52: return KEYD_UPARROW;
    case 0x48: return KEYD_PAUSE;
    case 0xe0: case 0xe4: return KEYD_RCTRL;
    case 0xe1: case 0xe5: return KEYD_RSHIFT;
    case 0xe2: case 0xe6: return KEYD_RALT;
    }
    if (key->usage >= 4 && key->usage <= 29) return 'a' + key->usage - 4;
    if (key->usage >= 0x1e && key->usage <= 0x26) return '1' + key->usage - 0x1e;
    if (key->usage == 0x27) return '0';
    /* Logical keys support input sources without HID usages. */
    switch (key->key) {
    case SOLAR_OS_KEY_UP: return KEYD_UPARROW;
    case SOLAR_OS_KEY_DOWN: return KEYD_DOWNARROW;
    case SOLAR_OS_KEY_LEFT: return KEYD_LEFTARROW;
    case SOLAR_OS_KEY_RIGHT: return KEYD_RIGHTARROW;
    case '\n': case '\r': return KEYD_ENTER;
    case 27: return KEYD_ESCAPE;
    case 127: return KEYD_BACKSPACE;
    }
    if (key->key >= 'A' && key->key <= 'Z') return key->key + 'a' - 'A';
    return key->key < 128 ? key->key : 0;
}
static void open_video(void)
{
    if (solar_os_doom_app->video_open_ex(SCREENWIDTH, SCREENHEIGHT, palette, 35, 0))
        I_Error("doom: foreground video unavailable\n");
    video_ready = true;
}
void solar_os_doom_poll(void)
{
    if (solar_os_doom_app->stop_requested()) solar_os_doom_exit(0);
    if (solar_os_doom_app->suspend_requested()) {
        solar_os_doom_reset_keys();
        solar_os_doom_sound_suspend();
        uint64_t start = solar_os_doom_app->time_us();
        if (solar_os_doom_app->suspend() || solar_os_doom_app->stop_requested()) solar_os_doom_exit(0);
        solar_os_doom_epoch += solar_os_doom_app->time_us() - start;
        solar_os_doom_reset_keys();
        solar_os_doom_sound_resume();
        if (video_ready) {
            if (frame && !exchanged_frames) solar_os_doom_app->present(frame, SCREENWIDTH * SCREENHEIGHT);
        }
    }
    uint64_t now = solar_os_doom_app->time_us();
    if (now - last_yield >= 5000) { solar_os_native_foreground_cooperate(solar_os_doom_app); last_yield = now; }
}
void I_StartTic(void)
{
    solar_os_doom_poll();
    for (unsigned i = 0; i < sizeof(tap_release) / sizeof(tap_release[0]); i++)
        if (tap_release[i]) { post(i, false); tap_release[i] = false; }
    for (unsigned i = 0; i < 64; i++) {
        solar_os_native_foreground_key_v1_t key; bool available;
        if (solar_os_doom_app->read_key(&key, &available) || !available) break;
        if (key.reset) { solar_os_doom_reset_keys(); continue; }
        bool down = key.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
        post(KEYD_RCTRL, (key.modifiers & 0x11) != 0);
        post(KEYD_RSHIFT, (key.modifiers & 0x22) != 0);
        post(KEYD_RALT, (key.modifiers & 0x44) != 0);
        int code = translate(&key);
        post(code, down);
        /* Text-only streams deliver taps without releases. */
        if (!key.usage && !key.physical_key && down && code > 0 && code < 512) tap_release[code] = true;
    }
}
void I_StartFrame(void) { solar_os_doom_poll(); }
void I_PreInitGraphics(void) {}
void I_ShutdownGraphics(void) {}
void I_UpdateNoBlit(void) {}
int I_StartDisplay(void) { solar_os_doom_poll(); return true; }
void I_EndDisplay(void) {}
int I_ScreenShot(const char *name) { return -1; }
void I_CalculateRes(unsigned int width, unsigned int height) {}
void I_SetPalette(int number)
{
    int lump = W_GetNumForName("PLAYPAL");
    if (number < 0 || (size_t)(number + 1) * 768 > W_LumpLength(lump)) I_Error("doom: invalid palette\n");
    const byte *data = (const byte *)W_CacheLumpNum(lump) + number * 768;
    for (unsigned i = 0; i < 256; i++)
        color_map[i] = (data[i*3] & 0xe0) | ((data[i*3+1] >> 3) & 0x1c) | (data[i*3+2] >> 6);
    W_UnlockLumpNum(lump);
    if (!video_ready) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned r = ((i >> 5) * 255 / 7) >> 3;
            unsigned g = (((i >> 2) & 7) * 255 / 7) >> 2;
            unsigned b = ((i & 3) * 255 / 3) >> 3;
            palette[i] = (r << 11) | (g << 5) | b;
        }
        open_video();
        exchanged_frames = solar_os_native_foreground_has_exchange(solar_os_doom_app);
        if (exchanged_frames) {
            size_t bytes;
            if (solar_os_doom_app->video_acquire(&frame, &bytes) || bytes != SCREENWIDTH * SCREENHEIGHT)
                I_Error("doom: no memory for frame\n");
        } else {
            frame = malloc(SCREENWIDTH * SCREENHEIGHT);
            if (!frame) I_Error("doom: no memory for frame\n");
        }
    }
}
void I_SetRes(void)
{
    for (unsigned i = 0; i < 5; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = i == 4 ? ST_SCALED_HEIGHT + 1 : SCREENHEIGHT;
        screens[i].byte_pitch = SCREENPITCH;
        screens[i].short_pitch = SCREENPITCH / 2;
        screens[i].int_pitch = SCREENPITCH / 4;
    }
}
void I_UpdateVideoMode(void)
{
    V_InitMode(VID_MODE8); V_DestroyUnusedTrueColorPalettes(); V_FreeScreens();
    I_SetRes(); V_AllocScreens(); R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
}
void I_InitGraphics(void) { I_UpdateVideoMode(); I_SetPalette(0); }
void I_FinishUpdate(void)
{
    solar_os_doom_poll();
    for (unsigned i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++) frame[i] = color_map[screens[0].data[i]];
    if (exchanged_frames ? solar_os_doom_app->video_exchange(&frame) :
        solar_os_doom_app->present(frame, SCREENWIDTH * SCREENHEIGHT))
        I_Error("doom: frame submission failed\n");
}
