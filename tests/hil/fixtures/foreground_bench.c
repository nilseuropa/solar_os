// SPDX-License-Identifier: MIT
#include <stddef.h>
#include <stdint.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include "solar_os_native_service_abi.h"

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_POINTER_EVENTS,
    .worker_stack_bytes = 8192, .worker_priority = 1,
    .minimum_internal_bytes = 4096, .minimum_psram_bytes = 512 * 1024,
    .tick_interval_ms = 5, .id = "fgbench", .summary = "Foreground runtime benchmark",
};

static const solar_os_native_foreground_api_v1_t *app;
static char result[2048];
static unsigned used;
static volatile uint32_t checksum;

/* A code working set larger than the small instruction-cache configuration.
 * Each handler has distinct constants and identical arithmetic cost. */
#define MIX(k) do { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x += (k); } while (0)
#define HANDLER(b,n) static __attribute__((noinline)) uint32_t op_##b##_##n(uint32_t x) \
    { MIX((b * 16 + n + 1) * 0x9e3779b9U); MIX(n + 17U); MIX(b + 31U); MIX(b * 16 + n + 73U); return x; }
#define ROW(b) HANDLER(b,0) HANDLER(b,1) HANDLER(b,2) HANDLER(b,3) \
    HANDLER(b,4) HANDLER(b,5) HANDLER(b,6) HANDLER(b,7) HANDLER(b,8) HANDLER(b,9) \
    HANDLER(b,10) HANDLER(b,11) HANDLER(b,12) HANDLER(b,13) HANDLER(b,14) HANDLER(b,15)
ROW(0) ROW(1) ROW(2) ROW(3) ROW(4) ROW(5) ROW(6) ROW(7)
ROW(8) ROW(9) ROW(10) ROW(11) ROW(12) ROW(13) ROW(14) ROW(15)
#define ENTRY(b,n) op_##b##_##n,
#define ENTRIES(b) ENTRY(b,0) ENTRY(b,1) ENTRY(b,2) ENTRY(b,3) ENTRY(b,4) ENTRY(b,5) \
    ENTRY(b,6) ENTRY(b,7) ENTRY(b,8) ENTRY(b,9) ENTRY(b,10) ENTRY(b,11) ENTRY(b,12) ENTRY(b,13) ENTRY(b,14) ENTRY(b,15)
static uint32_t (*const handlers[256])(uint32_t) = {
    ENTRIES(0) ENTRIES(1) ENTRIES(2) ENTRIES(3) ENTRIES(4) ENTRIES(5) ENTRIES(6) ENTRIES(7)
    ENTRIES(8) ENTRIES(9) ENTRIES(10) ENTRIES(11) ENTRIES(12) ENTRIES(13) ENTRIES(14) ENTRIES(15)
};
#undef MIX
#undef HANDLER
#undef ROW
#undef ENTRY
#undef ENTRIES

static void text(const char *s) { while (*s && used < sizeof(result)) result[used++] = *s++; }
static void number(uint64_t n)
{
    char digits[24]; unsigned count = 0;
    do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
    while (count && used < sizeof(result)) result[used++] = digits[--count];
}
static void record(const char *name, unsigned count, uint64_t work, uint64_t wall)
{
    text(name); text(" "); number(count); text(" "); number(work); text(" "); number(wall); text("\n");
}
static void silence(int16_t *out, size_t frames, uint32_t rate, void *user)
{
    (void)rate; (void)user;
    for (size_t i = 0; i < frames * 2; ++i) out[i] = 0;
}
static void calls(unsigned which, const char *name)
{
    uint64_t wall = app->time_us(), work = 0;
    unsigned count = 0;
    for (unsigned batch = 0; batch < 50; ++batch) {
        uint64_t start = app->time_us();
        for (unsigned i = 0; i < 1000; ++i) {
            solar_os_native_foreground_key_v1_t key;
            solar_os_native_foreground_pointer_v1_t pointer;
            bool available;
            switch (which) {
            case 0: checksum = app->time_us(); break;
            case 1: checksum = app->stop_requested(); break;
            case 2: checksum = app->read_key(&key, &available); break;
            case 3: checksum = app->read_pointer(&pointer, &available); break;
            }
        }
        work += app->time_us() - start; count += 1000;
        if (app->stop_requested()) break;
        app->sleep_ms(1);
    }
    record(name, count, work, app->time_us() - wall);
}
static uint8_t *frames(uint8_t *pixels, size_t size, unsigned pace, const char *name, bool exchange)
{
    solar_os_native_foreground_video_stats_v1_t stats;
    app->video_stats(&stats);
    uint64_t wall = app->time_us(), work = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < 60 && !app->stop_requested(); ++i) {
        /* Complete frames also exercise replacement while the presenter reads. */
        for (size_t k = 0; k < size; ++k) pixels[k] = (k + i) & 255;
        uint64_t start = app->time_us();
        if (exchange ? app->video_exchange(&pixels) : app->present(pixels, size)) break;
        work += app->time_us() - start; ++count;
        app->sleep_ms(pace);
    }
    record(name, count, work, app->time_us() - wall);
    app->video_stats(&stats);
    record("presented", stats.frames, stats.present_us, stats.replaced_frames);
    return pixels;
}

int main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    if (!host || argc != 2) return 1;
    app = host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, calendar_read));
    const solar_os_native_memory_api_v1_t *memory = host->get_service("memory", 1,
        offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    if (!app || !memory) return 1;
    uint16_t width, height;
    if (app->video_dimensions(&width, &height)) return 1;
    used = 0; text("FGBENCH1\nsize "); number(width); text(" "); number(height); text("\n");
    calls(0, "time"); calls(1, "stop"); calls(2, "keys"); calls(3, "pointer");
    uint64_t wall = app->time_us();
    uint32_t x = 0x12345678;
    for (unsigned batch = 0; batch < 50; ++batch) {
        for (unsigned i = 0; i < 10000; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
        checksum = x;
    }
    record("kernel", 500000, app->time_us() - wall, x);
    uint64_t dispatch_work = 0;
    unsigned dispatch_count = 0;
    x = 0x12345678;
    for (unsigned batch = 0; batch < 50 && !app->stop_requested(); ++batch) {
        uint64_t start = app->time_us();
        for (unsigned i = 0; i < 1000; ++i) x = handlers[x >> 24](x);
        dispatch_work += app->time_us() - start;
        dispatch_count += 1000;
        app->sleep_ms(1);
    }
    checksum = x;
    record("dispatch", dispatch_count, dispatch_work, x);
    uint32_t *words = memory->alloc(128 * 1024);
    if (!words) return 1;
    x = 0x12345678;
    for (unsigned i = 0; i < 32768; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; words[i] = x;
    }
    const char *ram_names[] = {"ram8k", "ram32k", "ram128k"};
    for (unsigned test = 0, count = 2048; test < 3; ++test, count *= 4) {
        uint64_t work = 0; unsigned iterations = 0;
        x = 0x12345678;
        for (unsigned batch = 0; batch < 50 && !app->stop_requested(); ++batch) {
            uint64_t start = app->time_us();
            for (unsigned i = 0; i < 1000; ++i) x = words[x & (count - 1)] + (x ^ (x >> 7));
            work += app->time_us() - start; iterations += 1000;
            app->sleep_ms(1);
        }
        checksum = x; record(ram_names[test], iterations, work, x);
    }
    memory->free(words);
    size_t size = (size_t)width * height;
    uint8_t *pixels = memory->alloc(size);
    if (!pixels) return 1;
    for (size_t i = 0; i < size; ++i) pixels[i] = (i / width / 16) & 255;
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i) palette[i] = i * 257U;
    if (app->video_open_ex(width, height, palette, 25, 0)) { memory->free(pixels); return 1; }
    frames(pixels, size, 1, "burst", false); frames(pixels, size, 40, "paced", false);
    uint32_t rate = 0; uint8_t channels;
    if (!app->audio_open(&rate, &channels)) {
        app->audio_close();
        if (rate && !app->synth_open(silence, NULL, 256, rate)) {
            frames(pixels, size, 40, "audio", false); app->synth_close();
        }
    }
    memory->free(pixels);
    if (solar_os_native_foreground_has_exchange(app)) {
        size_t bytes;
        if (app->video_acquire(&pixels, &bytes) || bytes != size) return 1;
        pixels = frames(pixels, size, 1, "exchange_burst", true);
        pixels = frames(pixels, size, 40, "exchange_paced", true);
        if (rate && !app->synth_open(silence, NULL, 256, rate)) {
            pixels = frames(pixels, size, 40, "exchange_audio", true); app->synth_close();
        }
    }
    text("END\n");
    int error = app->replace_file(argv[1], result, used);
    app->set_exit_code(error ? 1 : 0);
    return error ? 1 : 0;
}
