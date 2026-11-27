#include "solar_os_native_audio_player.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio_player.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"

_Static_assert(SOLAR_OS_NATIVE_AUDIO_PLAYER_VOLUME_GLOBAL == SOLAR_OS_AUDIO_VOLUME_GLOBAL,
    "audio player volume default");

typedef struct {
    solar_os_audio_player_t *player;
    solar_os_native_handle_t lease;
} native_player_t;

static int player_destroy(void *opaque)
{
    native_player_t *state = opaque;
    const esp_err_t error = solar_os_audio_player_destroy_checked(state->player);
    if (error != ESP_OK) return solar_os_native_result(error);
    state->player = NULL;
    if (state->lease) {
        const int result = solar_os_native_library_release(state->lease);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int player_close(solar_os_native_handle_t player)
{
    return solar_os_native_handle_close_kind(player, SOLAR_OS_NATIVE_HANDLE_AUDIO_PLAYER);
}

static int player_open(const solar_os_native_audio_player_options_v1_t *in,
    solar_os_native_handle_t *out, solar_os_native_stream_format_v1_t *format, char *device_id)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (format) *format = (solar_os_native_stream_format_v1_t){0};
    if (device_id) device_id[0] = '\0';
    if (!in || !in->open_timeout_ms || in->format.audio_format != SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE ||
        in->format.channels > 2 || (in->format.bits_per_sample && in->format.bits_per_sample != 16) ||
        in->format.frames_per_block > UINT16_MAX ||
        ((in->state || in->samples || in->cancel || in->pause) && !in->library_lease)) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    native_player_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.audio.player");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_native_handle_t handle;
    int result = solar_os_native_handle_create_exclusive(SOLAR_OS_NATIVE_HANDLE_AUDIO_PLAYER,
        state, player_destroy, NULL, &handle);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    if (in->library_lease) result = solar_os_native_library_retain(in->library_lease, &state->lease);
    if (!result) {
        const solar_os_audio_player_options_t options = {
            .owner = in->owner,
            .requested_audio = {
                .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                .sample_rate = in->format.sample_rate,
                .channels = (uint8_t)in->format.channels,
                .bits_per_sample = (uint8_t)in->format.bits_per_sample,
                .frames_per_block = (uint16_t)in->format.frames_per_block,
            },
            .volume = in->volume,
            .buffered = in->buffered,
            .external_buffer_bytes = in->external_buffer_bytes,
            .internal_buffer_bytes = in->internal_buffer_bytes,
            .target_ms = in->target_ms,
            .open_timeout_ms = in->open_timeout_ms,
            .state = in->state, .samples = in->samples, .user = in->user,
            .should_cancel = in->cancel, .cancel_user = in->user,
            .should_pause = in->pause, .pause_user = in->user,
        };
        solar_os_stream_audio_format_t negotiated;
        solar_os_audio_device_info_t device;
        result = solar_os_native_result(solar_os_audio_player_create(&options, &state->player,
            &negotiated, device_id ? &device : NULL));
        if (!result && format) {
            *format = (solar_os_native_stream_format_v1_t){
                .audio_format = SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE,
                .sample_rate = negotiated.sample_rate,
                .channels = negotiated.channels,
                .bits_per_sample = negotiated.bits_per_sample,
                .frames_per_block = negotiated.frames_per_block,
            };
        }
        if (!result && device_id) memcpy(device_id, device.id, sizeof(device.id));
    }
    solar_os_native_handle_release_exclusive(handle);
    if (result && player_close(handle) == SOLAR_OS_NATIVE_OK) return result;
    *out = handle;
    return result;
}

static int player_write(solar_os_native_handle_t handle, const int16_t *samples, size_t count)
{
    if (!samples || !count || count > SIZE_MAX / sizeof(*samples)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *opaque;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle,
        SOLAR_OS_NATIVE_HANDLE_AUDIO_PLAYER, &opaque);
    if (acquired) return acquired;
    native_player_t *state = opaque;
    const esp_err_t error = solar_os_audio_player_write(state->player, samples, count * sizeof(*samples), NULL);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int player_operation(solar_os_native_handle_t handle, esp_err_t (*operation)(solar_os_audio_player_t *))
{
    void *opaque;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle,
        SOLAR_OS_NATIVE_HANDLE_AUDIO_PLAYER, &opaque);
    if (acquired) return acquired;
    native_player_t *state = opaque;
    const esp_err_t error = operation(state->player);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static esp_err_t player_finish_core(solar_os_audio_player_t *player)
{
    return solar_os_audio_player_finish(player, NULL);
}

static int player_end(solar_os_native_handle_t handle)
{
    return player_operation(handle, solar_os_audio_player_end_input);
}

static int player_finish(solar_os_native_handle_t handle)
{
    return player_operation(handle, player_finish_core);
}

static esp_err_t player_error_core(solar_os_audio_player_t *player)
{
    return solar_os_audio_player_error(player);
}

static int player_error(solar_os_native_handle_t handle)
{
    return player_operation(handle, player_error_core);
}

static const solar_os_native_audio_player_api_v1_t audio_player_api = {
    .abi_version = SOLAR_OS_NATIVE_AUDIO_PLAYER_ABI,
    .struct_size = sizeof(audio_player_api),
    .open = player_open, .write = player_write, .end_input = player_end,
    .finish = player_finish, .error = player_error, .close = player_close,
};
#endif

const void *solar_os_native_audio_player_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_audio_player_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
