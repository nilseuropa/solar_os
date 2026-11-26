#include "solar_os_native_audio_routes.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_audio_routes_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#include "solar_os_memory.h"
#include "solar_os_native_audio_device_provider_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_stream.h"

static int route_copy(solar_os_audio_route_v1_t *out, const solar_os_audio_device_info_t *in)
{
    if (in->native_format.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE)
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;

    strlcpy(out->id, in->id, sizeof(out->id));
    strlcpy(out->name, in->name, sizeof(out->name));
    strlcpy(out->provider, in->provider, sizeof(out->provider));
    strlcpy(out->capture_stream, in->capture_stream, sizeof(out->capture_stream));
    strlcpy(out->playback_stream, in->playback_stream, sizeof(out->playback_stream));
    out->capabilities = in->capabilities;
    out->format = (solar_os_native_stream_format_v1_t){
        .audio_format = SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE,
        .sample_rate = in->native_format.sample_rate,
        .channels = in->native_format.channels,
        .bits_per_sample = in->native_format.bits_per_sample,
        .frames_per_block = in->native_format.frames_per_block,
    };
    return SOLAR_OS_NATIVE_OK;
}

static int route_info(size_t index, solar_os_audio_route_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_audio_route_v1_t){0};
    solar_os_audio_device_info_t device;
    if (!solar_os_audio_device_get(index, &device)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return route_copy(out, &device);
}

static int route_find(const char *id, solar_os_audio_route_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_audio_route_v1_t){0};
    solar_os_audio_device_info_t device;
    const esp_err_t error = solar_os_audio_device_get_info(id, &device);
    return error == ESP_OK ? route_copy(out, &device) : solar_os_native_result(error);
}

static int route_get_default_output(char *out, size_t capacity)
{
    if (!out || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    out[0] = '\0';
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    (void)solar_os_audio_get_default_output(id, sizeof(id));
    const size_t size = strlen(id) + 1;
    if (size > capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    memcpy(out, id, size);
    return SOLAR_OS_NATIVE_OK;
}

static int route_set_default_output(const char *id)
{
    return solar_os_native_result(solar_os_audio_set_default_output(id));
}

static int route_set_volume(const char *id, uint8_t volume)
{
    return solar_os_native_result(id && *id ? solar_os_audio_set_device_volume(id, volume) :
        solar_os_audio_set_volume(volume));
}

static int route_toggle_mute(uint8_t *volume_after)
{
    return solar_os_native_result(solar_os_audio_toggle_mute(volume_after));
}

static int route_board_status(solar_os_audio_board_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_audio_status_t status = {0};
    solar_os_audio_get_status(&status);
    *out = (solar_os_audio_board_status_v1_t){
        .initialized = status.initialized, .i2s_port = status.i2s_port,
        .mclk_pin = status.mclk_pin, .bclk_pin = status.bclk_pin, .ws_pin = status.ws_pin,
        .din_pin = status.din_pin, .dout_pin = status.dout_pin, .pa_pin = status.pa_pin,
        .format = {.audio_format = SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE, .sample_rate = status.sample_rate,
            .channels = status.channels, .bits_per_sample = status.bits_per_sample},
    };
    const size_t output_size = strlcpy(out->output_codec, status.output_codec ? status.output_codec : "-", sizeof(out->output_codec));
    const size_t input_size = strlcpy(out->input_codec, status.input_codec ? status.input_codec : "-", sizeof(out->input_codec));
    if (output_size >= sizeof(out->output_codec) || input_size >= sizeof(out->input_codec)) {
        *out = (solar_os_audio_board_status_v1_t){0};
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_audio_routes_api_v1_t audio_routes_api = {
    .abi_version = SOLAR_OS_NATIVE_AUDIO_ROUTES_ABI,
    .struct_size = sizeof(audio_routes_api),
    .count = solar_os_audio_device_count,
    .info = route_info,
    .find = route_find,
    .output_available = solar_os_audio_output_available,
    .get_default_output = route_get_default_output,
    .set_default_output = route_set_default_output,
    .get_volume = solar_os_audio_global_volume,
    .set_volume = route_set_volume,
    .toggle_mute = route_toggle_mute,
    .open_default = solar_os_native_stream_open_default_audio,
    .capture_frames = solar_os_native_stream_capture_audio,
    .measure_level = solar_os_native_stream_measure_audio,
    .board_status = route_board_status,
};
typedef struct {
    solar_os_native_audio_device_controls_v1_t callbacks;
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    uint32_t library;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    bool registered;
} audio_provider_t;

static esp_err_t audio_provider_volume(void *user, uint8_t volume)
{
    audio_provider_t *state = user;
    return solar_os_native_callback_result(state->callbacks.set_volume(state->callbacks.user, volume));
}

static esp_err_t audio_provider_get_gain(void *user, float *gain)
{
    audio_provider_t *state = user;
    return solar_os_native_callback_result(state->callbacks.get_input_gain(state->callbacks.user, gain));
}

static esp_err_t audio_provider_set_gain(void *user, float gain)
{
    audio_provider_t *state = user;
    return solar_os_native_callback_result(state->callbacks.set_input_gain(state->callbacks.user, gain));
}

static int audio_provider_destroy(void *opaque)
{
    audio_provider_t *state = opaque;
    if (state->registered) {
        const esp_err_t error = solar_os_audio_unregister_device_owned(state->id, state);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->registered = false;
    }
    if (state->library) {
        const int result = solar_os_native_library_release(state->library);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (state->driver_pin) solar_os_native_driver_unpin(state->driver_pin);
#endif
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int audio_provider_pin(audio_provider_t *state, const solar_os_native_audio_device_provider_v1_t *in)
{
    if (!in->controls.set_volume && !in->controls.get_input_gain && !in->controls.set_input_gain) return SOLAR_OS_NATIVE_OK;
    if (in->library_lease) return solar_os_native_library_retain(in->library_lease, &state->library);
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    state->driver_pin = solar_os_native_driver_pin();
    if (state->driver_pin) return SOLAR_OS_NATIVE_OK;
#endif
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}

static bool audio_provider_route_valid(const solar_os_native_audio_device_provider_v1_t *in)
{
    const solar_os_audio_route_v1_t *route = in->route;
    const uint32_t caps = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT | SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT |
        SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME | SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN;
    return memchr(route->id, 0, sizeof(route->id)) && memchr(route->name, 0, sizeof(route->name)) &&
        memchr(route->provider, 0, sizeof(route->provider)) &&
        memchr(route->capture_stream, 0, sizeof(route->capture_stream)) &&
        memchr(route->playback_stream, 0, sizeof(route->playback_stream)) &&
        route->capabilities && !(route->capabilities & ~caps) &&
        ((route->capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0) == (in->controls.set_volume != NULL) &&
        ((route->capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN) != 0) ==
            (in->controls.get_input_gain != NULL || in->controls.set_input_gain != NULL) &&
        route->format.audio_format == SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE &&
        route->format.bits_per_sample == 16;
}

static int audio_provider_register(const solar_os_native_audio_device_provider_v1_t *in,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->route || !audio_provider_route_valid(in)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    audio_provider_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.audio.device");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_native_handle_t handle;
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_AUDIO_DEVICE_PROVIDER,
        state, audio_provider_destroy, &handle);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    state->callbacks = in->controls;
    strlcpy(state->id, in->route->id, sizeof(state->id));
    result = audio_provider_pin(state, in);
    if (!result) {
        const solar_os_audio_route_v1_t *route = in->route;
        solar_os_audio_device_info_t device = {
            .capabilities = route->capabilities,
            .native_format = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                .sample_rate = route->format.sample_rate, .channels = route->format.channels,
                .bits_per_sample = route->format.bits_per_sample, .frames_per_block = route->format.frames_per_block},
            .input_gain_min_db = in->input_gain_min_db, .input_gain_max_db = in->input_gain_max_db,
            .input_gain_step_db = in->input_gain_step_db,
        };
        strlcpy(device.id, route->id, sizeof(device.id));
        strlcpy(device.name, route->name, sizeof(device.name));
        strlcpy(device.provider, route->provider, sizeof(device.provider));
        strlcpy(device.capture_stream, route->capture_stream, sizeof(device.capture_stream));
        strlcpy(device.playback_stream, route->playback_stream, sizeof(device.playback_stream));
        const solar_os_audio_device_ops_t ops = {
            .set_volume = in->controls.set_volume ? audio_provider_volume : NULL,
            .get_input_gain = in->controls.get_input_gain ? audio_provider_get_gain : NULL,
            .set_input_gain = in->controls.set_input_gain ? audio_provider_set_gain : NULL,
        };
        result = solar_os_native_result(solar_os_audio_register_device_ex(&device, &ops, state));
        state->registered = result == SOLAR_OS_NATIVE_OK;
    }
    if (result) {
        const int closed = solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_AUDIO_DEVICE_PROVIDER);
        if (closed == SOLAR_OS_NATIVE_OK || closed == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    *out = handle;
    return result;
}

static int audio_provider_close(solar_os_native_handle_t registration)
{
    return solar_os_native_handle_close_kind(registration, SOLAR_OS_NATIVE_HANDLE_AUDIO_DEVICE_PROVIDER);
}

static const solar_os_native_audio_device_provider_api_v1_t audio_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_AUDIO_DEVICE_PROVIDER_ABI,
    .struct_size = sizeof(audio_provider_api), .register_device = audio_provider_register,
    .close = audio_provider_close,
};
#endif

const void *solar_os_native_audio_routes_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_audio_routes_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
