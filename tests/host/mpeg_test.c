#include "solar_os_memory.h"
#include "solar_os_mpeg.h"
#include "solar_os.h"
#include "solar_os_native_mpeg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union allocation {
    max_align_t align;
    struct {
        size_t size;
    } block;
} allocation_t;
static size_t live, peak;
static unsigned calls, fail_at;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)tag;
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);
    if (++calls == fail_at)
        return NULL;
    allocation_t *a = malloc(sizeof(*a) + size);
    assert(a);
    a->block.size = size;
    live += size;
    if (live > peak)
        peak = live;
    return a + 1;
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls,
                             const char *tag)
{
    void *p = solar_os_memory_alloc(count * size, cls, tag);
    if (p)
        memset(p, 0, count * size);
    return p;
}
void solar_os_memory_free(void *ptr)
{
    if (!ptr)
        return;
    allocation_t *a = (allocation_t *)ptr - 1;
    live -= a->block.size;
    free(a);
}
void *solar_os_memory_realloc(void *ptr, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    void *p = solar_os_memory_alloc(size, cls, tag);
    if (!p)
        return NULL;
    if (ptr) {
        allocation_t *a = (allocation_t *)ptr - 1;
        memcpy(p, ptr, size < a->block.size ? size : a->block.size);
        solar_os_memory_free(ptr);
    }
    return p;
}
static unsigned cancel_checks, cancel_after;
static bool cancel(void *user)
{ cancel_checks++; return *(bool *)user || (cancel_after && cancel_checks >= cancel_after); }

const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }

typedef struct {
    const solar_os_native_mpeg_api_v1_t *api;
    solar_os_native_handle_t decoder;
    unsigned frames, audio_chunks;
} native_playback_t;

static int native_video(const solar_os_native_mpeg_frame_v1_t *frame, void *user)
{
    native_playback_t *playback = user;
    assert(frame->width == 160 && frame->height == 120 && frame->y_bytes >= 160 * 120);
    assert(playback->api->close(playback->decoder) == SOLAR_OS_NATIVE_ERROR_BUSY);
    solar_os_mpeg_info_t info;
    assert(playback->api->info(playback->decoder, &info) == SOLAR_OS_NATIVE_ERROR_BUSY);
    uint8_t raster[13 * 9 * 2];
    assert(playback->api->raster(frame, raster, sizeof(raster) - 1, 13, 9, true, false) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(!playback->api->raster(frame, raster, sizeof(raster), 13, 9, true, false));
    assert(!playback->api->raster(frame, raster, sizeof(raster), 13, 9, false, false) && raster[0] == frame->y[0]);
    solar_os_native_mpeg_frame_v1_t invalid = *frame;
    invalid.chroma_bytes = 1;
    assert(playback->api->raster(&invalid, raster, sizeof(raster), 13, 9, true, false) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    ++playback->frames;
    return SOLAR_OS_NATIVE_OK;
}

static int native_audio(const solar_os_mpeg_audio_t *audio, void *user)
{
    native_playback_t *playback = user;
    assert(audio->frames == 1152 && audio->sample_rate == 44100 && isfinite(audio->samples[0]));
    ++playback->audio_chunks;
    return SOLAR_OS_NATIVE_OK;
}

static void test_native_mpeg(const char *video_path, const char *video_only_path)
{
    const solar_os_native_mpeg_api_v1_t *api = solar_os_native_mpeg_get_service(
        SOLAR_OS_NATIVE_MPEG_SERVICE, SOLAR_OS_NATIVE_MPEG_ABI, sizeof(*api));
    assert(api);
    native_playback_t playback = {.api = api};
    bool stopped = false, ended = false;
    char detail[96];
    assert(!api->open(video_path, cancel, &stopped, &playback.decoder, detail, sizeof(detail)));
    stopped = true; /* The callback passed to open expired when it returned. */
    solar_os_mpeg_info_t info;
    assert(!api->info(playback.decoder, &info) && info.fps == 25 && info.audio);
    assert(!api->video(playback.decoder, native_video, &playback, NULL, NULL, &ended) && !ended && playback.frames == 1);
    assert(!api->audio(playback.decoder, native_audio, &playback, NULL, NULL, &ended) && !ended && playback.audio_chunks == 1);
    double position;
    assert(!api->seek(playback.decoder, 1.0, &position, NULL, NULL) && position >= 1.0);
    assert(!api->video(playback.decoder, native_video, &playback, NULL, NULL, &ended) && playback.frames == 2);
    assert(api->video(playback.decoder, native_video, &playback, cancel, &stopped, &ended) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->error_detail(playback.decoder, detail, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !detail[0]);
    assert(!api->error_detail(playback.decoder, detail, sizeof(detail)) && strstr(detail, "cancelled"));
    assert(!api->close(playback.decoder) && !live);
    assert(api->info(playback.decoder, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->open(video_only_path, NULL, NULL, &playback.decoder, NULL, 0));
    assert(!api->audio(playback.decoder, native_audio, &playback, NULL, NULL, &ended) && ended && playback.audio_chunks == 1);
    assert(!api->close(playback.decoder) && !live);
}
static uint32_t frame_hash(const solar_os_mpeg_frame_t *f)
{
    uint32_t hash = 2166136261U;
    for (unsigned y = 0; y < f->height; y++)
        for (unsigned x = 0; x < f->width; x++)
            hash = (hash ^ f->y[y * f->y_stride + x]) * 16777619U;
    return hash;
}
static esp_err_t play(const char *path, unsigned *frames)
{
    solar_os_mpeg_t *d = NULL;
    char detail[96];
    esp_err_t err = solar_os_mpeg_open(path, NULL, NULL, &d, detail, sizeof(detail));
    *frames = 0;
    if (err != ESP_OK) {
        assert(d == NULL);
        assert(detail[0]);
        return err;
    }
    solar_os_mpeg_info_t info;
    solar_os_mpeg_info(d, &info);
    assert(info.width == 160 && info.height == 120 && info.fps == 25);
    bool vend = false, aend = !info.audio;
    double ahead = 0, previous = -1;
    unsigned audio_frames = 0;
    uint8_t *rgb = malloc(161 * 123 * 2), *mono = malloc(161 * 123);
    while (!vend && err == ESP_OK) {
        solar_os_mpeg_frame_t f;
        err = solar_os_mpeg_video(d, &f, &vend);
        if (err != ESP_OK || vend)
            break;
        assert(f.time > previous);
        previous = f.time;
        (*frames)++;
        solar_os_mpeg_raster(&f, rgb, 161, 123, true, true);
        solar_os_mpeg_raster(&f, mono, 161, 123, false, true);
        assert(mono[0] == f.y[0]);
        while (!aend && ahead < f.time + 0.2) {
            solar_os_mpeg_audio_t a;
            err = solar_os_mpeg_audio(d, &a, &aend);
            if (err != ESP_OK || aend)
                break;
            assert(a.frames == 1152);
            for (unsigned i = 0; i < a.frames * 2; i++)
                assert(isfinite(a.samples[i]));
            audio_frames += a.frames;
            ahead = a.time + (double)a.frames / a.sample_rate;
        }
        if (*frames > 1010)
            abort();
    }
    free(rgb);
    free(mono);
    if (err != ESP_OK)
        assert(solar_os_mpeg_error(d)[0]);
    if (err == ESP_OK && info.audio)
        assert(audio_frames > 40000);
    solar_os_mpeg_close(d);
    assert(live == 0);
    return err;
}
static void raster_tests(void)
{
    uint8_t y[32 * 32], cb[16 * 16], cr[16 * 16], output[37 * 35 * 2], expected[37 * 35 * 2];
    uint32_t seed = 7;
    for (size_t i = 0; i < sizeof(y); i++) {
        seed = seed * 1664525 + 1013904223;
        y[i] = seed >> 24;
    }
    for (size_t i = 0; i < sizeof(cb); i++) {
        seed = seed * 1664525 + 1013904223;
        cb[i] = seed >> 24;
        seed = seed * 1664525 + 1013904223;
        cr[i] = seed >> 24;
    }
    solar_os_mpeg_frame_t f = {
        .width = 29, .height = 27, .y = y, .cb = cb, .cr = cr, .y_stride = 32, .chroma_stride = 16};
    const unsigned widths[] = {1, 7, 8, 13, 29, 37}, heights[] = {1, 9, 27, 35};
    for (unsigned color = 0; color < 2; color++)
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 4; j++) {
                unsigned w = widths[i], h = heights[j], bpp = color ? 2 : 1;
                memset(output, 0xa5, sizeof(output));
                solar_os_mpeg_raster(&f, output, w, h, color, true);
                solar_os_mpeg_raster(&f, expected, w, h, color, false);
                assert(!memcmp(output, expected, w * h * bpp));
                for (size_t k = w * h * bpp; k < sizeof(output); k++)
                    assert(output[k] == 0xa5);
            }
}
static void seek_tests(const char *path)
{
    solar_os_mpeg_t *d = NULL;
    bool stopped = false;
    char detail[96];
    assert(solar_os_mpeg_open(path, cancel, &stopped, &d, detail, sizeof(detail)) == ESP_OK);
    solar_os_mpeg_info_t info;
    solar_os_mpeg_info(d, &info);
    float reference[512];
    if (info.audio) {
        double position;
        assert(solar_os_mpeg_seek(d, 0, &position) == ESP_OK);
        solar_os_mpeg_audio_t audio;
        bool ended;
        assert(solar_os_mpeg_audio(d, &audio, &ended) == ESP_OK && !ended);
        assert(audio.frames >= 256);
        memcpy(reference, audio.samples, sizeof(reference));
    }
    const double targets[] = {2.1, 0.4, 3.0, 0, -10, 100};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        double position;
        assert(solar_os_mpeg_seek(d, targets[i], &position) == ESP_OK);
        double target = targets[i] < 0 ? 0 : targets[i];
        if (target < 3.9) assert(position >= target && position < target + 0.1);
        else assert(position > 3.8 && position < 4.1);
        solar_os_mpeg_frame_t frame;
        bool ended;
        assert(solar_os_mpeg_video(d, &frame, &ended) == ESP_OK && !ended);
        assert(frame.time == position && frame.y != NULL);
        if (info.audio) {
            solar_os_mpeg_audio_t audio;
            assert(solar_os_mpeg_audio(d, &audio, &ended) == ESP_OK);
            if (!ended) assert(fabs(audio.time - position) < 0.04);
            if (target == 0) {
                assert(!ended && audio.frames >= 256);
                assert(memcmp(reference, audio.samples, sizeof(reference)) == 0);
            }
        }
    }
    double position;
    assert(solar_os_mpeg_seek(d, NAN, &position) == ESP_ERR_INVALID_ARG);
    stopped = true;
    assert(solar_os_mpeg_seek(d, 1, &position) == ESP_ERR_TIMEOUT);
    solar_os_mpeg_close(d);
    assert(live == 0);
    if (info.audio) {
        stopped = false;
        assert(solar_os_mpeg_open(path, cancel, &stopped, &d, detail, sizeof(detail)) == ESP_OK);
        fail_at = calls + 1;
        assert(solar_os_mpeg_seek(d, 2, &position) == ESP_ERR_NO_MEM);
        assert(solar_os_mpeg_error(d)[0]);
        fail_at = 0;
        solar_os_mpeg_close(d);
        assert(live == 0);
    }
}
static void fast_seek_tests(const char *path)
{
    solar_os_mpeg_t *d = NULL;
    bool stopped = false, vend = false, aend = false;
    char detail[96];
    assert(solar_os_mpeg_open(path, cancel, &stopped, &d, detail, sizeof(detail)) == ESP_OK);
    solar_os_mpeg_frame_t frame;
    solar_os_mpeg_audio_t audio;
    double audio_until = -1;
    uint32_t hashes[1000];
    double times[1000];
    unsigned hash_count = 0;
    const double targets[] = {30, 10, 37, 0.4};
    float audio_reference[4][1152 * 2];
    double audio_time[4] = {0};
    bool audio_saved[4] = {false};
    do {
        assert(solar_os_mpeg_video(d, &frame, &vend) == ESP_OK);
        if (vend) break;
        assert(hash_count < 1000);
        times[hash_count] = frame.time;
        hashes[hash_count++] = frame_hash(&frame);
        while (!aend && audio_until < frame.time) {
            assert(solar_os_mpeg_audio(d, &audio, &aend) == ESP_OK);
            if (!aend) audio_until = audio.time + (double)audio.frames / audio.sample_rate;
        }
        for (unsigned n = 0; n < 4; n++) {
            if (!audio_saved[n] && frame.time >= targets[n]) {
                assert(!aend && audio.frames == 1152);
                memcpy(audio_reference[n], audio.samples, sizeof(audio_reference[n]));
                audio_time[n] = audio.time;
                audio_saved[n] = true;
            }
        }
    } while (!vend);
    for (unsigned n = 0; n < sizeof(targets) / sizeof(targets[0]); n++) {
        unsigned expected = 0;
        while (expected + 1 < hash_count && times[expected] < targets[n]) expected++;
        cancel_checks = 0;
        double position;
        assert(solar_os_mpeg_seek(d, targets[n], &position) == ESP_OK);
        assert(cancel_checks < 300); /* Full-prefix decoding requires thousands. */
        assert(solar_os_mpeg_video(d, &frame, &vend) == ESP_OK && !vend);
        assert(fabs(frame.time - times[expected]) < 0.0001);
        assert(frame_hash(&frame) == hashes[expected]);
        assert(solar_os_mpeg_audio(d, &audio, &aend) == ESP_OK && !aend);
        assert(fabs(audio.time - position) < 0.04);
        int at = (int)llround((audio.time - audio_time[n]) * audio.sample_rate);
        assert(at >= 0 && at < 1152);
        size_t samples = audio.frames < (size_t)(1152 - at) ? audio.frames : (size_t)(1152 - at);
        /* Synthesis-window summation can differ in its last float bits after
         * a restart. Require agreement far below one 16-bit PCM step. */
        for (size_t k = 0; k < samples * 2; k++) {
            assert(fabsf(audio.samples[k] - audio_reference[n][at * 2 + k]) < 0.000001f);
        }
        printf("%.1f-second MPEG seek: %u cancel checks, matching reference pixels and A/V time\n",
               targets[n], cancel_checks);
    }
    solar_os_mpeg_close(d);
    assert(live == 0);
    assert(solar_os_mpeg_open(path, cancel, &stopped, &d, detail, sizeof(detail)) == ESP_OK);
    cancel_checks = 0;
    cancel_after = 10;
    double position;
    assert(solar_os_mpeg_seek(d, 30, &position) == ESP_ERR_TIMEOUT);
    solar_os_mpeg_close(d);
    cancel_after = 0;
    assert(live == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 11);
    raster_tests();
    unsigned frames;
    assert(play(argv[1], &frames) == ESP_OK && frames == 100);
    unsigned allocations = calls;
    assert(peak < 1024 * 1024);
    calls = 0;
    assert(play(argv[2], &frames) == ESP_OK && frames == 100);
    assert(play(argv[3], &frames) == ESP_ERR_NOT_SUPPORTED);
    assert(play(argv[4], &frames) == ESP_OK && frames == 1000);
    seek_tests(argv[1]);
    seek_tests(argv[2]);
    fast_seek_tests(argv[4]);
    fast_seek_tests(argv[5]);
    seek_tests(argv[argc - 1]);
    for (unsigned i = 1; i <= allocations; i++) {
        calls = 0;
        fail_at = i;
        esp_err_t err = play(argv[1], &frames);
        assert(err == ESP_ERR_NO_MEM);
        assert(live == 0);
    }
    fail_at = 0;
    for (int i = 6; i < argc - 1; i++) {
        solar_os_mpeg_t *invalid = NULL;
        char detail[96];
        esp_err_t err = solar_os_mpeg_open(argv[i], NULL, NULL, &invalid, detail, sizeof(detail));
        assert(err != ESP_OK && invalid == NULL && detail[0] && live == 0);
    }
    bool stopped = true;
    solar_os_mpeg_t *d = NULL;
    char detail[96];
    assert(solar_os_mpeg_open(argv[1], cancel, &stopped, &d, detail, sizeof(detail)) ==
           ESP_ERR_TIMEOUT);
    assert(!d && !live);
    stopped = false;
    assert(solar_os_mpeg_open(argv[1], cancel, &stopped, &d, detail, sizeof(detail)) == ESP_OK);
    stopped = true;
    solar_os_mpeg_frame_t frame;
    bool ended = false;
    assert(solar_os_mpeg_video(d, &frame, &ended) == ESP_ERR_TIMEOUT);
    solar_os_mpeg_close(d);
    assert(live == 0);
    test_native_mpeg(argv[1], argv[2]);
    puts("MPEG frames, audio, raster, format rejection, cancellation and allocation failures "
         "passed");
}
