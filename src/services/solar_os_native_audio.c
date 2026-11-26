#include "solar_os_native_audio.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_audio_abi.h"
#include "solar_os_native_audio_decoder_abi.h"
#include "solar_os_native_audio_pcm_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO || SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
#include "solar_os.h"
#include "solar_os_native_handles.h"
#endif

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_memory.h"

static bool audio_cancel(void *user)
{
    const solar_os_audio_file_options_v1_t *options = user;
    return options->should_cancel(options->user);
}

static bool audio_pause(void *user)
{
    const solar_os_audio_file_options_v1_t *options = user;
    return options->should_pause(options->user);
}

static bool audio_monitor_enabled(void *user)
{
    const solar_os_audio_file_options_v1_t *options = user;
    return options->should_monitor(options->user);
}

static void audio_progress(const solar_os_audio_wav_progress_t *progress, void *user)
{
    const solar_os_audio_file_options_v1_t *options = user;
    options->progress(progress, options->user);
}

static void audio_samples(const int16_t *samples, size_t count, uint8_t channels, void *user)
{
    const solar_os_audio_file_options_v1_t *options = user;
    options->samples(samples, count, channels, options->user);
}

static void audio_device(bool capture, const solar_os_audio_device_info_t *device, void *user)
{
    const solar_os_audio_file_options_v1_t *options = user;
    options->device(capture, device->id, device->capture_stream, device->playback_stream, options->user);
}

static const solar_os_audio_wav_options_t *audio_options(
    const solar_os_audio_file_options_v1_t *options, solar_os_audio_wav_options_t *out)
{
    if (!options) return NULL;
    *out = (solar_os_audio_wav_options_t){
        .owner = options->owner,
        .capture_stream = options->capture_stream,
        .record_format = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .sample_rate = options->sample_rate,
            .channels = options->channels,
        },
        .monitor = options->monitor,
        .monitor_volume = options->monitor_volume,
        .should_monitor = options->should_monitor ? audio_monitor_enabled : NULL,
        .should_cancel = options->should_cancel ? audio_cancel : NULL,
        .should_pause = options->should_pause ? audio_pause : NULL,
        .progress = options->progress ? audio_progress : NULL,
        .samples = options->samples ? audio_samples : NULL,
        .device = options->device ? audio_device : NULL,
        .user = (void *)options,
        .progress_interval_ms = options->progress_interval_ms,
        .start_ms = options->start_ms,
    };
    return out;
}

static int audio_probe(const char *path, uint32_t format, uint32_t *detected,
                       solar_os_audio_file_info_v1_t *info)
{
    if (info) *info = (solar_os_audio_file_info_v1_t){0};
    return solar_os_native_result(solar_os_audio_file_probe(path, format, detected, info));
}

static int audio_play(const char *path, uint32_t format, uint8_t volume,
                      const solar_os_audio_file_options_v1_t *options,
                      solar_os_audio_file_info_v1_t *info)
{
    solar_os_audio_wav_options_t request;
    if (info) *info = (solar_os_audio_file_info_v1_t){0};
    return solar_os_native_result(
        solar_os_audio_file_play(path, format, volume, audio_options(options, &request), info));
}

static int audio_record(const char *path, uint32_t duration_ms,
                        const solar_os_audio_file_options_v1_t *options,
                        solar_os_audio_file_info_v1_t *info)
{
    solar_os_audio_wav_options_t request;
    if (info) *info = (solar_os_audio_file_info_v1_t){0};
    return solar_os_native_result(
        solar_os_audio_record_wav(path, duration_ms, audio_options(options, &request), info));
}

static int audio_monitor(uint8_t volume, const solar_os_audio_file_options_v1_t *options,
                         solar_os_audio_file_info_v1_t *info)
{
    solar_os_audio_wav_options_t request;
    if (info) *info = (solar_os_audio_file_info_v1_t){0};
    return solar_os_native_result(
        solar_os_audio_monitor_stream(volume, audio_options(options, &request), info));
}

static int audio_pcm_destroy(void *state)
{
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int audio_pcm_open(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_audio_s16_converter_t *state = solar_os_memory_calloc(
        1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.audio.pcm");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_AUDIO_PCM;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(kind, state, audio_pcm_destroy, owner, out) :
        solar_os_native_handle_create(kind, state, audio_pcm_destroy, out);
    if (result) solar_os_memory_free(state);
    return result;
}

static int audio_pcm_reset(solar_os_native_handle_t id)
{
    solar_os_audio_s16_converter_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_AUDIO_PCM);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_audio_s16_converter_reset(state);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int audio_pcm_format(const solar_os_native_stream_format_v1_t *in, solar_os_stream_audio_format_t *out)
{
    if (!in || in->audio_format != SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE ||
        !in->sample_rate || !in->channels || in->channels > 2 ||
        (in->bits_per_sample && in->bits_per_sample != 16))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_stream_audio_format_t){
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = in->sample_rate,
        .channels = in->channels,
        .bits_per_sample = 16,
    };
    return SOLAR_OS_NATIVE_OK;
}

static int audio_pcm_convert(solar_os_native_handle_t id, const int16_t *input, size_t frames,
                             const solar_os_native_stream_format_v1_t *source,
                             const solar_os_native_stream_format_v1_t *target,
                             int16_t *output, size_t capacity, size_t *count, bool *done)
{
    if (!count || !done) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0;
    *done = false;
    solar_os_stream_audio_format_t from, to;
    int result = audio_pcm_format(source, &from);
    if (!result) result = audio_pcm_format(target, &to);
    if (result) return result;

    solar_os_audio_s16_converter_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_AUDIO_PCM);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    result = solar_os_native_result(
        solar_os_audio_s16_convert(state, input, frames, &from, &to, output, capacity, count, done));
    solar_os_native_handle_release(id);
    return result;
}

static int audio_pcm_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_AUDIO_PCM);
}

static const solar_os_native_audio_pcm_api_v1_t audio_pcm_api = {
    .abi_version = SOLAR_OS_NATIVE_AUDIO_PCM_ABI,
    .struct_size = sizeof(audio_pcm_api),
    .open = audio_pcm_open,
    .reset = audio_pcm_reset,
    .convert = audio_pcm_convert,
    .close = audio_pcm_close,
};

static int audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume)
{
    return solar_os_native_result(solar_os_audio_play_tone(frequency_hz, duration_ms, volume));
}

static int audio_enqueue_tone(const solar_os_audio_tone_request_t *request, uint32_t *request_id)
{
    if (request_id) *request_id = 0;
    return solar_os_native_result(solar_os_audio_tone_enqueue(request, request_id));
}

static int audio_cancel_tone(uint32_t request_id)
{
    return solar_os_native_result(solar_os_audio_tone_cancel(request_id));
}

static int audio_prepare_board(void)
{
    return solar_os_native_result(solar_os_audio_init());
}

static int audio_loopback(uint32_t duration_ms, uint8_t volume)
{
    return solar_os_native_result(solar_os_audio_loopback(duration_ms, volume));
}

static const solar_os_native_audio_api_v1_t audio_api = {
    .abi_version = SOLAR_OS_NATIVE_AUDIO_ABI,
    .struct_size = sizeof(audio_api),
    .capabilities = SOLAR_OS_AUDIO_FILE_CAP_WAV
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
        | SOLAR_OS_AUDIO_FILE_CAP_MP3
#endif
        ,
    .probe = audio_probe,
    .play = audio_play,
    .record = audio_record,
    .monitor = audio_monitor,
    .play_tone = audio_play_tone, .enqueue_tone = audio_enqueue_tone,
    .cancel_tone = audio_cancel_tone, .tone_status = solar_os_audio_tone_queue_get_status,
    .prepare_board = audio_prepare_board, .release_idle_board = solar_os_audio_deinit, .loopback = audio_loopback,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
#include "solar_os_audio_codec.h"

static int decoder_limits(uint32_t format, solar_os_audio_decoder_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_audio_decoder_limits_v1_t){0};
    if (format != SOLAR_OS_AUDIO_FORMAT_MP3) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    *out = (solar_os_audio_decoder_limits_v1_t){
        .max_samples = SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES,
        .stream_window_bytes = SOLAR_OS_AUDIO_MP3_STREAM_WINDOW_BYTES,
    };
    return SOLAR_OS_NATIVE_OK;
}

static solar_os_native_stream_format_v1_t decoder_pcm_format(const solar_os_stream_audio_format_t *format)
{
    return (solar_os_native_stream_format_v1_t){
        .audio_format = SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE,
        .sample_rate = format->sample_rate,
        .channels = format->channels,
        .bits_per_sample = format->bits_per_sample,
        .frames_per_block = format->frames_per_block,
    };
}

static int decoder_probe(uint32_t format, const void *input, size_t input_len,
                          solar_os_native_stream_format_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_stream_format_v1_t){0};
    if (format != SOLAR_OS_AUDIO_FORMAT_MP3) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    solar_os_stream_audio_format_t pcm_format;
    const esp_err_t error = solar_os_audio_mp3_probe(input, input_len, &pcm_format);
    if (error == ESP_OK) *out = decoder_pcm_format(&pcm_format);
    return solar_os_native_result(error);
}

static int decoder_destroy(void *state)
{
    solar_os_audio_mp3_decoder_destroy(state);
    return SOLAR_OS_NATIVE_OK;
}

static int decoder_open(uint32_t format, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (format != SOLAR_OS_AUDIO_FORMAT_MP3) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    solar_os_audio_mp3_decoder_t *state = NULL;
    const esp_err_t error = solar_os_audio_mp3_decoder_create(&state);
    if (error != ESP_OK) return solar_os_native_result(error);
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_AUDIO_DECODER;
    const int result = owner ? solar_os_native_handle_create_owned(kind, state, decoder_destroy, owner, out) :
        solar_os_native_handle_create(kind, state, decoder_destroy, out);
    if (result) solar_os_audio_mp3_decoder_destroy(state);
    return result;
}

static int decoder_reset(solar_os_native_handle_t id)
{
    solar_os_audio_mp3_decoder_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_AUDIO_DECODER);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_audio_mp3_decoder_reset(state);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int decoder_process(solar_os_native_handle_t id, const void *input, size_t input_len,
                           int16_t *pcm, size_t capacity, solar_os_audio_decoder_frame_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_audio_decoder_frame_v1_t){0};
    if ((!input && input_len) || (!pcm && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (pcm && capacity < SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES)
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;

    solar_os_audio_mp3_decoder_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_AUDIO_DECODER);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (!input_len) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_OK;
    }
    solar_os_audio_decoded_frame_t frame;
    size_t consumed;
    const esp_err_t error = pcm ?
        solar_os_audio_mp3_decode(state, input, input_len, &consumed, pcm, capacity, &frame) :
        solar_os_audio_mp3_scan(state, input, input_len, &consumed, &frame);
    if (error == ESP_OK) {
        *out = (solar_os_audio_decoder_frame_v1_t){
            .format = decoder_pcm_format(&frame.format),
            .consumed_bytes = consumed,
            .source_frames = frame.source_frames,
            .decoded_frames = pcm ? frame.frames : 0,
            .decoded_samples = pcm ? frame.samples : 0,
            .payload_bytes = frame.main_data_bytes,
        };
    }
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int decoder_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_AUDIO_DECODER);
}

static const solar_os_native_audio_decoder_api_v1_t audio_decoder_api = {
    .abi_version = SOLAR_OS_NATIVE_AUDIO_DECODER_ABI,
    .struct_size = sizeof(audio_decoder_api),
    .limits = decoder_limits,
    .probe = decoder_probe,
    .open = decoder_open,
    .reset = decoder_reset,
    .process = decoder_process,
    .close = decoder_close,
};
#endif

const void *solar_os_native_audio_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_audio_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
