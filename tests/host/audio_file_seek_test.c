#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_audio.h"
#include "solar_os_audio_codec.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_audio_player.h"
#include "solar_os_memory.h"
#include "solar_os_work.h"
#include "minimp3.h"

#define AUDIO_WAV_BUFFER_BYTES 4096U
#define AUDIO_WAV_PCM_FORMAT 1U
#define AUDIO_MP3_INPUT_BUFFER_BYTES 16384U
#define AUDIO_MP3_OUTPUT_SAMPLES_MAX (AUDIO_WAV_BUFFER_BYTES / sizeof(int16_t))
#define AUDIO_FRAME_CHUNK 1U
#define SOLAR_OS_LOGI(...) ((void)0)
#define SOLAR_OS_LOGW(...) ((void)0)

static int16_t *captured;
static size_t captured_count;
static bool cancel_now;
static unsigned allocations;
static unsigned allocation_calls, fail_allocation, cancel_calls, cancel_after;
static unsigned pcm_decodes, header_scans;
static unsigned work_checkpoints;
int __real_mp3dec_decode_frame(mp3dec_t *, const uint8_t *, int, int16_t *, mp3dec_frame_info_t *);
int __wrap_mp3dec_decode_frame(mp3dec_t *d, const uint8_t *bytes, int len,
                              int16_t *pcm, mp3dec_frame_info_t *info)
{
    if (pcm) pcm_decodes++; else header_scans++;
    return __real_mp3dec_decode_frame(d, bytes, len, pcm, info);
}
struct solar_os_audio_player { int unused; };
static struct solar_os_audio_player sink;

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls; (void)tag;
    if (++allocation_calls == fail_allocation) return NULL;
    void *result = malloc(size);
    if (result) allocations++;
    return result;
}
void solar_os_memory_free(void *p) { if (p) allocations--; free(p); }
static void *audio_heap_alloc(size_t size)
{ return solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "test"); }
static void audio_log_heap_nomem(const char *where, size_t size)
{ (void)where; (void)size; }
static bool solar_os_storage_is_mounted(void) { return true; }
static bool audio_playback_use_buffered_player(void) { return false; }
static int64_t esp_timer_get_time(void) { return 0; }
static void vTaskDelay(unsigned ticks) { (void)ticks; }
void solar_os_work_begin(solar_os_work_budget_t *budget) { budget->last_yield_us = 0; }
void solar_os_work_checkpoint(solar_os_work_budget_t *budget)
{ (void)budget; work_checkpoints++; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test"; }

esp_err_t solar_os_audio_player_create(const solar_os_audio_player_options_t *options,
    solar_os_audio_player_t **player, solar_os_stream_audio_format_t *format,
    solar_os_audio_device_info_t *device)
{
    (void)options;
    memset(device, 0, sizeof(*device));
    *format = (solar_os_stream_audio_format_t){
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .sample_rate = 44100,
        .channels = 1, .bits_per_sample = 16, .frames_per_block = 1};
    *player = &sink;
    return ESP_OK;
}
esp_err_t solar_os_audio_player_write(solar_os_audio_player_t *player,
    const void *data, size_t len, const volatile bool *cancelled)
{
    (void)player; (void)cancelled;
    captured = realloc(captured, captured_count * sizeof(*captured) + len);
    assert(captured);
    memcpy(captured + captured_count, data, len);
    captured_count += len / sizeof(*captured);
    return ESP_OK;
}
esp_err_t solar_os_audio_player_finish(solar_os_audio_player_t *player,
    const volatile bool *cancelled) { (void)player; (void)cancelled; return ESP_OK; }
void solar_os_audio_player_destroy(solar_os_audio_player_t *player) { (void)player; }

/* Generated from production function bodies by test_audio_file_seek.py. */
#include "audio_file_playback.inc"

static bool cancel(void *user)
{ (void)user; return cancel_now || (cancel_after && ++cancel_calls >= cancel_after); }
static void run(const char *path, bool mp3)
{
    solar_os_audio_wav_options_t options = {.should_cancel = cancel};
    solar_os_audio_wav_info_t info;
    esp_err_t (*play)(const char *, uint8_t, const solar_os_audio_wav_options_t *,
                     solar_os_audio_wav_info_t *) = mp3 ? audio_play_mp3_stream : audio_play_wav_stream;
    captured = NULL; captured_count = 0;
    assert(play(path, 50, &options, &info) == ESP_OK && captured_count > 100000);
    size_t total = captured_count;
    int16_t *baseline = captured;
    const uint32_t targets[] = {1700, 300, 0, 501, 40000, 59000, 100000};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        captured = NULL; captured_count = 0;
        options.start_ms = targets[i];
        pcm_decodes = header_scans = 0;
        assert(play(path, 50, &options, &info) == ESP_OK);
        if (mp3 && targets[i] >= 40000) {
            /* Real minimp3 calls: only bounded warm-up, not the full prefix. */
            size_t output_frames = (captured_count + 1151) / 1152;
            unsigned bound = strstr(path, "mpeg2.mp3") ? 520 : 140;
            assert(pcm_decodes < output_frames + bound && header_scans > 1000);
            if (targets[i] == 59000)
                printf("59-second MP3 seek: %u PCM decodes, %u header scans\n", pcm_decodes, header_scans);
        }
        size_t offset = (uint64_t)targets[i] * 44100 / 1000;
        if (offset >= total) assert(captured_count == 0);
        else {
            assert(llabs((long long)captured_count - (long long)(total - offset)) <= 8);
            bool match = false;
            for (int adjustment = -8; adjustment <= 8; adjustment++) {
                int64_t at = (int64_t)offset + adjustment;
                if (at >= 0 && (uint64_t)at + 1000 <= total && captured_count >= 1000 &&
                    memcmp(captured, baseline + at, 1000 * sizeof(*baseline)) == 0) match = true;
            }
            assert(match);
        }
        free(captured);
        assert(allocations == 0);
    }
    free(baseline);
    captured = NULL; captured_count = 0;
    cancel_now = true;
    options.start_ms = 1700;
    assert(play(path, 50, &options, &info) == ESP_ERR_TIMEOUT);
    assert(captured_count == 0 && allocations == 0);
    cancel_now = false;
    if (mp3) {
        cancel_after = 20;
        cancel_calls = 0;
        options.start_ms = 59000;
        assert(play(path, 50, &options, &info) == ESP_ERR_TIMEOUT);
        assert(captured_count == 0 && allocations == 0);
        cancel_after = 0;
        fail_allocation = allocation_calls + 5; /* four PCM/input buffers, then the index */
        assert(play(path, 50, &options, &info) == ESP_OK);
        assert(captured_count > 0 && allocations == 0); /* low-memory sequential fallback */
        free(captured);
        captured = NULL;
        captured_count = 0;
        fail_allocation = 0;
    }
}
int main(int argc, char **argv)
{
    assert(argc == 6);
    run(argv[1], false);
    run(argv[2], false);
    run(argv[3], true);
    run(argv[4], true);
    run(argv[5], true);
    assert(work_checkpoints > 0);
    puts("WAV/CBR MP3/VBR MP3 seek positions, PCM, end clamp and cancellation passed");
}
