#include "solar_os_native_display_link.h"
#include "solar_os_config.h"
#include "solar_os_native_display_link_abi.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_DLP_CLIENT
#include "solar_os.h"
#include "solar_os_display_link.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include <stdatomic.h>

#define CHECK_VALUE(name) _Static_assert(SOLAR_OS_NATIVE_DISPLAY_LINK_##name == SDLP_##name, "DLP " #name)
CHECK_VALUE(MONO1);
CHECK_VALUE(GRAY2);
CHECK_VALUE(GRAY4);
CHECK_VALUE(GRAY8);
CHECK_VALUE(RGB565_BE);
CHECK_VALUE(PRESERVE_CONTENT);
CHECK_VALUE(STAGED_FRAME);
CHECK_VALUE(BRIGHTNESS);
CHECK_VALUE(POWER_SAVE);
CHECK_VALUE(READY);
CHECK_VALUE(RLE);
CHECK_VALUE(SYNC);
CHECK_VALUE(LZ4);
#undef CHECK_VALUE

typedef struct {
    solar_os_display_link_t *link;
    atomic_bool in_operation;
    bool ready;
} native_display_link_t;

typedef struct {
    solar_os_native_cancel_fn fn;
    void *user;
    bool cancelled;
} cancellation_t;

static bool cancelled(void *user)
{
    cancellation_t *state = user;
    state->cancelled = state->cancelled || state->fn(state->user);
    return state->cancelled;
}

static int destroy(void *value)
{
    native_display_link_t *state = value;
    if (atomic_load(&state->in_operation)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    state->ready = false;
    esp_err_t error = solar_os_display_link_close(state->link);
    if (error != ESP_OK) return solar_os_native_result(error);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int link_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LINK);
}

static int link_open(const char *name, const char *owner,
                const solar_os_native_display_link_options_v1_t *options,
                solar_os_native_cancel_fn cancel, void *user, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    native_display_link_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.link");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    atomic_init(&state->in_operation, true);
    const solar_os_app_t *app = solar_os_application_descriptor();
    int result = app ? solar_os_native_handle_create_owned(SOLAR_OS_NATIVE_HANDLE_DISPLAY_LINK,
        state, destroy, app, out) : solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_DISPLAY_LINK,
        state, destroy, out);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    solar_os_display_link_options_t config = {.format = SOLAR_OS_NATIVE_DISPLAY_LINK_PREFERRED};
    if (options) config = (solar_os_display_link_options_t){.format = options->format,
        .timeout_ms = options->timeout_ms, .compressed = options->compressed};
    cancellation_t cancellation = {.fn = cancel, .user = user};
    const solar_os_display_link_call_options_t call = {.cancelled = cancel ? cancelled : NULL,
        .cancel_user = &cancellation};
    esp_err_t error = solar_os_display_link_open_stream_checked(name, owner, &config, &call, &state->link);
    state->ready = error == ESP_OK;
    bool empty = !state->link;
    atomic_store(&state->in_operation, false);
    if (empty && !link_close(*out)) *out = 0;
    return error && cancellation.cancelled ? SOLAR_OS_NATIVE_ERROR_CANCELLED : solar_os_native_result(error);
}

static int enter(solar_os_native_handle_t id, native_display_link_t **out)
{
    native_display_link_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LINK);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (atomic_exchange(&state->in_operation, true)) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    if (!state->ready) {
        atomic_store(&state->in_operation, false);
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static void leave(solar_os_native_handle_t id, native_display_link_t *state)
{
    atomic_store(&state->in_operation, false);
    solar_os_native_handle_release(id);
}

static int info(solar_os_native_handle_t id, solar_os_native_display_link_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    native_display_link_t *state;
    int result = enter(id, &state);
    if (result) return result;
    esp_err_t error = solar_os_display_link_copy_info(state->link, out);
    leave(id, state);
    return solar_os_native_result(error);
}

static int present(solar_os_native_handle_t id, const void *pixels, size_t size,
                   solar_os_native_cancel_fn cancel, void *user)
{
    native_display_link_t *state;
    int result = enter(id, &state);
    if (result) return result;
    cancellation_t cancellation = {.fn = cancel, .user = user};
    const solar_os_display_link_call_options_t call = {.cancelled = cancel ? cancelled : NULL,
        .cancel_user = &cancellation};
    esp_err_t error = solar_os_display_link_present_with_options(state->link, pixels, size, &call);
    leave(id, state);
    return error && cancellation.cancelled ? SOLAR_OS_NATIVE_ERROR_CANCELLED : solar_os_native_result(error);
}

static const solar_os_native_display_link_api_v1_t display_link_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_LINK_ABI, .struct_size = sizeof(display_link_api),
    .open = link_open, .info = info, .present = present, .close = link_close,
};
#endif

const void *solar_os_native_display_link_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_display_link_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
