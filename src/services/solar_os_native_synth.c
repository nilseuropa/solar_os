#include "solar_os_native_synth.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
#include <inttypes.h>
#include <stdio.h>
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"
#include "solar_os_synth.h"
#include "solar_os_synth_voice.h"

_Static_assert(SOLAR_OS_SYNTH_STREAM_NAME_MAX == SOLAR_OS_STREAM_ID_MAX, "synth output name capacity");

typedef struct {
    char owner[SOLAR_OS_SYNTH_OWNER_MAX];
    uint32_t library;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    bool attempted_start;
} render_session_t;

static int render_destroy(void *opaque)
{
    render_session_t *session = opaque;
    if (session->attempted_start) {
        esp_err_t error = solar_os_synth_stop(session->owner);
        if (error == ESP_ERR_INVALID_STATE) {
            solar_os_synth_status_t status;
            solar_os_synth_get_status(&status);
            if (strcmp(status.owner, session->owner)) error = ESP_OK;
        }
        if (error != ESP_OK) return solar_os_native_result(error);
        session->attempted_start = false;
    }
    if (session->library) {
        const int result = solar_os_native_library_release(session->library);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (session->driver_pin) solar_os_native_driver_unpin(session->driver_pin);
#endif
    solar_os_memory_free(session);
    return SOLAR_OS_NATIVE_OK;
}

static int render_pin(render_session_t *session, solar_os_native_handle_t library)
{
    if (library) return solar_os_native_library_retain(library, &session->library);
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    session->driver_pin = solar_os_native_driver_pin();
    if (session->driver_pin) return SOLAR_OS_NATIVE_OK;
#endif
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}

static int render_open(const solar_os_native_synth_config_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->render || in->block_frames < SOLAR_OS_SYNTH_BLOCK_FRAMES_MIN ||
        in->block_frames > SOLAR_OS_SYNTH_BLOCK_FRAMES_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    render_session_t *session = solar_os_memory_calloc(1, sizeof(*session),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.synth");
    if (!session) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_native_handle_t handle;
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_SYNTH, session, render_destroy, &handle);
    if (result) {
        solar_os_memory_free(session);
        return result;
    }
    snprintf(session->owner, sizeof(session->owner), "native-synth-%08" PRIx32, handle);
    result = render_pin(session, in->library_lease);
    if (!result) {
        const solar_os_synth_config_t config = {
            .owner = session->owner, .playback_stream = in->playback_stream,
            .render = in->render, .user = in->user, .block_frames = in->block_frames,
        };
        session->attempted_start = true;
        result = solar_os_native_result(solar_os_synth_start(&config));
    }
    if (result) {
        const int closed = solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_SYNTH);
        if (closed == SOLAR_OS_NATIVE_OK || closed == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    *out = handle;
    return result;
}

static int render_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_SYNTH);
}

static int render_status(solar_os_synth_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_synth_get_status(status);
    status->last_error = solar_os_native_result(status->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int voice_destroy(void *opaque)
{
    char *owner = opaque;
    esp_err_t error = solar_os_synth_voice_stop(owner);
    if (error == ESP_ERR_INVALID_STATE) {
        solar_os_synth_voice_status_t status;
        solar_os_synth_voice_get_status(&status);
        if (strcmp(status.owner, owner)) error = ESP_OK;
    }
    if (error != ESP_OK) return solar_os_native_result(error);
    solar_os_memory_free(owner);
    return SOLAR_OS_NATIVE_OK;
}

static int voice_open(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    char *owner = solar_os_memory_alloc(SOLAR_OS_SYNTH_OWNER_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.voice");
    if (!owner) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_native_handle_t handle;
    const int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_VOICE, owner, voice_destroy, &handle);
    if (result) {
        solar_os_memory_free(owner);
        return result;
    }
    snprintf(owner, SOLAR_OS_SYNTH_OWNER_MAX, "native-voice-%08" PRIx32, handle);
    *out = handle;
    return SOLAR_OS_NATIVE_OK;
}

static int voice_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_VOICE);
}

static int voice_status(solar_os_synth_voice_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_synth_voice_get_status(status);
    status->last_error = solar_os_native_result(status->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int voice_configure(solar_os_native_handle_t handle, const solar_os_synth_voice_config_t *config)
{
    void *owner;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_VOICE, &owner);
    if (acquired) return acquired;
    const esp_err_t error = solar_os_synth_voice_configure(owner, config);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int voice_configure_performance(solar_os_native_handle_t handle,
    const solar_os_synth_voice_performance_t *performance)
{
    void *owner;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_VOICE, &owner);
    if (acquired) return acquired;
    const esp_err_t error = solar_os_synth_voice_configure_performance(owner, performance);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int voice_note_on(solar_os_native_handle_t handle, uint32_t frequency_hz, uint8_t velocity)
{
    void *owner;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_VOICE, &owner);
    if (acquired) return acquired;
    const esp_err_t error = solar_os_synth_voice_note_on(owner, frequency_hz, velocity);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int voice_note_off(solar_os_native_handle_t handle, uint32_t frequency_hz)
{
    void *owner;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_VOICE, &owner);
    if (acquired) return acquired;
    const esp_err_t error = solar_os_synth_voice_note_off(owner, frequency_hz);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int voice_all_notes_off(solar_os_native_handle_t handle)
{
    void *owner;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_VOICE, &owner);
    if (acquired) return acquired;
    const esp_err_t error = solar_os_synth_voice_all_notes_off(owner);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int voice_set_wavetable(solar_os_native_handle_t handle, const int16_t *samples, size_t count)
{
    void *owner;
    const int acquired = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_VOICE, &owner);
    if (acquired) return acquired;
    const esp_err_t error = solar_os_synth_voice_set_wavetable(owner, samples, count);
    solar_os_native_handle_release_exclusive(handle);
    return solar_os_native_result(error);
}

static int voice_get_wavetable(int16_t *samples, size_t count)
{
    return solar_os_native_result(solar_os_synth_voice_get_wavetable(samples, count));
}

static const solar_os_native_synth_api_v1_t synth_api = {
    .abi_version = SOLAR_OS_NATIVE_SYNTH_ABI, .struct_size = sizeof(synth_api),
    .open = render_open, .close = render_close, .status = render_status,
};

static const solar_os_native_voice_api_v1_t voice_api = {
    .abi_version = SOLAR_OS_NATIVE_VOICE_ABI, .struct_size = sizeof(voice_api),
    .open = voice_open, .close = voice_close, .status = voice_status,
    .configure = voice_configure, .configure_performance = voice_configure_performance,
    .note_on = voice_note_on, .note_off = voice_note_off, .all_notes_off = voice_all_notes_off,
    .set_wavetable = voice_set_wavetable, .get_wavetable = voice_get_wavetable,
    .waveform_name = solar_os_synth_waveform_name, .parse_waveform = solar_os_synth_parse_waveform,
};
#endif

const void *solar_os_native_synth_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_synth_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
