#define SOLAR_OS_DOOM_PORT_IMPLEMENTATION
#include "port.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

int solar_os_doom_main(int argc, char **argv);
static const char *wad_path;
static unsigned allocations, calls, frames, video_opens, suspends, phase;
static unsigned fail_after, reads;
static bool short_read, did_short_read;
static uint64_t now;
static const char *mode;
static solar_os_native_foreground_audio_render_v1_t audio_render;
static void *audio_user;
static unsigned audio_starts, audio_closes, nonzero_samples, audio_blocks;
static uint32_t audio_rate = 16000;
static bool audio_probe, mixer_locked, mutex_ready, in_callback;
static void pump_audio(void)
{
    if (!audio_render) return;
    assert(!mixer_locked && !in_callback);
    int16_t samples[512];
    mixer_locked = in_callback = true;
    audio_render(samples, 256, audio_rate, audio_user);
    mixer_locked = in_callback = false;
    audio_blocks++;
    for (unsigned i = 0; i < 512; i++) if (samples[i]) nonzero_samples++;
}
static void *allocate(size_t size)
{
    assert(!in_callback);
    ++calls;
    if (fail_after && calls >= fail_after) return NULL;
    void *p = malloc(size); if (p) allocations++; return p;
}
static void release(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
static int write_text(const char *text, size_t length) { return fwrite(text, 1, length, stdout); }
static uint64_t time_us(void) { return now += 1000; }
static void sleep_ms(uint32_t ms) { now += (uint64_t)ms * 1000; pump_audio(); }
static int resolve(const char *path, char *out, size_t size)
{ return snprintf(out, size, "%s", path) >= size ? -1 : 0; }
static int file_size(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb"); if (!file) return -1;
    fseek(file, 0, SEEK_END); *size = ftell(file); fclose(file); return 0;
}
static int read_file(const char *path, uint32_t offset, void *out, size_t length, size_t *actual)
{
    assert(!in_callback);
    assert(length <= 65536);
    FILE *file = fopen(path, "rb"); if (!file) return -1;
    fseek(file, offset, SEEK_SET); *actual = fread(out, 1, length, file); fclose(file);
    if (short_read && ++reads == 4 && *actual) { (*actual)--; did_short_read = true; }
    return 0;
}
static int video_open(uint16_t width, uint16_t height, const uint16_t *palette, uint16_t fps, uint8_t background)
{
    assert(width == 320 && height == 240 && fps == 35 && palette && background == 0);
    assert(++video_opens == 1); return 0;
}
static int present(const uint8_t *pixels, size_t size)
{ assert(pixels && size == 320 * 240); frames++; return 0; }
static uint8_t host_frames[2][320 * 240];
static unsigned host_writing;
static int acquire_frame(uint8_t **pixels, size_t *size)
{ host_writing = 0; *pixels = host_frames[0]; *size = 320 * 240; return 0; }
static int exchange_frame(uint8_t **pixels)
{
    assert(*pixels == host_frames[host_writing]);
    present(*pixels, 320 * 240);
    *pixels = host_frames[host_writing ^= 1];
    memset(*pixels, 0xcd, 320 * 240); return 0;
}
static bool stop_requested(void) { return frames >= 120 && phase == 12; }
static bool suspend_requested(void) { return frames >= 12 && !suspends; }
static int suspend_app(void)
{ assert(!audio_render); suspends++; now += 5000000; if (!strcmp(mode, "switch-rate")) audio_rate = 48000; return 0; }
static int enable_suspend(void) { return 0; }
static int audio_open(uint32_t *rate, uint8_t *channels)
{
    assert(!audio_render && !audio_probe);
    if (!strcmp(mode, "audio-denied")) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    audio_probe = true; *rate = audio_rate; *channels = 2; return 0;
}
static void audio_close(void) { assert(audio_probe); audio_probe = false; }
static int synth_open(solar_os_native_foreground_audio_render_v1_t render, void *user,
    size_t frames, uint32_t rate)
{
    assert(!audio_render && !audio_probe && rate == audio_rate && frames == 256);
    mutex_ready = true;
    if (!strcmp(mode, "synth-denied")) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    audio_render = render; audio_user = user; audio_starts++; return 0;
}
static bool synth_lock(void)
{ if (!mutex_ready) return false; assert(!mixer_locked); mixer_locked = true; return true; }
static void synth_unlock(void) { assert(mixer_locked); mixer_locked = false; }
static void synth_close(void) { assert(!mixer_locked); if (audio_render) audio_closes++; audio_render = NULL; }
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    /* Start a game through the real menu, then exercise held movement/fire. */
    const unsigned at[] = {2, 2, 4, 4, 6, 6, 8, 8, 14, 18, 20, 24};
    const unsigned usage[] = {0x29,0x29,0x28,0x28,0x28,0x28,0x28,0x28,0x52,0x52,0xe0,0xe0};
    *available = phase < sizeof(at)/sizeof(at[0]) && frames >= at[phase];
    if (*available) {
        *key = (solar_os_native_foreground_key_v1_t){.usage = usage[phase],
            .modifiers = phase == 10 ? 1 : 0, .action = (phase & 1) ? 1 : 0};
        phase++;
    }
    return 0;
}
static solar_os_native_foreground_api_v1_t app = {
    .abi_version = 1, .struct_size = sizeof(app), .time_us = time_us, .sleep_ms = sleep_ms,
    .resolve_path = resolve, .file_size = file_size, .read_file = read_file,
    .video_open_ex = video_open, .present = present, .read_key = read_key,
    .stop_requested = stop_requested, .enable_suspend = enable_suspend,
    .suspend_requested = suspend_requested, .suspend = suspend_app,
    .audio_open = audio_open, .audio_close = audio_close, .synth_open = synth_open,
    .synth_lock = synth_lock, .synth_unlock = synth_unlock, .synth_close = synth_close,
};
static solar_os_native_memory_api_v1_t memory = {
    .abi_version = 1, .struct_size = sizeof(memory), .alloc = allocate, .free = release,
};
static solar_os_native_libc_api_v1_t libc = {
    .abi_version = 1, .struct_size = sizeof(libc), .format = vsnprintf,
};
static const void *service(const char *id, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
    if (!strcmp(id, "foreground")) { assert(size <= sizeof(app)); return &app; }
    if (!strcmp(id, "memory")) { assert(size <= sizeof(memory)); return &memory; }
    if (!strcmp(id, "libc")) { assert(size <= sizeof(libc)); return &libc; }
    return NULL;
}
static solar_os_native_host_api_v1_t host = {
    .abi_version = SOLAR_OS_NATIVE_ABI_VERSION, .struct_size = sizeof(host),
    .write_utf8 = write_text, .get_service = service,
};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }
int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    assert(argc >= 3); wad_path = argv[1];
    mode = argv[2];
    if (!strcmp(mode, "exchange")) { app.video_acquire = acquire_frame; app.video_exchange = exchange_frame; }
    if (!strcmp(mode, "no-synth")) app.struct_size = offsetof(solar_os_native_foreground_api_v1_t, video_open_index2);
    fail_after = argc > 3 ? atoi(argv[3]) : 0;
    short_read = !strcmp(argv[2], "short");
    char *args[] = {"doom", (char *)wad_path};
    int result = solar_os_doom_main(2, args);
    printf("result=%d frames=%u suspends=%u keys=%u state=%d\n", result, frames, suspends, phase, gamestate);
    assert(!allocations);
    assert(!audio_render && !audio_probe && !mixer_locked && audio_starts == audio_closes);
    if (!strcmp(mode, "game") || !strcmp(mode, "exchange") || !strcmp(mode, "switch-rate") || !strcmp(mode, "audio-denied") ||
        !strcmp(mode, "synth-denied") || !strcmp(mode, "no-synth") || !strcmp(mode, "silent-samples") ||
        !strcmp(mode, "no-samples")) {
        assert(result == 0 && frames >= 120 && suspends == 1 && phase == 12);
        assert(gamestate == GS_LEVEL && solar_os_doom_epoch >= 5000000);
        if (!strcmp(mode, "game") || !strcmp(mode, "exchange") || !strcmp(mode, "switch-rate"))
            assert(audio_starts == 2 && audio_blocks > 0 && nonzero_samples > 0);
        else assert(nonzero_samples == 0);
    } else { assert(result != 0); }
    if (short_read) assert(did_short_read);
    printf("doom: host lifecycle passed (frames=%u, allocations=%u, calls=%u)\n", frames, allocations, calls);
    return 0;
}
