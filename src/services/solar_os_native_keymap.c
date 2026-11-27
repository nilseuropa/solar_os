#include "solar_os_native_keymap.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_keymap_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_native.h"
#include "solar_os_memory.h"

#if SOLAR_OS_PACKAGE_SERVICE_INPUT_KEYMAP
#include "solar_os_input_keymap.h"
#include "solar_os_input_keymap_json.h"
static int keymap_info(const char *name, solar_os_input_keymap_info_t *info)
{ return solar_os_native_result(solar_os_input_keymap_info(name, info)); }
static int keymap_get(const char *name, bool defaults, solar_os_input_keymap_t *map)
{ return solar_os_native_result(solar_os_input_keymap_get(name, defaults, map)); }
static int keymap_set(const char *name, const solar_os_input_keymap_t *map)
{ return solar_os_native_result(solar_os_input_keymap_set(name, map)); }
static int keymap_reset(const char *name)
{ return solar_os_native_result(solar_os_input_keymap_reset(name)); }
static int keymap_load(const char *name, const char *path)
{ return solar_os_native_result(solar_os_input_keymap_load(name, path)); }
static int keymap_validate(const solar_os_input_keymap_t *map)
{ return solar_os_native_result(solar_os_input_keymap_validate(map)); }
static int keymap_parse(const char *json, const solar_os_input_keymap_t *base, solar_os_input_keymap_t *out)
{ return solar_os_native_result(solar_os_input_keymap_parse(json, base, out)); }
static int keymap_init_matrix(solar_os_input_keymap_t *map, uint32_t rows, uint32_t cols,
    uint32_t first, uint32_t stride)
{ return solar_os_native_result(solar_os_input_keymap_init_matrix(map, rows, cols, first, stride)); }

typedef struct {
    solar_os_native_keymap_provider_v1_t callbacks;
    solar_os_input_keymap_handle_t profile;
    uint32_t lease;
} native_keymap_provider_t;

/* The core pins each whole profile operation across lock/prepare/unlock.
 * Retirement waits for that pin before freeing this state or its code lease.
 * Do not gate unlock on a native handle acquisition: close may already be in
 * progress while an admitted operation still needs to unlock its hardware. */
static esp_err_t provider_lock(void *value)
{
    native_keymap_provider_t *state = value;
    return solar_os_native_callback_result(state->callbacks.lock(state->callbacks.user));
}
static void provider_unlock(void *value)
{
    native_keymap_provider_t *state = value;
    state->callbacks.unlock(state->callbacks.user);
}
static esp_err_t provider_prepare(void *value)
{
    native_keymap_provider_t *state = value;
    return state->callbacks.prepare ? solar_os_native_callback_result(state->callbacks.prepare(state->callbacks.user)) : ESP_OK;
}
static int provider_destroy(void *value)
{
    native_keymap_provider_t *state = value;
    if (state->profile) {
        esp_err_t error = solar_os_input_keymap_try_unregister(state->profile);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->profile = 0;
    }
    if (state->callbacks.producer) {
        solar_os_native_handle_release(state->callbacks.producer);
        state->callbacks.producer = 0;
    }
    if (state->lease) {
        int result = solar_os_native_library_release(state->lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
        state->lease = 0;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}
static int provider_open(const solar_os_native_keymap_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (out) *out = 0;
    if (!out || !in || in->struct_size < sizeof(*in) || !in->library_lease || !in->lock || !in->unlock || !in->defaults)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_keymap_provider_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.input.keymap");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->callbacks = *in;
    state->callbacks.producer = 0;
    /* Input producer payloads encode the core's exclusive source ID. Retain
     * this reference for the profile lifetime so close cannot recycle it. */
    solar_os_input_source_t source = (solar_os_input_source_t)(uintptr_t)solar_os_native_handle_acquire(in->producer, SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER);
    int result = source ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (source) state->callbacks.producer = in->producer;
    if (!result) result = solar_os_native_library_retain(in->library_lease, &state->lease);
    const solar_os_input_keymap_ops_t ops = {.lock = provider_lock, .unlock = provider_unlock, .prepare = provider_prepare, .context = state};
    if (!result) result = solar_os_native_result(solar_os_input_keymap_register(source, in->defaults, &ops, &state->profile));
    state->callbacks.defaults = NULL;
    if (!result) result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_INPUT_KEYMAP, state, provider_destroy, out);
    if (result) (void)provider_destroy(state); /* Inactive profiles have no callbacks in flight. */
    return result;
}
static native_keymap_provider_t *provider_acquire(solar_os_native_handle_t id)
{ return solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_INPUT_KEYMAP); }
static int provider_activate(solar_os_native_handle_t id)
{
    native_keymap_provider_t *state = provider_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_input_keymap_activate(state->profile));
    solar_os_native_handle_release(id);
    return result;
}
static int provider_process(solar_os_native_handle_t id, uint16_t physical, bool pressed, bool *published)
{
    if (published) *published = false;
    native_keymap_provider_t *state = provider_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_input_keymap_process(state->profile, physical, pressed, published));
    solar_os_native_handle_release(id);
    return result;
}
static int provider_press_priority(solar_os_native_handle_t id, uint16_t physical, unsigned *priority)
{
    if (!priority) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *priority = 2;
    native_keymap_provider_t *state = provider_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *priority = solar_os_input_keymap_press_priority(state->profile, physical);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}
static int provider_release_all(solar_os_native_handle_t id)
{
    native_keymap_provider_t *state = provider_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_input_keymap_release_all(state->profile);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}
static int provider_close(solar_os_native_handle_t id)
{
    native_keymap_provider_t *state = provider_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t error = solar_os_input_keymap_try_unregister(state->profile);
    solar_os_native_handle_release(id);
    if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_INPUT_KEYMAP);
}
static const solar_os_native_keymap_api_v1_t keymap_api = {
    .abi_version = SOLAR_OS_NATIVE_KEYMAP_ABI, .struct_size = sizeof(keymap_api),
    .info = keymap_info, .get = keymap_get, .set = keymap_set, .reset = keymap_reset, .load = keymap_load,
    .find_slot = solar_os_input_keymap_find_slot, .validate = keymap_validate,
    .release = solar_os_input_keymap_release, .decode = solar_os_input_keymap_decode, .parse = keymap_parse,
    .provider_open = provider_open, .provider_activate = provider_activate, .provider_process = provider_process,
    .provider_press_priority = provider_press_priority, .provider_release_all = provider_release_all, .provider_close = provider_close,
    .init_matrix = keymap_init_matrix,
};
#endif
const void *solar_os_native_keymap_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_keymap_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
