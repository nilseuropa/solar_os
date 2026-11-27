#include "solar_os_native_emotion.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_EMOTION
#include "solar_os.h"
#include "solar_os_emotion.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

#define EMOTION_HANDLE_KIND SOLAR_OS_NATIVE_HANDLE_EMOTION

typedef struct {
    solar_os_emotion_t *instance;
    bool ready;
} native_emotion_t;

static int emotion_destroy(void *opaque)
{
    native_emotion_t *state = opaque;
    state->ready = false;
    const int result = solar_os_native_result(solar_os_emotion_close(&state->instance));
    if (!result) solar_os_memory_free(state);
    return result;
}

static int emotion_open(const solar_os_emotion_options_t *options, solar_os_native_handle_t *out,
    char *detail, size_t capacity)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!options || (!detail && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (detail && capacity) detail[0] = 0;
    native_emotion_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "emotion.native");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    solar_os_native_handle_t handle = 0;
    int result = owner ? solar_os_native_handle_create_owned(EMOTION_HANDLE_KIND, state, emotion_destroy, owner, &handle) :
        solar_os_native_handle_create(EMOTION_HANDLE_KIND, state, emotion_destroy, &handle);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    solar_os_context_t *ctx = solar_os_application_context();
    solar_os_gfx_t *gfx = ctx ? solar_os_context_gfx(ctx) : NULL;
    result = solar_os_native_result(solar_os_emotion_open(options, gfx, &state->instance, detail, capacity));
    state->ready = result == SOLAR_OS_NATIVE_OK;
    if (result && !state->instance) {
        (void)solar_os_native_handle_close_kind(handle, EMOTION_HANDLE_KIND);
    } else *out = handle;
    return result;
}

static int emotion_acquire(solar_os_native_handle_t id, solar_os_emotion_t **instance)
{
    void *borrowed;
    const int result = solar_os_native_handle_acquire_exclusive(id, EMOTION_HANDLE_KIND, &borrowed);
    if (result) return result;
    native_emotion_t *state = borrowed;
    if (!state->ready) {
        solar_os_native_handle_release_exclusive(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *instance = state->instance;
    return SOLAR_OS_NATIVE_OK;
}

static int emotion_finish(solar_os_native_handle_t id, esp_err_t error)
{
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(error);
}

static int emotion_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, EMOTION_HANDLE_KIND); }

static int emotion_set_suspended(solar_os_native_handle_t id, bool suspended)
{
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    return result ? result : emotion_finish(id, suspended ? solar_os_emotion_suspend(instance) :
        solar_os_emotion_resume(instance));
}

static int emotion_set(solar_os_native_handle_t id, const char *pose, uint32_t transition_ms)
{
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    return result ? result : emotion_finish(id, solar_os_emotion_set(instance, pose, transition_ms));
}

static int emotion_react(solar_os_native_handle_t id, const char *pose, uint32_t hold_ms,
    uint32_t transition_ms, const char *cue)
{
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    return result ? result : emotion_finish(id, solar_os_emotion_react(instance, pose, hold_ms, transition_ms, cue));
}

static int emotion_look(solar_os_native_handle_t id, int32_t x, int32_t y)
{
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    return result ? result : emotion_finish(id, solar_os_emotion_look(instance, x, y));
}

static int emotion_blink(solar_os_native_handle_t id)
{
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    return result ? result : emotion_finish(id, solar_os_emotion_blink(instance));
}

static int emotion_status(solar_os_native_handle_t id, solar_os_emotion_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_emotion_status_t){0};
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    if (result) return result;
    solar_os_emotion_status(instance, out);
    out->error = solar_os_native_result(out->error);
    out->audio_error = solar_os_native_result(out->audio_error);
    return emotion_finish(id, ESP_OK);
}

static int emotion_uses_foreground(solar_os_native_handle_t id, bool *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = false;
    solar_os_emotion_t *instance;
    const int result = emotion_acquire(id, &instance);
    if (result) return result;
    solar_os_context_t *ctx = solar_os_application_context();
    solar_os_gfx_t *gfx = ctx ? solar_os_context_gfx(ctx) : NULL;
    *out = gfx && solar_os_emotion_uses_gfx(instance, gfx);
    return emotion_finish(id, ESP_OK);
}

static const solar_os_native_emotion_api_v1_t emotion_api = {
    .abi_version = SOLAR_OS_NATIVE_EMOTION_ABI, .struct_size = sizeof(emotion_api),
    .open = emotion_open, .close = emotion_close, .set_suspended = emotion_set_suspended,
    .set = emotion_set, .react = emotion_react, .look = emotion_look, .blink = emotion_blink,
    .status = emotion_status, .uses_foreground = emotion_uses_foreground,
};
#endif

const void *solar_os_native_emotion_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_emotion_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
