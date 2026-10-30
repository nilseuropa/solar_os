#include <assert.h>
#include <string.h>
#include "solar_os_audio.h"

#define AUDIO_LEVEL_BUFFER_SAMPLES 512
static solar_os_stream_audio_format_t native;
static esp_err_t open_result, read_result;
static unsigned closes, mode;
static size_t frames_read;

esp_err_t solar_os_audio_open_default(solar_os_stream_direction_t direction,
    const char *owner, const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *handle, solar_os_audio_device_info_t *device) {
    (void)device;
    assert(direction == SOLAR_OS_STREAM_DIRECTION_SOURCE && !strcmp(owner, "audio.mic"));
    assert(options->requested_audio.bits_per_sample == 16);
    handle->audio = native; return open_result;
}
esp_err_t solar_os_stream_read(solar_os_stream_handle_t *handle, void *data,
    size_t len, uint32_t timeout, size_t *received) {
    assert(timeout == 1000);
    const unsigned channels = handle->audio.channels;
    int16_t *samples = data;
    for (size_t i = 0; i < len / 2; i++) samples[i] = i % channels ? -20000 : 10000;
    *received = mode == 1 ? 0 : mode == 2 ? 1 : mode == 3 ? len + 2 : len;
    frames_read += len / (2 * channels);
    return read_result;
}
void solar_os_stream_close(solar_os_stream_handle_t *handle) { (void)handle; closes++; }

#include "audio_capture_level.inc"

int main(void) {
    native = (solar_os_stream_audio_format_t){.sample_format=SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate=16000, .channels=1, .bits_per_sample=16};
    solar_os_audio_level_t level;
    assert(audio_measure_capture_stream(100, -1, &level) == ESP_OK);
    assert(level.samples == 1600 && frames_read == 1600 && closes == 1);
    assert(level.peak_percent == 30 && level.average_percent == 30);
    native.channels = 2;
    assert(audio_measure_capture_stream(100, -1, &level) == ESP_OK);
    assert(level.samples == 3200 && level.peak_percent == 61 && level.average_percent == 45);
    assert(audio_measure_capture_stream(100, 0, &level) == ESP_OK && level.samples == 1600 && level.average_percent == 30);
    assert(audio_measure_capture_stream(100, 1, &level) == ESP_OK && level.samples == 1600 && level.average_percent == 61);
    unsigned before = closes;
    assert(audio_measure_capture_stream(100, 2, &level) == ESP_ERR_INVALID_ARG && closes == before + 1);
    for (mode = 1; mode <= 3; mode++) {
        before = closes;
        assert(audio_measure_capture_stream(100, -1, &level) == ESP_ERR_INVALID_SIZE && closes == before + 1);
    }
    mode = 0; read_result = ESP_ERR_TIMEOUT; before = closes;
    assert(audio_measure_capture_stream(100, -1, &level) == ESP_ERR_TIMEOUT && closes == before + 1);
    read_result = ESP_OK; open_result = ESP_ERR_INVALID_STATE; before = closes;
    assert(audio_measure_capture_stream(100, -1, &level) == ESP_ERR_INVALID_STATE && closes == before);
    open_result = ESP_OK; native.channels = 0;
    assert(audio_measure_capture_stream(100, -1, &level) == ESP_ERR_NOT_SUPPORTED && closes == before + 1);
    before = closes;
    assert(audio_measure_capture_stream(0, -1, &level) == ESP_ERR_INVALID_ARG && closes == before);
    return 0;
}
