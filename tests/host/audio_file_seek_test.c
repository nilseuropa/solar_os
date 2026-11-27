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
#include "solar_os_native_audio.h"
#include "solar_os_native_audio_decoder_abi.h"
#include "solar_os_native_audio_pcm_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_work.h"
#include "minimp3.h"

#define AUDIO_WAV_BUFFER_BYTES 4096U
#define AUDIO_WAV_PCM_FORMAT 1U
#define AUDIO_MP3_INPUT_BUFFER_BYTES 16384U
#define AUDIO_MP3_PROBE_SCAN_BYTES 65536U
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
static const solar_os_native_audio_decoder_api_v1_t *decoder_api;
static const solar_os_app_t decoder_owner = {0};
static const solar_os_app_t *active_app;
static solar_os_native_handle_t try_close_during_decode;

const solar_os_app_t *solar_os_application_descriptor(void) { return active_app; }

int __real_mp3dec_decode_frame(mp3dec_t *, const uint8_t *, int, int16_t *, mp3dec_frame_info_t *);
int __wrap_mp3dec_decode_frame(mp3dec_t *d, const uint8_t *bytes, int len,
                              int16_t *pcm, mp3dec_frame_info_t *info)
{
    if (pcm) pcm_decodes++; else header_scans++;
    if (try_close_during_decode) {
        assert(decoder_api->close(try_close_during_decode) == SOLAR_OS_NATIVE_ERROR_BUSY);
        try_close_during_decode = 0;
    }
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
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    const size_t bytes = count * size;
    void *result = solar_os_memory_alloc(bytes, cls, tag);
    if (result) memset(result, 0, bytes);
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
    strcpy(device->id, "fixture");
    strcpy(device->playback_stream, "fixture.pcm");
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

esp_err_t solar_os_audio_record_wav(const char *path, uint32_t duration_ms,
    const solar_os_audio_wav_options_t *options, solar_os_audio_wav_info_t *info)
{
    (void)path; (void)duration_ms; (void)options; (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool legacy_audio_prepared;

esp_err_t solar_os_audio_init(void)
{
    legacy_audio_prepared = true;
    return ESP_OK;
}

void solar_os_audio_deinit(void)
{
    legacy_audio_prepared = false;
}

esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume)
{
    assert(legacy_audio_prepared && volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    return duration_ms == 0 ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t solar_os_audio_play_tone(uint32_t frequency, uint32_t duration, uint8_t volume)
{
    assert(volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL && duration == 100);
    return frequency ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_audio_tone_enqueue(const solar_os_audio_tone_request_t *request, uint32_t *id)
{
    if (!request) return ESP_ERR_INVALID_ARG;
    assert(request->step_count == 1 && request->steps[0].frequency_hz == 800);
    if (id) *id = 12;
    return ESP_OK;
}

esp_err_t solar_os_audio_tone_cancel(uint32_t id)
{
    return id == 12 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void solar_os_audio_tone_queue_get_status(solar_os_audio_tone_queue_status_t *out)
{
    if (out) *out = (solar_os_audio_tone_queue_status_t){ .worker_running = true, .queued = 1 };
}

esp_err_t solar_os_audio_monitor_stream(uint8_t volume,
    const solar_os_audio_wav_options_t *options, solar_os_audio_wav_info_t *info)
{
    (void)volume; (void)options; (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool cancel(void *user)
{ (void)user; return cancel_now || (cancel_after && ++cancel_calls >= cancel_after); }

static void route(bool capture, const char *id, const char *input, const char *output, void *user)
{
    unsigned *routes = user;
    assert(!capture && !strcmp(id, "fixture") && !*input && !strcmp(output, "fixture.pcm"));
    ++*routes;
}

static void run(const char *path, bool mp3)
{
    const solar_os_native_audio_api_v1_t *api =
        solar_os_native_audio_get_service("media.audio", 1, sizeof(*api));
    assert(api && api->capabilities == (SOLAR_OS_AUDIO_FILE_CAP_WAV | SOLAR_OS_AUDIO_FILE_CAP_MP3));
    assert(!solar_os_native_audio_get_service("media.audio", 2, 8));
    assert(!solar_os_native_audio_get_service("media.audio", 1, sizeof(*api) + 1));
    assert(!api->prepare_board() && legacy_audio_prepared);
    assert(!api->loopback(10, SOLAR_OS_AUDIO_VOLUME_GLOBAL));
    assert(api->loopback(0, SOLAR_OS_AUDIO_VOLUME_GLOBAL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    api->release_idle_board();
    assert(!legacy_audio_prepared);
    assert(!api->play_tone(800, 100, SOLAR_OS_AUDIO_VOLUME_GLOBAL));
    assert(api->play_tone(0, 100, SOLAR_OS_AUDIO_VOLUME_GLOBAL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    const solar_os_audio_tone_step_t step = { .frequency_hz = 800, .duration_ms = 100 };
    const solar_os_audio_tone_request_t tone = { .steps = &step, .step_count = 1 };
    uint32_t tone_id = 99;
    assert(api->enqueue_tone(NULL, &tone_id) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && tone_id == 0);
    assert(!api->enqueue_tone(&tone, &tone_id) && tone_id == 12);
    solar_os_audio_tone_queue_status_t tone_state;
    api->tone_status(&tone_state);
    assert(tone_state.worker_running && tone_state.queued == 1);
    assert(!api->cancel_tone(tone_id) && api->cancel_tone(99) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    unsigned routes = 0;
    solar_os_audio_file_options_v1_t options = {
        .should_cancel = cancel, .device = route, .user = &routes,
    };
    solar_os_audio_file_info_v1_t info;
    uint32_t format;
    assert(api->probe(path, SOLAR_OS_AUDIO_FORMAT_AUTO, &format, &info) == SOLAR_OS_NATIVE_OK);
    assert(format == (mp3 ? SOLAR_OS_AUDIO_FORMAT_MP3 : SOLAR_OS_AUDIO_FORMAT_WAV));
    assert(info.sample_rate > 0 && info.channels > 0 && info.block_align > 0);
    captured = NULL; captured_count = 0;
    assert(api->play(path, format, 50, &options, &info) == SOLAR_OS_NATIVE_OK && captured_count > 100000);
    assert(routes == 1);
    size_t total = captured_count;
    int16_t *baseline = captured;
    const uint32_t targets[] = {1700, 300, 0, 501, 40000, 59000, 100000};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        captured = NULL; captured_count = 0;
        options.start_ms = targets[i];
        pcm_decodes = header_scans = 0;
        assert(api->play(path, format, 50, &options, &info) == SOLAR_OS_NATIVE_OK);
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
    assert(api->play(path, format, 50, &options, &info) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(captured_count == 0 && allocations == 0);
    cancel_now = false;
    if (mp3) {
        cancel_after = 20;
        cancel_calls = 0;
        options.start_ms = 59000;
        assert(api->play(path, format, 50, &options, &info) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
        assert(captured_count == 0 && allocations == 0);
        cancel_after = 0;
        fail_allocation = allocation_calls + 5; /* four PCM/input buffers, then the index */
        assert(api->play(path, format, 50, &options, &info) == SOLAR_OS_NATIVE_OK);
        assert(captured_count > 0 && allocations == 0); /* low-memory sequential fallback */
        free(captured);
        captured = NULL;
        captured_count = 0;
        fail_allocation = 0;
    }
}
static void test_decoder(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    uint8_t input[16384];
    const size_t input_size = fread(input, 1, sizeof(input), file);
    fclose(file);

    decoder_api = solar_os_native_audio_get_service(
        SOLAR_OS_NATIVE_AUDIO_DECODER_SERVICE, SOLAR_OS_NATIVE_AUDIO_DECODER_ABI, sizeof(*decoder_api));
    assert(decoder_api);
    assert(!solar_os_native_audio_get_service(SOLAR_OS_NATIVE_AUDIO_DECODER_SERVICE, 1, sizeof(*decoder_api) + 1));

    solar_os_audio_decoder_limits_v1_t limits;
    assert(decoder_api->limits(SOLAR_OS_AUDIO_FORMAT_WAV, &limits) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!limits.max_samples && !limits.stream_window_bytes);
    assert(decoder_api->limits(SOLAR_OS_AUDIO_FORMAT_MP3, &limits) == SOLAR_OS_NATIVE_OK);
    assert(limits.max_samples == SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES);
    assert(limits.stream_window_bytes == SOLAR_OS_AUDIO_MP3_STREAM_WINDOW_BYTES && limits.stream_window_bytes < input_size);

    solar_os_native_stream_format_v1_t format;
    assert(decoder_api->probe(SOLAR_OS_AUDIO_FORMAT_MP3, input, input_size, &format) == SOLAR_OS_NATIVE_OK);
    assert(format.sample_rate && format.channels);

    solar_os_native_handle_t handle = 123;
    assert(decoder_api->open(SOLAR_OS_AUDIO_FORMAT_WAV, &handle) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !handle);
    active_app = &decoder_owner;
    assert(decoder_api->open(SOLAR_OS_AUDIO_FORMAT_MP3, &handle) == SOLAR_OS_NATIVE_OK && handle);
    active_app = NULL;
    solar_os_audio_decoder_frame_v1_t frame;
    assert(decoder_api->process(handle, NULL, 0, NULL, 0, &frame) == SOLAR_OS_NATIVE_OK && !frame.consumed_bytes);
    int16_t native_pcm[SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES], core_pcm[SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES];
    assert(decoder_api->process(handle, input, input_size, native_pcm, limits.max_samples - 1, &frame)
           == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !frame.decoded_samples);
    assert(decoder_api->process(handle, input, input_size, NULL, 0, &frame) == SOLAR_OS_NATIVE_OK);
    assert(frame.consumed_bytes && frame.source_frames && frame.payload_bytes && !frame.decoded_samples);
    assert(decoder_api->reset(handle) == SOLAR_OS_NATIVE_OK);

    solar_os_audio_mp3_decoder_t *core = NULL;
    assert(solar_os_audio_mp3_decoder_create(&core) == ESP_OK);
    size_t offset = 0, decoded_samples = 0;
    for (size_t i = 0; i < 8; i++) {
        const size_t remaining = input_size - offset;
        assert(remaining >= limits.stream_window_bytes);
        size_t consumed;
        solar_os_audio_decoded_frame_t core_frame;
        assert(solar_os_audio_mp3_decode(core, input + offset, remaining, &consumed,
                   core_pcm, limits.max_samples, &core_frame) == ESP_OK);
        try_close_during_decode = handle;
        assert(decoder_api->process(handle, input + offset, remaining, native_pcm, limits.max_samples, &frame)
               == SOLAR_OS_NATIVE_OK);
        assert(!try_close_during_decode && frame.consumed_bytes == consumed && consumed);
        assert(frame.source_frames == core_frame.source_frames && frame.payload_bytes == core_frame.main_data_bytes);
        assert(frame.format.sample_rate == core_frame.format.sample_rate && frame.format.channels == core_frame.format.channels);
        assert(frame.decoded_frames == core_frame.frames && frame.decoded_samples == core_frame.samples);
        assert(!memcmp(native_pcm, core_pcm, frame.decoded_samples * sizeof(native_pcm[0])));
        offset += consumed;
        decoded_samples += frame.decoded_samples;
    }
    assert(decoded_samples);
    solar_os_audio_mp3_decoder_destroy(core);
    assert(solar_os_native_handle_close_owner(&decoder_owner) == SOLAR_OS_NATIVE_OK);
    assert(!solar_os_native_handle_count());
    assert(decoder_api->reset(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(decoder_api->close(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

static void test_pcm(void)
{
    const solar_os_native_audio_pcm_api_v1_t *pcm = solar_os_native_audio_get_service(
        SOLAR_OS_NATIVE_AUDIO_PCM_SERVICE, SOLAR_OS_NATIVE_AUDIO_PCM_ABI, sizeof(*pcm));
    assert(pcm);
    assert(!solar_os_native_audio_get_service(SOLAR_OS_NATIVE_AUDIO_PCM_SERVICE, 1, sizeof(*pcm) + 1));
    solar_os_native_handle_t handle;
    active_app = &decoder_owner;
    assert(pcm->open(&handle) == SOLAR_OS_NATIVE_OK);
    active_app = NULL;

    const solar_os_native_stream_format_v1_t mono = {.sample_rate = 48000, .channels = 1};
    const solar_os_native_stream_format_v1_t stereo = {.sample_rate = 48000, .channels = 2};
    const int16_t input[] = {1000, -4000, 8000};
    int16_t output[4];
    size_t count;
    bool done;
    assert(pcm->convert(handle, input, 3, &mono, &stereo, output, 4, &count, &done) == SOLAR_OS_NATIVE_OK);
    assert(count == 4 && !done && output[0] == 1000 && output[1] == 1000);
    assert(output[2] == -4000 && output[3] == -4000);
    assert(pcm->convert(handle, input, 3, &mono, &stereo, output, 4, &count, &done) == SOLAR_OS_NATIVE_OK);
    assert(count == 2 && done && output[0] == 8000 && output[1] == 8000);

    assert(pcm->reset(handle) == SOLAR_OS_NATIVE_OK);
    const solar_os_native_stream_format_v1_t slower = {.sample_rate = 8000, .channels = 1};
    assert(pcm->convert(handle, input, 3, &mono, &slower, output, 4, &count, &done) == SOLAR_OS_NATIVE_OK);
    assert(count == 1 && done && output[0] == input[0]);
    assert(pcm->convert(handle, input, 3, &mono, &slower, output, 4, &count, &done) == SOLAR_OS_NATIVE_OK);
    assert(count == 0 && done);
    assert(pcm->convert(handle, input, SIZE_MAX, &mono, &slower, output, 4, &count, &done) ==
           SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !count && !done);
    assert(pcm->convert(handle, input, 3, &mono, &slower, output, 4, &count, &done) == SOLAR_OS_NATIVE_OK);
    assert(count == 1 && done && output[0] == input[0]);
    assert(solar_os_native_handle_close_owner(&decoder_owner) == SOLAR_OS_NATIVE_OK);
    assert(!solar_os_native_handle_count() && !allocations);
    assert(pcm->reset(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(pcm->close(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    run(argv[1], false);
    run(argv[2], false);
    run(argv[3], true);
    run(argv[4], true);
    run(argv[5], true);
    test_pcm();
    test_decoder(argv[3]);
    test_decoder(argv[4]);
    test_decoder(argv[5]);
    assert(work_checkpoints > 0);
    puts("Native MP3 decoder limits, stream consumption, PCM and owner cleanup passed");
    puts("WAV/CBR MP3/VBR MP3 seek positions, PCM, end clamp and cancellation passed");
}
