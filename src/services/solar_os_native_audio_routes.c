#include "solar_os_native_audio_routes.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_audio_routes_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
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
#endif

const void *solar_os_native_audio_routes_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_audio_routes_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
