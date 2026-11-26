#include "solar_os_native_stream.h"
#include "solar_os_native_stream_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_STREAMS
#include "solar_os_stream.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif
typedef struct {
    solar_os_stream_handle_t handle;
    solar_os_stream_video_frame_t frame;
    bool open, ready, leased;
} native_stream_t;
static int type_out(solar_os_stream_type_t type)
{
    switch (type) {
    case SOLAR_OS_STREAM_TYPE_SCALAR: return SOLAR_OS_NATIVE_STREAM_SCALAR;
    case SOLAR_OS_STREAM_TYPE_EVENT: return SOLAR_OS_NATIVE_STREAM_EVENT;
    case SOLAR_OS_STREAM_TYPE_BYTES: return SOLAR_OS_NATIVE_STREAM_BYTES;
    case SOLAR_OS_STREAM_TYPE_AUDIO: return SOLAR_OS_NATIVE_STREAM_AUDIO;
    case SOLAR_OS_STREAM_TYPE_VIDEO: return SOLAR_OS_NATIVE_STREAM_VIDEO;
    default: return -1;
    }
}
static int direction_out(solar_os_stream_direction_t direction)
{
    switch (direction) {
    case SOLAR_OS_STREAM_DIRECTION_SOURCE: return SOLAR_OS_NATIVE_STREAM_SOURCE;
    case SOLAR_OS_STREAM_DIRECTION_SINK: return SOLAR_OS_NATIVE_STREAM_SINK;
    case SOLAR_OS_STREAM_DIRECTION_DUPLEX: return SOLAR_OS_NATIVE_STREAM_DUPLEX;
    default: return -1;
    }
}
static int info_out(const solar_os_stream_info_t *in, solar_os_native_stream_info_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    int type = type_out(in->type), direction = direction_out(in->direction), sharing;
    switch (in->sharing) {
    case SOLAR_OS_STREAM_SHARING_SHARED: sharing = SOLAR_OS_NATIVE_STREAM_SHARED; break;
    case SOLAR_OS_STREAM_SHARING_EXCLUSIVE: sharing = SOLAR_OS_NATIVE_STREAM_EXCLUSIVE; break;
    case SOLAR_OS_STREAM_SHARING_FANOUT: sharing = SOLAR_OS_NATIVE_STREAM_FANOUT; break;
    case SOLAR_OS_STREAM_SHARING_MIXED: sharing = SOLAR_OS_NATIVE_STREAM_MIXED; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    if (type < 0 || direction < 0) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    out->type = type; out->direction = direction; out->sharing = sharing;
#define COPY(field) do { _Static_assert(sizeof(out->field) == sizeof(in->field), "stream field capacity"); memcpy(out->field, in->field, sizeof(out->field)); } while (0)
    COPY(id); COPY(provider); COPY(device); COPY(unit); COPY(format); COPY(summary); COPY(owner);
#undef COPY
    out->active_handles = in->active_handles; out->read_units = in->read_units; out->written_units = in->written_units;
    out->overruns = in->overruns; out->underruns = in->underruns; out->has_scalar_range = in->has_scalar_range;
    out->scalar_minimum = in->scalar_minimum; out->scalar_maximum = in->scalar_maximum;
    if (in->type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        if (in->audio.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        out->media.audio_format = SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE; out->media.sample_rate = in->audio.sample_rate;
        out->media.channels = in->audio.channels; out->media.bits_per_sample = in->audio.bits_per_sample; out->media.frames_per_block = in->audio.frames_per_block;
    } else if (in->type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        if (in->video.codec != SOLAR_OS_STREAM_VIDEO_JPEG) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        out->media.video_codec = SOLAR_OS_NATIVE_STREAM_VIDEO_JPEG; out->media.width = in->video.width;
        out->media.height = in->video.height; out->media.jpeg_quality = in->video.jpeg_quality;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int stream_info(size_t index, solar_os_native_stream_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_stream_info_t value = {0};
    return solar_os_stream_get(index, &value) ? info_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int stream_find(const char *id, solar_os_native_stream_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_stream_info_t value = {0};
    int result = solar_os_native_result(solar_os_stream_get_info(id, &value)); return result ? result : info_out(&value, out);
}
static int stream_set_unit(const char *id, const char *unit) { return solar_os_native_result(solar_os_stream_set_unit(id, unit)); }
static int stream_destroy(void *value)
{
    native_stream_t *state = value;
    if (state->leased) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->open) { int result = solar_os_native_result(solar_os_stream_close_ex(&state->handle)); if (result) return result; }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int stream_options(const solar_os_native_stream_options_v1_t *in,
    solar_os_stream_type_t type, solar_os_stream_open_options_t *options)
{
    *options = (solar_os_stream_open_options_t){0};
    if (!in) return SOLAR_OS_NATIVE_OK;
    switch (in->direction) {
    case SOLAR_OS_NATIVE_STREAM_SOURCE: options->direction = SOLAR_OS_STREAM_DIRECTION_SOURCE; break;
    case SOLAR_OS_NATIVE_STREAM_SINK: options->direction = SOLAR_OS_STREAM_DIRECTION_SINK; break;
    case SOLAR_OS_NATIVE_STREAM_DUPLEX: options->direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    options->timeout_ms = in->timeout_ms;
    if (type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        if (in->format.audio_format != SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE || in->format.channels > UINT8_MAX ||
            in->format.bits_per_sample > UINT8_MAX || in->format.frames_per_block > UINT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        options->requested_audio = (solar_os_stream_audio_format_t){SOLAR_OS_STREAM_AUDIO_S16_LE, in->format.sample_rate,
            in->format.channels, in->format.bits_per_sample, in->format.frames_per_block};
    } else if (type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        if (in->format.video_codec != SOLAR_OS_NATIVE_STREAM_VIDEO_JPEG || in->format.width > UINT16_MAX ||
            in->format.height > UINT16_MAX || in->format.jpeg_quality > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        options->requested_video = (solar_os_stream_video_format_t){SOLAR_OS_STREAM_VIDEO_JPEG, in->format.width, in->format.height, in->format.jpeg_quality};
    }
    return SOLAR_OS_NATIVE_OK;
}

static int stream_create(native_stream_t **value, solar_os_native_handle_t *out)
{
    native_stream_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.stream");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    const int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_STREAM, state, stream_destroy, out);
    if (result) {
        stream_destroy(state);
        return result;
    }
    *value = state;
    return SOLAR_OS_NATIVE_OK;
}

static int stream_open_result(native_stream_t *state, solar_os_native_handle_t *out, esp_err_t error)
{
    state->open = solar_os_stream_handle_valid(&state->handle);
    state->ready = error == ESP_OK && state->open;
    if (state->ready) return SOLAR_OS_NATIVE_OK;
    if (!solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_STREAM)) *out = 0;
    return solar_os_native_result(error == ESP_OK ? ESP_ERR_INVALID_STATE : error);
}

static int stream_open(const char *id, const char *owner, const solar_os_native_stream_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_stream_info_t info = {0};
    int result = solar_os_native_result(solar_os_stream_get_info(id, &info));
    if (result) return result;
    solar_os_stream_open_options_t options;
    result = stream_options(in, info.type, &options);
    if (result) return result;
    native_stream_t *state;
    result = stream_create(&state, out);
    if (result) return result;
    return stream_open_result(state, out, solar_os_stream_open_typed(id, owner, info.type, in ? &options : NULL, &state->handle));
}

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
int solar_os_native_stream_open_default_audio(const char *owner,
    const solar_os_native_stream_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !owner || !*owner || in->direction > SOLAR_OS_NATIVE_STREAM_SINK)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_stream_open_options_t options;
    int result = stream_options(in, SOLAR_OS_STREAM_TYPE_AUDIO, &options);
    if (result) return result;
    native_stream_t *state;
    result = stream_create(&state, out);
    if (result) return result;
    return stream_open_result(state, out,
        solar_os_audio_open_default(options.direction, owner, &options, &state->handle, NULL));
}
#endif

static native_stream_t *stream_acquire(solar_os_native_handle_t id)
{
    native_stream_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_STREAM);
    if (state && !state->ready) {
        solar_os_native_handle_release(id);
        return NULL;
    }
    return state;
}

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
int solar_os_native_stream_capture_audio(solar_os_native_handle_t id, size_t frames,
    int16_t *samples, size_t sample_capacity)
{
    native_stream_t *state = stream_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(
        solar_os_audio_capture_frames(&state->handle, frames, samples, sample_capacity));
    solar_os_native_handle_release(id);
    return result;
}
int solar_os_native_stream_measure_audio(solar_os_native_handle_t id, uint32_t duration_ms,
    int channel, solar_os_audio_level_t *level)
{
    native_stream_t *state = stream_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(
        solar_os_audio_measure_stream(&state->handle, duration_ms, channel, level));
    solar_os_native_handle_release(id);
    return result;
}

#endif

static int stream_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_STREAM); }
static bool stream_valid(solar_os_native_handle_t id)
{
    native_stream_t *state = stream_acquire(id); if (!state) return false;
    bool valid = solar_os_stream_handle_valid(&state->handle); solar_os_native_handle_release(id); return valid;
}
static int stream_read(solar_os_native_handle_t id, void *bytes, size_t capacity, uint32_t timeout, size_t *size)
{
    if (!size || (!bytes && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size = 0; native_stream_t *state = stream_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_stream_read(&state->handle, bytes, capacity, timeout, size));
    solar_os_native_handle_release(id); return result;
}
static int stream_write(solar_os_native_handle_t id, const void *bytes, size_t size, uint32_t timeout, size_t *written)
{
    if (!written || (!bytes && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *written = 0; native_stream_t *state = stream_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_stream_write(&state->handle, bytes, size, timeout, written));
    solar_os_native_handle_release(id); return result;
}
static solar_os_stream_read_options_t read_options(const solar_os_native_stream_read_options_v1_t *in)
{ return in ? (solar_os_stream_read_options_t){in->window_ms, in->timeout_ms} : (solar_os_stream_read_options_t){0}; }
static int stream_read_scalar(solar_os_native_handle_t id, const solar_os_native_stream_read_options_v1_t *in, float *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_stream_t *state = stream_acquire(id); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_stream_read_options_t options = read_options(in);
    int result = solar_os_native_result(solar_os_stream_read_scalar(&state->handle, in ? &options : NULL, out));
    solar_os_native_handle_release(id); return result;
}
static int stream_read_sample(solar_os_native_handle_t id, const solar_os_native_stream_read_options_v1_t *in, solar_os_native_stream_sample_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); native_stream_t *state = stream_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_stream_read_options_t options = read_options(in); solar_os_stream_scalar_sample_t value = {0};
    int result = solar_os_native_result(solar_os_stream_read_scalar_sample(&state->handle, in ? &options : NULL, &value));
    if (!result) *out = (solar_os_native_stream_sample_v1_t){value.value, value.uptime_ms};
    solar_os_native_handle_release(id); return result;
}
static int stream_observe_sample(const char *id, const solar_os_native_stream_read_options_v1_t *in, solar_os_native_stream_sample_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_stream_read_options_t options = read_options(in); solar_os_stream_scalar_sample_t value = {0};
    int result = solar_os_native_result(solar_os_stream_observe_scalar_sample(id, in ? &options : NULL, &value));
    if (!result) *out = (solar_os_native_stream_sample_v1_t){value.value, value.uptime_ms};
    return result;
}
static int stream_scalar_command(solar_os_native_handle_t id, float value, uint64_t deadline, uint32_t timeout)
{
    native_stream_t *state = stream_acquire(id); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    if (state->handle.type == SOLAR_OS_STREAM_TYPE_SCALAR) {
        solar_os_stream_scalar_command_t command = {value, deadline}; size_t written = 0;
        result = solar_os_native_result(solar_os_stream_write(&state->handle, &command, sizeof(command), timeout, &written));
        if (!result && written != sizeof(command)) result = SOLAR_OS_NATIVE_ERROR_IO;
    }
    solar_os_native_handle_release(id); return result;
}
static int stream_csv_header(const char *id, char *header, size_t capacity)
{
    if (!header || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *header = 0; solar_os_stream_info_t info = {0}; int result = solar_os_native_result(solar_os_stream_get_info(id, &info));
    return result ? result : solar_os_native_result(solar_os_stream_csv_header(&info, header, capacity));
}
static int stream_read_csv(solar_os_native_handle_t id, const solar_os_native_stream_read_options_v1_t *in, solar_os_native_stream_csv_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); native_stream_t *state = stream_acquire(id); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_stream_read_options_t options = read_options(in); solar_os_stream_csv_record_t value = {0};
    int result = solar_os_native_result(solar_os_stream_read_csv(&state->handle, in ? &options : NULL, &value));
    if (!result) {
        out->time_ms = value.time_ms; out->uptime_ms = value.uptime_ms; out->has_data = value.has_data; out->time_valid = value.time_valid;
        _Static_assert(sizeof(out->line) == sizeof(value.line), "CSV line capacity");
        _Static_assert(sizeof(out->change_key) == sizeof(value.change_key), "CSV key capacity");
        memcpy(out->line, value.line, sizeof(out->line)); memcpy(out->change_key, value.change_key, sizeof(out->change_key));
    }
    solar_os_native_handle_release(id); return result;
}
static int stream_frame_acquire(solar_os_native_handle_t id, solar_os_native_stream_frame_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); native_stream_t *state = stream_acquire(id); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_BUSY;
    if (!state->leased) {
        result = solar_os_native_result(solar_os_stream_acquire_frame(&state->handle, &state->frame));
        if (!result) { state->leased = true; *out = (solar_os_native_stream_frame_v1_t){state->frame.data, state->frame.length, state->frame.width, state->frame.height, state->frame.timestamp_us}; }
    }
    solar_os_native_handle_release(id); return result;
}
static int stream_frame_release(solar_os_native_handle_t id)
{
    native_stream_t *state = stream_acquire(id); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (state->leased) { result = solar_os_native_result(solar_os_stream_release_frame(&state->handle, &state->frame)); if (!result) state->leased = false; }
    solar_os_native_handle_release(id); return result;
}
static int stream_handle_info(solar_os_native_handle_t id, solar_os_native_stream_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_stream_info_v1_t){0};
    native_stream_t *state = stream_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_stream_info_t info;
    const esp_err_t error = solar_os_stream_handle_info(&state->handle, &info);
    solar_os_native_handle_release(id);
    return error == ESP_OK ? info_out(&info, out) : solar_os_native_result(error);
}

static const char *stream_type_name(uint32_t type)
{
    return solar_os_stream_type_name((solar_os_stream_type_t)type);
}

static const char *stream_direction_name(uint32_t direction)
{
    return solar_os_stream_direction_name((solar_os_stream_direction_t)direction);
}

static const char *stream_sharing_name(uint32_t sharing)
{
    return solar_os_stream_sharing_name((solar_os_stream_sharing_t)sharing);
}

static const char *stream_audio_format_name(uint32_t format)
{
    return solar_os_stream_audio_sample_format_name((solar_os_stream_audio_sample_format_t)format);
}

static const solar_os_native_stream_api_v1_t stream_api = {
    .abi_version = SOLAR_OS_NATIVE_STREAM_ABI, .struct_size = sizeof(stream_api), .count = solar_os_stream_count, .info = stream_info,
    .find = stream_find, .set_unit = stream_set_unit, .open = stream_open, .close = stream_close, .valid = stream_valid,
    .read = stream_read, .write = stream_write, .read_scalar = stream_read_scalar, .read_sample = stream_read_sample, .observe_sample = stream_observe_sample,
    .scalar_command = stream_scalar_command, .csv_header = stream_csv_header, .read_csv = stream_read_csv,
    .frame_acquire = stream_frame_acquire, .frame_release = stream_frame_release,
    .handle_info = stream_handle_info,
    .type_name = stream_type_name,
    .direction_name = stream_direction_name,
    .sharing_name = stream_sharing_name,
    .audio_format_name = stream_audio_format_name,
};
#endif
const void *solar_os_native_stream_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_stream_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
