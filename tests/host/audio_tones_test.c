#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_audio.h"

#define SOLAR_OS_LOGI(...) ((void)0)
#define UINT32_MAX_TIMEOUT UINT32_MAX
typedef unsigned TickType_t;
/* Exercise a tick rate at which a 10 ms polling delay rounds down to zero. */
#define pdMS_TO_TICKS(ms) ((ms) / 20U)

static bool opened;
static unsigned opens, closes, writes, volume_sets;
static uint8_t selected_volume;
static uint32_t rate;
static uint8_t channels;
static uint32_t native_block;
static uint64_t played_frames;
static int16_t pending[8192];
static size_t head, queued, capacity, audible;
static size_t run_lengths[8], gaps[8];
static unsigned runs;
static bool in_tone;
static size_t silence_frames;
static volatile bool cancelled;
static unsigned cancel_write, fail_write, short_write;
static bool cancel_drain;

static void play_frame(void)
{
    int16_t sample = 0;
    if (queued) {
        sample = pending[head];
        head = (head + 1U) % capacity;
        queued--;
    }
    /* Model the user's silent first 100 ms after the speaker is enabled. */
    if (played_frames * 1000ULL < (uint64_t)rate * 100U) sample = 0;
    played_frames++;
    if (sample) {
        if (!in_tone) {
            assert(runs < 8);
            gaps[runs] = silence_frames;
            runs++;
            in_tone = true;
        }
        run_lengths[runs - 1U]++;
        audible++;
        silence_frames = 0;
    } else {
        in_tone = false;
        silence_frames++;
    }
}

static int64_t esp_timer_get_time(void)
{
    return (int64_t)(played_frames * 1000000ULL / rate);
}

static void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    const size_t frames = (uint64_t)rate * ticks * 20U / 1000U;
    for (size_t i = 0; i < frames; i++) play_frame();
    if (cancel_drain) cancelled = true;
}

esp_err_t solar_os_audio_open_default(
    solar_os_stream_direction_t direction, const char *owner,
    const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *stream, solar_os_audio_device_info_t *device)
{
    assert(!opened && direction == SOLAR_OS_STREAM_DIRECTION_SINK);
    assert(strcmp(owner, "audio-tone") == 0);
    assert(options->requested_audio.sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE);
    opened = true;
    opens++;
    stream->audio = (solar_os_stream_audio_format_t) {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = rate, .channels = channels, .bits_per_sample = 16,
        .frames_per_block = native_block,
    };
    strcpy(device->id, "speaker");
    return ESP_OK;
}

esp_err_t solar_os_audio_set_device_volume(const char *id, uint8_t volume)
{
    assert(opened && strcmp(id, "speaker") == 0);
    volume_sets++;
    selected_volume = volume;
    return ESP_OK;
}

esp_err_t solar_os_stream_write(solar_os_stream_handle_t *stream,
                                const void *data, size_t len,
                                uint32_t timeout_ms, size_t *written)
{
    assert(opened && stream->audio.channels == channels);
    assert(timeout_ms == UINT32_MAX_TIMEOUT);
    writes++;
    if (writes == fail_write) return ESP_FAIL;
    if (writes == short_write) { *written = 0; return ESP_OK; }
    const int16_t *samples = data;
    const size_t frames = len / (channels * sizeof(*samples));
    for (size_t i = 0; i < frames; i++) {
        if (channels == 2) assert(samples[i * 2] == samples[i * 2 + 1]);
        /* Writes acknowledge queuing, while backpressure advances playback. */
        if (queued == capacity) play_frame();
        pending[(head + queued) % capacity] = samples[i * channels];
        queued++;
    }
    *written = len;
    if (writes == cancel_write) cancelled = true;
    return ESP_OK;
}

void solar_os_stream_close(solar_os_stream_handle_t *stream)
{
    (void)stream;
    assert(opened);
    opened = false;
    closes++;
    /* Real teardown discards any samples still waiting for playback. */
    queued = 0;
}

#include "audio_tones.inc"

static void reset(uint32_t sample_rate, uint8_t channel_count)
{
    assert(!opened);
    opens = closes = writes = volume_sets = 0;
    selected_volume = 100;
    rate = sample_rate;
    channels = channel_count;
    native_block = 256;
    played_frames = head = queued = audible = runs = silence_frames = 0;
    capacity = rate * 128U / 1000U;
    assert(capacity <= sizeof(pending) / sizeof(pending[0]));
    memset(run_lengths, 0, sizeof(run_lengths));
    memset(gaps, 0, sizeof(gaps));
    in_tone = cancelled = cancel_drain = false;
    cancel_write = fail_write = short_write = 0;
}

int main(void)
{
    /* Reproduce immediate teardown: a 70 ms beep fits in the queue and is
     * discarded before the speaker receives any of it. */
    reset(16000, 1);
    solar_os_stream_handle_t stream = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_audio_device_info_t device;
    const solar_os_stream_open_options_t options = {
        .requested_audio.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    };
    assert(solar_os_audio_open_default(SOLAR_OS_STREAM_DIRECTION_SINK,
               "audio-tone", &options, &stream, &device) == ESP_OK);
    assert(audio_tone_write_segment(&stream, 1200, 70, NULL) == ESP_OK);
    solar_os_stream_close(&stream);
    assert(audible == 0);

    const uint32_t rates[] = {16000, 44100};
    const uint32_t durations[] = {1, 20, 70, 100, 1000};
    for (size_t r = 0; r < 2; r++) {
        for (uint8_t ch = 1; ch <= 2; ch++) {
            for (size_t d = 0; d < 5; d++) {
                reset(rates[r], ch);
                /* Include absent/oversized provider block hints. */
                native_block = d == 0 ? 0 : (d == 1 ? 1024 : 256);
                assert(audio_play_tone_locked(1200, durations[d],
                           SOLAR_OS_AUDIO_VOLUME_GLOBAL, NULL) == ESP_OK);
                assert(opens == 1 && closes == 1 && volume_sets == 0);
                assert(audible == (uint64_t)rate * durations[d] / 1000U);
                assert(runs == 1);
            }
        }
    }
    reset(16000, 2);
    const solar_os_audio_tone_step_t alarm[] = {
        {1200, 70, 45}, {1200, 70, 45}, {1200, 70, 45}, {1200, 70, 0},
    };
    assert(audio_play_tone_sequence_locked(alarm, 4, 80, &cancelled) == ESP_OK);
    assert(opens == 1 && closes == 1 && volume_sets == 1 && selected_volume == 80);
    assert(runs == 4 && audible == 4480);
    for (unsigned i = 0; i < 4; i++) {
        assert(run_lengths[i] == 1120);
        if (i) assert(gaps[i] == 720);
    }

    /* Every failure/cancellation closes the output exactly once. */
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        reset(16000, 1);
        if (scenario == 0) fail_write = 10;
        if (scenario == 1) short_write = 10;
        if (scenario == 2) cancel_write = 10;
        if (scenario == 3) cancel_drain = true;
        const esp_err_t expected[] = {
            ESP_FAIL, ESP_ERR_INVALID_SIZE, ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT,
        };
        assert(audio_play_tone_sequence_locked(alarm, 4, 100, &cancelled) ==
               expected[scenario]);
        assert(opens == 1 && closes == 1 && !opened);
    }
    reset(16000, 3);
    assert(audio_play_tone_locked(1200, 70, 100, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(opens == 1 && closes == 1);
    reset(16000, 1);
    assert(audio_play_tone_locked(0, 70, 100, NULL) == ESP_ERR_INVALID_ARG);
    assert(audio_play_tone_locked(1200, 0, 100, NULL) == ESP_ERR_INVALID_ARG);
    assert(audio_play_tone_locked(1200, 70, 101, NULL) == ESP_ERR_INVALID_ARG);
    cancelled = true;
    assert(audio_play_tone_locked(1200, 70, 100, &cancelled) == ESP_ERR_TIMEOUT);
    assert(opens == 0);
    puts("short tones, alarm pauses, buffered drain, volume and cleanup passed");
    return 0;
}
