#include "solar_os_native_stream_provider.h"

#include <stdatomic.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_stream_provider_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_STREAMS
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_handles.h"
#include "solar_os_stream.h"

typedef struct {
    char id[SOLAR_OS_STREAM_ID_MAX];
    solar_os_native_stream_format_v1_t format;
    solar_os_native_stream_provider_callbacks_v1_t callbacks;
} native_stream_endpoint_t;

typedef struct {
    solar_os_native_handle_t library;
    size_t count;
    atomic_bool opening;
    bool registered;
    native_stream_endpoint_t endpoints[];
} native_stream_provider_t;

static bool audio_format_in(const solar_os_native_stream_format_v1_t *format,
                            solar_os_stream_audio_format_t *out)
{
    if (format->audio_format != SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE || !format->sample_rate ||
        !format->channels || format->channels > UINT8_MAX || format->bits_per_sample != 16 ||
        format->frames_per_block > UINT16_MAX) return false;
    *out = (solar_os_stream_audio_format_t){
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = format->sample_rate,
        .channels = format->channels,
        .bits_per_sample = format->bits_per_sample,
        .frames_per_block = format->frames_per_block,
    };
    return true;
}

static bool video_format_in(const solar_os_native_stream_format_v1_t *format,
                            solar_os_stream_video_format_t *out)
{
    if (format->video_codec != SOLAR_OS_NATIVE_STREAM_VIDEO_JPEG ||
        !format->width || format->width > UINT16_MAX || !format->height || format->height > UINT16_MAX ||
        format->jpeg_quality > UINT8_MAX) return false;
    *out = (solar_os_stream_video_format_t){
        .codec = SOLAR_OS_STREAM_VIDEO_JPEG,
        .width = format->width,
        .height = format->height,
        .jpeg_quality = format->jpeg_quality,
    };
    return true;
}

static solar_os_native_stream_options_v1_t request_out(solar_os_stream_type_t type,
                                                        const solar_os_stream_open_options_t *in)
{
    solar_os_native_stream_options_v1_t request = {
        .direction = in->direction, .timeout_ms = in->timeout_ms,
    };
    if (type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        request.format = (solar_os_native_stream_format_v1_t){
            .audio_format = SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE,
            .sample_rate = in->requested_audio.sample_rate,
            .channels = in->requested_audio.channels,
            .bits_per_sample = in->requested_audio.bits_per_sample,
            .frames_per_block = in->requested_audio.frames_per_block,
        };
    } else if (type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        request.format = (solar_os_native_stream_format_v1_t){
            .video_codec = SOLAR_OS_NATIVE_STREAM_VIDEO_JPEG,
            .width = in->requested_video.width,
            .height = in->requested_video.height,
            .jpeg_quality = in->requested_video.jpeg_quality,
        };
    }
    return request;
}

static esp_err_t endpoint_open(void *opaque, const char *owner,
                              const solar_os_stream_open_options_t *options, solar_os_stream_handle_t *handle)
{
    native_stream_endpoint_t *endpoint = opaque;
    const solar_os_native_stream_provider_callbacks_v1_t *ops = &endpoint->callbacks;
    const solar_os_native_stream_options_v1_t request = request_out(handle->type, options);
    solar_os_native_stream_format_v1_t selected = endpoint->format;
    if (ops->negotiate) {
        const int result = ops->negotiate(ops->user, &request, &selected);
        if (result) return solar_os_native_callback_result(result);
    }
    if ((handle->type == SOLAR_OS_STREAM_TYPE_AUDIO && !audio_format_in(&selected, &handle->audio)) ||
        (handle->type == SOLAR_OS_STREAM_TYPE_VIDEO && !video_format_in(&selected, &handle->video)))
        return ESP_ERR_INVALID_ARG;
    if (!ops->open) return ESP_OK;
    return solar_os_native_callback_result(ops->open(ops->user, owner, &request, &selected, &handle->context));
}

static esp_err_t endpoint_close(void *opaque, solar_os_stream_handle_t *handle)
{
    native_stream_endpoint_t *endpoint = opaque;
    return solar_os_native_callback_result(endpoint->callbacks.close(endpoint->callbacks.user, handle->context));
}

static esp_err_t endpoint_read(void *opaque, solar_os_stream_handle_t *handle,
                              void *bytes, size_t capacity, uint32_t timeout, size_t *size)
{
    native_stream_endpoint_t *endpoint = opaque;
    const int result = endpoint->callbacks.read(endpoint->callbacks.user, handle->context,
                                                bytes, capacity, timeout, size);
    if (!result && *size > capacity) {
        *size = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_native_callback_result(result);
}

static esp_err_t endpoint_write(void *opaque, solar_os_stream_handle_t *handle,
                               const void *bytes, size_t size, uint32_t timeout, size_t *written)
{
    native_stream_endpoint_t *endpoint = opaque;
    const int result = endpoint->callbacks.write(endpoint->callbacks.user, handle->context,
                                                 bytes, size, timeout, written);
    if (!result && *written > size) {
        *written = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_native_callback_result(result);
}

static solar_os_native_stream_read_options_v1_t read_request(const solar_os_stream_read_options_t *in)
{
    return in ? (solar_os_native_stream_read_options_v1_t){in->window_ms, in->timeout_ms} :
        (solar_os_native_stream_read_options_v1_t){0};
}

static esp_err_t endpoint_read_scalar(void *opaque, const solar_os_stream_read_options_t *options, float *out)
{
    native_stream_endpoint_t *endpoint = opaque;
    const solar_os_native_stream_read_options_v1_t request = read_request(options);
    return solar_os_native_callback_result(endpoint->callbacks.read_scalar(endpoint->callbacks.user, &request, out));
}

static esp_err_t endpoint_sample(native_stream_endpoint_t *endpoint, const solar_os_stream_read_options_t *options,
                                 solar_os_stream_scalar_sample_t *out, bool observe)
{
    const solar_os_native_stream_read_options_v1_t request = read_request(options);
    solar_os_native_stream_sample_v1_t sample = {0};
    const int result = observe ?
        endpoint->callbacks.observe_sample(endpoint->callbacks.user, &request, &sample) :
        endpoint->callbacks.read_sample(endpoint->callbacks.user, &request, &sample);
    if (!result) *out = (solar_os_stream_scalar_sample_t){.value = sample.value, .uptime_ms = sample.uptime_ms};
    return solar_os_native_callback_result(result);
}

static esp_err_t endpoint_read_sample(void *opaque, const solar_os_stream_read_options_t *options,
                                      solar_os_stream_scalar_sample_t *out)
{
    return endpoint_sample(opaque, options, out, false);
}

static esp_err_t endpoint_observe_sample(void *opaque, const solar_os_stream_read_options_t *options,
                                         solar_os_stream_scalar_sample_t *out)
{
    return endpoint_sample(opaque, options, out, true);
}

static esp_err_t endpoint_csv_header(void *opaque, char *header, size_t capacity)
{
    native_stream_endpoint_t *endpoint = opaque;
    return solar_os_native_callback_result(endpoint->callbacks.csv_header(endpoint->callbacks.user, header, capacity));
}

static esp_err_t endpoint_read_csv(void *opaque, solar_os_stream_handle_t *handle,
                                   const solar_os_stream_read_options_t *options, solar_os_stream_csv_record_t *out)
{
    native_stream_endpoint_t *endpoint = opaque;
    const solar_os_native_stream_read_options_v1_t request = read_request(options);
    solar_os_native_stream_csv_v1_t row = {0};
    const int result = endpoint->callbacks.read_csv(endpoint->callbacks.user, handle->context, &request, &row);
    if (result) return solar_os_native_callback_result(result);
    if (strnlen(row.line, sizeof(row.line)) == sizeof(row.line) ||
        strnlen(row.change_key, sizeof(row.change_key)) == sizeof(row.change_key)) return ESP_ERR_INVALID_SIZE;
    *out = (solar_os_stream_csv_record_t){
        .has_data = row.has_data != 0, .time_valid = row.time_valid != 0,
        .time_ms = row.time_ms, .uptime_ms = row.uptime_ms,
    };
    strlcpy(out->line, row.line, sizeof(out->line));
    strlcpy(out->change_key, row.change_key, sizeof(out->change_key));
    return ESP_OK;
}

static esp_err_t endpoint_acquire_frame(void *opaque, solar_os_stream_handle_t *handle,
                                       solar_os_stream_video_frame_t *out)
{
    native_stream_endpoint_t *endpoint = opaque;
    solar_os_native_stream_provider_frame_v1_t frame = {0};
    const int result = endpoint->callbacks.acquire_frame(endpoint->callbacks.user, handle->context, &frame);
    if (!result) {
        *out = (solar_os_stream_video_frame_t){
            .data = frame.data, .length = frame.length,
            .width = frame.width, .height = frame.height, .timestamp_us = frame.timestamp_us,
        };
    }
    return solar_os_native_callback_result(result);
}

static esp_err_t endpoint_release_frame(void *opaque, solar_os_stream_handle_t *handle,
                                       solar_os_stream_video_frame_t *in)
{
    native_stream_endpoint_t *endpoint = opaque;
    const solar_os_native_stream_provider_frame_v1_t frame = {
        .data = in->data, .length = in->length,
        .width = in->width, .height = in->height, .timestamp_us = in->timestamp_us,
    };
    return solar_os_native_callback_result(
        endpoint->callbacks.release_frame(endpoint->callbacks.user, handle->context, &frame));
}

_Static_assert(SOLAR_OS_STREAM_TYPE_SCALAR == SOLAR_OS_NATIVE_STREAM_SCALAR &&
               SOLAR_OS_STREAM_TYPE_EVENT == SOLAR_OS_NATIVE_STREAM_EVENT &&
               SOLAR_OS_STREAM_TYPE_BYTES == SOLAR_OS_NATIVE_STREAM_BYTES &&
               SOLAR_OS_STREAM_TYPE_AUDIO == SOLAR_OS_NATIVE_STREAM_AUDIO &&
               SOLAR_OS_STREAM_TYPE_VIDEO == SOLAR_OS_NATIVE_STREAM_VIDEO, "stream types");
_Static_assert(SOLAR_OS_STREAM_DIRECTION_SOURCE == SOLAR_OS_NATIVE_STREAM_SOURCE &&
               SOLAR_OS_STREAM_DIRECTION_SINK == SOLAR_OS_NATIVE_STREAM_SINK &&
               SOLAR_OS_STREAM_DIRECTION_DUPLEX == SOLAR_OS_NATIVE_STREAM_DUPLEX, "stream directions");
_Static_assert(SOLAR_OS_STREAM_SHARING_SHARED == SOLAR_OS_NATIVE_STREAM_SHARED &&
               SOLAR_OS_STREAM_SHARING_EXCLUSIVE == SOLAR_OS_NATIVE_STREAM_EXCLUSIVE &&
               SOLAR_OS_STREAM_SHARING_FANOUT == SOLAR_OS_NATIVE_STREAM_FANOUT &&
               SOLAR_OS_STREAM_SHARING_MIXED == SOLAR_OS_NATIVE_STREAM_MIXED, "stream sharing");

static bool endpoint_prepare(native_stream_endpoint_t *state,
                             const solar_os_native_stream_provider_endpoint_v1_t *descriptor,
                             solar_os_stream_driver_t *driver)
{
    const solar_os_native_stream_info_v1_t *in = &descriptor->info;
    const solar_os_native_stream_provider_callbacks_v1_t *ops = &descriptor->callbacks;
    if (in->type > SOLAR_OS_NATIVE_STREAM_VIDEO || in->direction > SOLAR_OS_NATIVE_STREAM_DUPLEX ||
        in->sharing > SOLAR_OS_NATIVE_STREAM_MIXED) return false;

    *driver = (solar_os_stream_driver_t){
        .info = {
            .type = (solar_os_stream_type_t)in->type,
            .direction = (solar_os_stream_direction_t)in->direction,
            .sharing = (solar_os_stream_sharing_t)in->sharing,
            .has_scalar_range = in->has_scalar_range != 0,
            .scalar_minimum = in->scalar_minimum,
            .scalar_maximum = in->scalar_maximum,
        },
        .open = endpoint_open,
        .close_checked = ops->close ? endpoint_close : NULL,
        .read = ops->read ? endpoint_read : NULL,
        .write = ops->write ? endpoint_write : NULL,
        .read_scalar = ops->read_scalar ? endpoint_read_scalar : NULL,
        .read_scalar_sample = ops->read_sample ? endpoint_read_sample : NULL,
        .observe_scalar_sample = ops->observe_sample ? endpoint_observe_sample : NULL,
        .csv_header = ops->csv_header ? endpoint_csv_header : NULL,
        .read_csv = ops->read_csv ? endpoint_read_csv : NULL,
        .acquire_frame = ops->acquire_frame ? endpoint_acquire_frame : NULL,
        .release_frame = ops->release_frame ? endpoint_release_frame : NULL,
        .user = state,
    };
#define COPY_TEXT(field) do { \
        _Static_assert(sizeof(driver->info.field) == sizeof(in->field), "stream field capacity"); \
        if (!memchr(in->field, 0, sizeof(in->field))) return false; \
        strlcpy(driver->info.field, in->field, sizeof(driver->info.field)); \
    } while (0)
    COPY_TEXT(id);
    COPY_TEXT(provider);
    COPY_TEXT(device);
    COPY_TEXT(unit);
    COPY_TEXT(format);
    COPY_TEXT(summary);
#undef COPY_TEXT

    if ((in->type == SOLAR_OS_NATIVE_STREAM_AUDIO && !audio_format_in(&in->media, &driver->info.audio)) ||
        (in->type == SOLAR_OS_NATIVE_STREAM_VIDEO && !video_format_in(&in->media, &driver->info.video))) return false;

    strlcpy(state->id, driver->info.id, sizeof(state->id));
    state->format = in->media;
    state->callbacks = *ops;
    return true;
}

static int stream_provider_destroy(void *opaque)
{
    native_stream_provider_t *state = opaque;
    if (atomic_load(&state->opening)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->registered) {
        const char *ids[SOLAR_OS_STREAM_MAX];
        const void *owners[SOLAR_OS_STREAM_MAX];
        for (size_t i = 0; i < state->count; i++) {
            ids[i] = state->endpoints[i].id;
            owners[i] = &state->endpoints[i];
        }
        const esp_err_t error = solar_os_stream_unregister_many_owned(ids, owners, state->count);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK) return solar_os_native_result(error);
        state->registered = false;
    }
    if (state->library) {
        const int result = solar_os_native_library_release(state->library);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int stream_provider_register(solar_os_native_handle_t lease,
                                     const solar_os_native_stream_provider_endpoint_v1_t *endpoints,
                                     size_t count, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!lease || !endpoints || !count || count > SOLAR_OS_STREAM_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_stream_provider_t *state = solar_os_memory_calloc(1,
        sizeof(*state) + count * sizeof(state->endpoints[0]), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.stream.provider");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_stream_driver_t *drivers = solar_os_memory_calloc(count,
        sizeof(*drivers), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.stream.publish");
    if (!drivers) {
        solar_os_memory_free(state);
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }

    for (size_t i = 0; i < count; i++) {
        if (!endpoint_prepare(&state->endpoints[i], &endpoints[i], &drivers[i])) {
            solar_os_memory_free(drivers);
            solar_os_memory_free(state);
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
    }

    state->count = count;
    atomic_init(&state->opening, true);
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_STREAM_PROVIDER;
    int result = solar_os_native_handle_create(kind, state, stream_provider_destroy, out);
    if (result) {
        solar_os_memory_free(drivers);
        solar_os_memory_free(state);
        return result;
    }
    result = solar_os_native_library_retain(lease, &state->library);
    if (!result) result = solar_os_native_result(solar_os_stream_register_many(drivers, count));
    solar_os_memory_free(drivers);
    state->registered = result == SOLAR_OS_NATIVE_OK;
    atomic_store(&state->opening, false);
    if (result) {
        const int close = solar_os_native_handle_close_kind(*out, kind);
        if (!close || close == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) *out = 0;
    }
    return result;
}

static int stream_provider_close(solar_os_native_handle_t registration)
{
    return solar_os_native_handle_close_kind(registration, SOLAR_OS_NATIVE_HANDLE_STREAM_PROVIDER);
}

static const solar_os_native_stream_provider_api_v1_t stream_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_STREAM_PROVIDER_ABI,
    .struct_size = sizeof(stream_provider_api),
    .provider_register = stream_provider_register,
    .provider_close = stream_provider_close,
};
#endif

const void *solar_os_native_stream_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_stream_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
