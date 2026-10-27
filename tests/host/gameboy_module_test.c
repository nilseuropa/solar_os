#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t rom[32768], save[8192];
static uint64_t now;
static unsigned allocations, allocate_calls, fail_at, presents, suspends, opens, closes, writes,
    keys;
static unsigned scenario;
static bool audio, lock, save_exists, stop, hot_fail, save_fail;
static int exit_code;
static solar_os_native_foreground_audio_render_v1_t render;
static void *render_user;
static void *alloc(size_t bytes) {
    allocate_calls++;
    if (allocate_calls == fail_at)
        return NULL;
    void *p = malloc(bytes);
    assert(p);
    allocations++;
    return p;
}
static void *alloc_internal(size_t bytes) { return hot_fail ? NULL : alloc(bytes); }
static void release(void *p) {
    if (p) {
        assert(allocations);
        allocations--;
        free(p);
    }
}
static uint64_t time_us(void) {
    now += 100;
    return now;
}
static void sleep_ms(uint32_t ms) {
    now += (uint64_t)ms * 1000;
    if (audio) {
        int16_t samples[534] = {0};
        assert(!lock);
        lock = true;
        render(samples, 267, 16000, render_user);
        lock = false;
    }
}
static bool stop_requested(void) {
    if (scenario == 5)
        return now > 350000U;
    if (scenario == 6)
        return now > 450000U;
    if (scenario == 7)
        return now > 150000U;
    if (scenario == 8)
        return now > 300000U;
    if (scenario == 9)
        return now > 150000U;
    if (scenario == 10)
        return now > 300000U;
    return stop || now > 2200000ULL + 5000000ULL * suspends;
}
static bool suspend_requested(void) { return scenario == 1 && !suspends && now > 500000; }
static int suspend_app(void) {
    assert(!lock);
    audio = false;
    suspends++;
    now += 5000000;
    return 0;
}
static int raw_read_key(solar_os_native_foreground_key_v1_t *key, bool *available) {
    memset(key, 0, sizeof(*key));
    *available = false;
    if (scenario == 1 && now > 100000 && keys == 0) {
        key->key = 'p';
        keys++;
        *available = true;
    } else if (scenario == 1 && suspends && keys == 1) {
        key->key = 'p';
        keys++;
        *available = true;
    } else if (scenario == 2 && presents > 3 && keys == 0) {
        key->key = 'r';
        keys++;
        *available = true;
    } else if (scenario == 3 && now > 100000 && keys == 0) {
        key->key = 'q';
        keys++;
        *available = true;
    }
    if ((scenario == 5 || scenario == 6) && keys < 4) {
        const uint64_t times[4] = {100000, 150000, 200000, 400000};
        if (now > times[keys]) {
            key->key = 'z';
            key->usage = 0x1d;
            key->physical_key = 0x321;
            key->source = (keys % 2) + 1;
            key->action = keys < 2 ? 0 : 1;
            keys++;
            *available = true;
        }
    }
    /* Character-only keyboard sources send both halves of a synthetic tap in
     * one batch. Game Boy must keep its pulse active across emulated frames. */
    if ((scenario == 7 || scenario == 8) && keys < 2 && now > 100000U) {
        key->key = 'z';
        key->source = 1;
        key->action = keys == 0 ? SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS :
                                SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
        keys++;
        *available = true;
    }
    if ((scenario == 9 || scenario == 10) && keys < 2 && now > 100000U) {
        key->key = 0x83; /* Trackball Right from the board-button source. */
        key->source = 2;
        key->action = keys == 0 ? SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS :
                                SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
        keys++;
        *available = true;
    }
    return 0;
}
#ifdef TEST_NATIVE_KEY_PULSES
#include "native_key_pulse_fixture.h"
static bool native_key_raw_read(solar_os_native_foreground_key_v1_t *key) {
    bool available;
    assert(raw_read_key(key, &available) == 0);
    return available;
}
static void native_key_raw_reset(void) { assert(false); }
static uint64_t native_key_clock(void) { return now; }
#endif
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available) {
#ifdef TEST_NATIVE_KEY_PULSES
    return foreground_read_key(key, available);
#else
    return raw_read_key(key, available);
#endif
}
static int file_size(const char *path, uint32_t *size) {
    if (!strcmp(path, "/sdcard/cart.gb")) {
        *size = sizeof(rom);
        return 0;
    }
    if (!strcmp(path, "/sdcard/cart.sav") && save_exists) {
        *size = sizeof(save);
        return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int read_file(const char *path, uint32_t at, void *buffer, size_t length, size_t *read) {
    const uint8_t *data;
    size_t n;
    if (!strcmp(path, "/sdcard/cart.gb")) {
        data = rom;
        n = sizeof(rom);
    } else {
        data = save;
        n = sizeof(save);
    }
    assert(at + length <= n);
    memcpy(buffer, data + at, length);
    *read = length;
    return 0;
}
static int replace_file(const char *path, const void *buffer, size_t n) {
    assert(!strcmp(path, "/sdcard/cart.sav") && n == sizeof(save));
    writes++;
    if (save_fail)
        return SOLAR_OS_NATIVE_ERROR_FAILED;
    memcpy(save, buffer, n);
    save_exists = true;
    return 0;
}
static int resolve(const char *path, char *out, size_t n) {
    assert(!strcmp(path, "cart.gb") && n >= 16);
    strcpy(out, "/sdcard/cart.gb");
    return 0;
}
static int video(uint16_t w, uint16_t h, uint16_t fps) {
    assert(w == 160 && h == 144 && fps == 25);
    return 0;
}
static int present(const uint8_t *pixels, size_t n) {
    assert(pixels && n == 5760);
    presents++;
    return 0;
}
static int synth_open(solar_os_native_foreground_audio_render_v1_t cb, void *user, size_t frames,
                      uint32_t rate) {
    assert(cb && frames == 267 && rate == 16000);
    opens++;
    render = cb;
    render_user = user;
    if (scenario == 4)
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    audio = true;
    return 0;
}
static bool synth_lock(void) {
    assert(!lock);
    lock = true;
    return true;
}
static void synth_unlock(void) {
    assert(lock);
    lock = false;
}
static void synth_close(void) {
    assert(!lock);
    audio = false;
    render = NULL;
    render_user = NULL;
    closes++;
}
static int rtc(uint8_t registers[5]) {
    memset(registers, 0, 5);
    return 0;
}
static uint8_t buttons(void) { return scenario == 9 || scenario == 10 ? 2 : 0; }
static int video_stats(solar_os_native_foreground_video_stats_v1_t *stats) {
    memset(stats, 0, sizeof(*stats));
    return 0;
}
static void set_exit(int code) { exit_code = code; }
static int write_utf8(const char *text, size_t n) {
    assert(text && n);
    return 0;
}
static const solar_os_native_memory_api_v1_t memory = {.abi_version = 1,
                                                       .struct_size = sizeof(memory),
                                                       .alloc = alloc,
                                                       .free = release,
                                                       .alloc_internal = alloc_internal};
static const solar_os_native_foreground_api_v1_t app = {.abi_version = 1,
                                                        .struct_size = sizeof(app),
                                                        .time_us = time_us,
                                                        .sleep_ms = sleep_ms,
                                                        .stop_requested = stop_requested,
                                                        .suspend_requested = suspend_requested,
                                                        .suspend = suspend_app,
                                                        .read_key = read_key,
                                                        .file_size = file_size,
                                                        .read_file = read_file,
                                                        .replace_file = replace_file,
                                                        .resolve_path = resolve,
                                                        .video_open_index2 = video,
                                                        .present = present,
                                                        .synth_open = synth_open,
                                                        .synth_lock = synth_lock,
                                                        .synth_unlock = synth_unlock,
                                                        .synth_close = synth_close,
                                                        .rtc_read = rtc,
                                                        .buttons_source = buttons,
                                                        .video_stats = video_stats,
                                                        .set_exit_code = set_exit};
static const void *get_service(const char *name, uint32_t abi, uint32_t size) {
    assert(abi == 1);
    if (!strcmp(name, "foreground")) {
        assert(size == offsetof(solar_os_native_foreground_api_v1_t, video_stats) + sizeof(app.video_stats));
        return &app;
    }
    assert(!strcmp(name, "memory") && size == sizeof(memory));
    return &memory;
}
static const solar_os_native_host_api_v1_t host = {.abi_version = 1,
                                                   .struct_size = sizeof(host),
                                                   .write_utf8 = write_utf8,
                                                   .get_service = get_service};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }
extern int gameboy_module_main(int argc, char **argv);
static void setup(unsigned mode) {
#ifdef TEST_NATIVE_KEY_PULSES
    native_key_pulse_reset();
#endif
    assert(!allocations);
    now = allocate_calls = presents = suspends = opens = closes = writes = keys = 0;
    scenario = mode;
    fail_at = 0;
    stop = hot_fail = save_fail = audio = lock = save_exists = false;
    exit_code = -1;
    memset(rom, 0, sizeof(rom));
    memset(save, 0, sizeof(save));
    /* Original test program: enable cartridge RAM, write 0x42, then loop. */
    const uint8_t code[] = {0x3e, 0x0a, 0xea, 0x00, 0x00, 0x3e, 0x42, 0xea, 0x00, 0xa0, 0x18, 0xfe};
    rom[0x100] = 0xc3;
    rom[0x101] = 0x50;
    rom[0x102] = 0x01;
    memcpy(rom + 0x150, code, sizeof(code));
    rom[0x147] = 3;
    rom[0x149] = 2;
    uint8_t checksum = 0;
    for (unsigned i = 0x134; i <= 0x14c; i++)
        checksum = (uint8_t)(checksum - rom[i] - 1);
    rom[0x14d] = checksum;
}
static int run(void) {
    char *argv[] = {"gameboy", "cart.gb"};
    int result = gameboy_module_main(2, argv);
    assert(!allocations && !audio && !render && !lock);
    return result;
}
int main(void) {
    setup(0);
    assert(!run() && exit_code == 0 && presents > 10 && writes == 1 && save[0] == 0x42);
    unsigned count = allocate_calls;
    setup(0);
    save_exists = true;
    save[0] = 0x42;
    assert(!run() && !writes && save[0] == 0x42);
    setup(1);
    assert(!run() && suspends == 1 && opens == 2 && writes == 1 && save[0] == 0x42);
    setup(2);
    assert(!run() && keys == 1 && writes == 1 && save[0] == 0x42);
    setup(3);
    assert(!run() && keys == 1 && presents > 0);
    const uint8_t joypad[] = {0x3e, 0x10, 0xe0, 0x00, 0xf0, 0x00, 0x2f,
                              0xe6, 0x01, 0xea, 0x01, 0xa0, 0x18, 0xf2};
    setup(5);
    memcpy(rom + 0x15a, joypad, sizeof(joypad));
    assert(!run() && keys == 3 && save[1] == 1);
    setup(6);
    memcpy(rom + 0x15a, joypad, sizeof(joypad));
    assert(!run() && keys == 4 && save[1] == 0);
    setup(7);
    memcpy(rom + 0x15a, joypad, sizeof(joypad));
    assert(!run() && keys == 2 && save[1] == 1); /* Tap reaches the real guest joypad. */
    setup(8);
    memcpy(rom + 0x15a, joypad, sizeof(joypad));
    assert(!run() && keys == 2 && save[1] == 0); /* Pulse expires without a stuck button. */
#ifdef TEST_NATIVE_KEY_PULSES
    setup(9);
    memcpy(rom + 0x15a, joypad, sizeof(joypad)); rom[0x15b] = 0x20;
    assert(!run() && keys == 2 && save[1] == 1); /* Board arrow visible in a guest frame. */
    setup(10);
    memcpy(rom + 0x15a, joypad, sizeof(joypad)); rom[0x15b] = 0x20;
    assert(!run() && keys == 2 && save[1] == 0); /* Shared host pulse releases the arrow. */
#endif
    setup(4);
    assert(!run() && opens == 1 && presents > 10 && writes == 1);
    setup(0);
    hot_fail = true;
    assert(!run() && presents > 10);
    for (unsigned i = 1; i <= count; i++) {
        setup(0);
        fail_at = i;
        int result = run();
        assert(result == 0 || result == 1);
    }
    setup(0);
    rom[0x14d] ^= 1;
    assert(run() == 1 && !presents && !writes);
    setup(0);
    rom[0x150] = 0xd3;
    assert(run() == 1 && !writes);
    setup(0);
    save_fail = true;
    assert(run() == 1 && exit_code == 1 && writes == 1);
    puts("Game Boy module: real core, audio, resume, reset, save and allocation cleanup OK");
    return 0;
}
