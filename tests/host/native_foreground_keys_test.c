#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "native_key_pulse_fixture.h"

static solar_os_native_foreground_key_v1_t queue[128];
static size_t count;
static uint64_t now;
static bool native_key_raw_read(solar_os_native_foreground_key_v1_t *key)
{
    if (!count) return false;
    *key = queue[0];
    memmove(queue, queue + 1, --count * sizeof(*queue));
    return true;
}
static void native_key_raw_reset(void) { count = 0; }
static uint64_t native_key_clock(void) { return now; }
static void push(uint8_t source, uint8_t key, uint8_t action, uint16_t physical, uint16_t usage)
{
    assert(count < 128);
    queue[count++] = (solar_os_native_foreground_key_v1_t){
        .source = source, .key = key, .action = action, .physical_key = physical, .usage = usage};
}
static solar_os_native_foreground_key_v1_t read_key(bool expected)
{
    solar_os_native_foreground_key_v1_t key;
    bool available;
    assert(foreground_read_key(&key, &available) == 0 && available == expected);
    return key;
}
static void tap(uint8_t source, uint8_t key)
{
    push(source, key, 0, 0, 0);
    push(source, key, 1, 0, 0);
}

#ifdef TEST_DOOM_INPUT
#include "d_event.h"
#include "doom_keydefs.inc"
#include "solar_os_key_codes.h"
static bool held[512], tap_release[512];
static unsigned downs[512], ups[512];
static void D_PostEvent(event_t *event)
{
    assert(event->data1 > 0 && event->data1 < 512);
    if (event->type == ev_keydown) downs[event->data1]++;
    else { assert(event->type == ev_keyup); ups[event->data1]++; }
}
static void solar_os_doom_poll(void) {}
static const solar_os_native_foreground_api_v1_t doom_api = {.read_key = foreground_read_key};
static const solar_os_native_foreground_api_v1_t *solar_os_doom_app = &doom_api;
#include "doom_input.inc"
static void test_doom_tics(void)
{
    native_key_pulse_reset();
    solar_os_doom_reset_keys();
    const int codes[] = {KEYD_UPARROW, KEYD_DOWNARROW, KEYD_LEFTARROW, KEYD_RIGHTARROW};
    for (unsigned i = 0; i < 4; i++) {
        tap(2, SOLAR_OS_KEY_UP + i);
        I_StartTic();
        assert(held[codes[i]] && downs[codes[i]] == 1); /* Guest advances with arrow down. */
        I_StartTic();
        assert(!held[codes[i]] && ups[codes[i]] == 1);
        now += 140000;
        I_StartTic();
        assert(downs[codes[i]] == 1 && ups[codes[i]] == 1);
    }
    push(1, 'z', 0, 12, 0x1d);
    I_StartTic(); I_StartTic(); assert(held['z']);
    push(1, 'z', 1, 12, 0x1d);
    I_StartTic(); assert(!held['z']);
    puts("DOOM input sees trackball arrows for a game tic and preserves physical holds");
}
#endif
int main(void)
{
    native_key_pulse_reset();
    now = 1000000;
    /* A board-navigation tap that has sat in the queue still spans guest time. */
    tap(2, 0x83);
    now += 1000000;
    solar_os_native_foreground_key_v1_t key = read_key(true);
    assert(key.source == 2 && key.key == 0x83 && key.action == 0);
    read_key(false);
    now += 139999; read_key(false);
    now++; key = read_key(true);
    assert(key.source == 2 && key.key == 0x83 && key.action == 1 && key.modifiers == 0);
    read_key(false);

    /* Real held-key and HID streams are delivered without synthetic delay. */
    push(1, 'z', 0, 12, 0x1d); push(1, 'z', 1, 12, 0x1d);
    key = read_key(true); assert(key.physical_key == 12 && key.action == 0);
    key = read_key(true); assert(key.physical_key == 12 && key.action == 1);
    read_key(false);

    /* Pulses of the same key from different sources cannot release each other. */
    tap(1, 'x'); tap(2, 'x');
    key = read_key(true); assert(key.source == 1);
    key = read_key(true); assert(key.source == 2);
    read_key(false);
    now += 100000;
    tap(1, 'x'); key = read_key(true); assert(key.source == 1 && key.action == 0);
    read_key(false);
    now += 40000;
    key = read_key(true); assert(key.source == 2 && key.action == 1);
    read_key(false);
    now += 100000;
    key = read_key(true); assert(key.source == 1 && key.action == 1);
    read_key(false);

    tap(2, 0x80); read_key(true); read_key(false);
    atomic_store(&key_pulse_state.input_reset, true);
    key = read_key(true); assert(key.reset);
    now += 200000; read_key(false); /* No stale key-up after suspend/reset. */

    for (unsigned i = 0; i < FOREGROUND_KEY_PULSE_MAX + 1; i++) tap(1, 32 + i);
    for (unsigned i = 0; i < FOREGROUND_KEY_PULSE_MAX; i++) {
        key = read_key(true); assert(!key.reset);
    }
    key = read_key(true); assert(key.reset && count == 0);
    now += 200000; read_key(false);
    tap(3, 'a'); key = read_key(true); assert(!key.reset && key.key == 'a');
    read_key(false);
    now += 140000; key = read_key(true); assert(key.action == 1);
    read_key(false);
#ifdef TEST_DOOM_INPUT
    test_doom_tics();
#endif
    puts("Native logical taps span guest time; held keys, source isolation and resets passed");
    return 0;
}
