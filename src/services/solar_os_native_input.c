#include "solar_os_native_input.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_input.h"
#include "solar_os_input_composition.h"
#include "solar_os_keys.h"
#include "solar_os.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
static int input_pointer_apply_display_transform(solar_os_input_pointer_event_t *event, uint16_t width, uint16_t height, uint16_t orientation_degrees, uint8_t scale)
{ return solar_os_native_result(solar_os_input_pointer_apply_display_transform(event, width, height, orientation_degrees, scale)); }
static int input_pointer_apply_orientation(solar_os_input_pointer_event_t *event, uint16_t width, uint16_t height, uint16_t orientation_degrees)
{ return solar_os_native_result(solar_os_input_pointer_apply_orientation(event, width, height, orientation_degrees)); }
static int input_pointer_calibration_get(solar_os_input_source_t source, bool *enabled, solar_os_input_pointer_calibration_t *calibration)
{ return solar_os_native_result(solar_os_input_pointer_calibration_get(source, enabled, calibration)); }
static int input_pointer_calibration_set(solar_os_input_source_t source, const solar_os_input_pointer_calibration_t *calibration)
{ return solar_os_native_result(solar_os_input_pointer_calibration_set(source, calibration)); }
static int input_pointer_calibration_reset(solar_os_input_source_t source)
{ return solar_os_native_result(solar_os_input_pointer_calibration_reset(source)); }
static int input_set_keyboard_layout(solar_os_input_keyboard_layout_t layout)
{ return solar_os_native_result(solar_os_input_set_keyboard_layout(layout)); }
static int input_set_keyboard_layout_list(const char *text)
{ return solar_os_native_result(solar_os_input_set_keyboard_layout_list(text)); }
static int input_keyboard_layout_list(char *text, size_t text_len)
{ return solar_os_native_result(solar_os_input_keyboard_layout_list(text, text_len)); }
static int input_key_text(uint8_t key, solar_os_input_text_t *text)
{
    if (!text) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *text = (solar_os_input_text_t){0};
    *text = solar_os_input_key_text(key);
    return SOLAR_OS_NATIVE_OK;
}
static int input_resolve_layout_key(solar_os_input_key_event_t *event, solar_os_input_text_t *text)
{
    if (!text) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *text = (solar_os_input_text_t){0};
    if (!event) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *text = solar_os_input_resolve_layout_key(event);
    return SOLAR_OS_NATIVE_OK;
}
static size_t input_compose_feed(solar_os_input_compose_t *state, const solar_os_input_text_t *text, uint16_t out[2])
{
    if (!state || !text || !out) return 0;
    return solar_os_input_compose_feed(state, *text, out);
}
static int input_set_repeat(uint16_t rate_cps, uint16_t delay_ms)
{ return solar_os_native_result(solar_os_input_set_repeat(rate_cps, delay_ms)); }


/* Source IDs are nonzero core-owned values. Encode the ID in the opaque handle
 * payload; no separate adapter object or allocation is needed. Exclusive
 * creation prevents another producer from aliasing and closing this source. */
static solar_os_input_source_t producer_acquire(solar_os_native_handle_t id)
{ return (solar_os_input_source_t)(uintptr_t)solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER); }
static int producer_destroy(void *value)
{ solar_os_input_source_close((solar_os_input_source_t)(uintptr_t)value); return SOLAR_OS_NATIVE_OK; }
static int producer_open(const char *name, solar_os_input_source_class_t source_class,
    uint32_t capabilities, bool ready, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_input_source_t source = SOLAR_OS_INPUT_SOURCE_INVALID;
    int result = solar_os_native_result(solar_os_input_source_create(name, source_class, capabilities, ready, &source));
    if (result) return result;
    void *value = (void *)(uintptr_t)source;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    result = owner ? solar_os_native_handle_create_owned(SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER, value, producer_destroy, owner, out) :
        solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER, value, producer_destroy, out);
    if (result) solar_os_input_source_close(source);
    return result;
}
static int producer_info(solar_os_native_handle_t id, solar_os_input_source_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *info = (solar_os_input_source_info_t){0};
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_input_source_get_info(source, info) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_native_handle_release(id);
    return result;
}
static int producer_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER); }
static int producer_ready(solar_os_native_handle_t id, bool ready)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_source_set_ready(source, ready));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_release_all(solar_os_native_handle_t id)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_input_source_release_all(source);
    const int result = SOLAR_OS_NATIVE_OK;
    solar_os_native_handle_release(id);
    return result;
}
static int producer_modifiers(solar_os_native_handle_t id, uint8_t modifiers)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_input_composition_set_modifiers(source, modifiers);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}
static int producer_write_key(solar_os_native_handle_t id, uint16_t physical_key, uint16_t usage, uint8_t key, uint8_t modifiers, solar_os_input_key_action_t action)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_key(source, physical_key, usage, key, modifiers, action));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_set_key(solar_os_native_handle_t id, uint16_t physical_key, uint16_t usage, uint8_t key, uint8_t modifiers)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_set_key(source, physical_key, usage, key, modifiers));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_write_key_tap(solar_os_native_handle_t id, uint16_t physical_key, uint16_t usage, uint8_t key, uint8_t modifiers)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_key_tap(source, physical_key, usage, key, modifiers));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_write_char(solar_os_native_handle_t id, char ch)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_char(source, ch));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_write_pointer(solar_os_native_handle_t id, const solar_os_input_pointer_event_t *event)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_pointer(source, event));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_write_pointer_sample(solar_os_native_handle_t id, const solar_os_input_pointer_sample_t *sample)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_pointer_sample(source, sample));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_write_axis(solar_os_native_handle_t id, const solar_os_input_axis_event_t *event)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_axis(source, event));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_write_gesture(solar_os_native_handle_t id, const solar_os_input_gesture_event_t *event)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_write_gesture(source, event));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_gestures(solar_os_native_handle_t id, uint32_t gesture_mask)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_source_set_gestures(source, gesture_mask));
    solar_os_native_handle_release(id);
    return result;
}
static int producer_calibration_default(solar_os_native_handle_t id, const solar_os_input_pointer_calibration_t *calibration)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_input_pointer_calibration_default(source, calibration));
    solar_os_native_handle_release(id);
    return result;
}
static size_t producer_read_chars(solar_os_native_handle_t id, char *buffer, size_t buffer_len)
{
    const solar_os_input_source_t source = producer_acquire(id);
    if (!source) return 0;
    const size_t result = solar_os_input_read_source_chars(source, buffer, buffer_len);
    solar_os_native_handle_release(id);
    return result;
}


typedef struct {
    uint32_t kind;
    solar_os_input_source_t source;
    bool claimed;
} native_input_capture_t;
static int capture_destroy(void *value)
{
    native_input_capture_t *state = value;
    if (state->claimed) {
        if (state->kind == SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE) solar_os_input_release_keyboard(state);
        else solar_os_input_release_pointer(state->source, state);
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}
static int capture_create(uint32_t kind, const char *name, bool shortcuts, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!name || !*name || strlen(name) >= SOLAR_OS_INPUT_SOURCE_NAME_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_input_capture_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.input.capture");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->kind = kind;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    int result = owner ? solar_os_native_handle_create_owned(kind, state, capture_destroy, owner, out) :
        solar_os_native_handle_create(kind, state, capture_destroy, out);
    if (result) { solar_os_memory_free(state); return result; }
    (void)solar_os_native_handle_acquire(*out, kind);
    if (kind == SOLAR_OS_NATIVE_HANDLE_INPUT_POINTER_CAPTURE)
        result = solar_os_native_result(solar_os_input_capture_pointer(name, state, &state->source));
    else
        result = solar_os_native_result(shortcuts ? solar_os_input_capture_keyboard_with_shortcuts(name, state) :
            solar_os_input_capture_keyboard(name, state));
    state->claimed = result == SOLAR_OS_NATIVE_OK;
    solar_os_native_handle_release(*out);
    if (result) { (void)solar_os_native_handle_close_kind(*out, kind); *out = 0; }
    return result;
}
static int keyboard_open(const char *name, bool shortcuts, solar_os_native_handle_t *out)
{ return capture_create(SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE, name, shortcuts, out); }
static int keyboard_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE); }
static bool keyboard_claimed(solar_os_native_handle_t id)
{
    native_input_capture_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE);
    if (!state) return false;
    const bool claimed = solar_os_input_keyboard_captured_by(state);
    solar_os_native_handle_release(id);
    return claimed;
}
static int keyboard_read(solar_os_native_handle_t id, solar_os_input_key_event_t *event, bool *reset)
{
    if (!event || !reset) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *event = (solar_os_input_key_event_t){0}; *reset = false;
    native_input_capture_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (solar_os_input_keyboard_captured_by(state))
        result = solar_os_input_read_captured_key(state, event, reset) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_native_handle_release(id);
    return result;
}
static int pointer_open(const char *name, solar_os_native_handle_t *out)
{ return capture_create(SOLAR_OS_NATIVE_HANDLE_INPUT_POINTER_CAPTURE, name, false, out); }
static int pointer_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_INPUT_POINTER_CAPTURE); }
static int pointer_read(solar_os_native_handle_t id, solar_os_input_pointer_sample_t *sample, bool *reset)
{
    if (!sample || !reset) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *sample = (solar_os_input_pointer_sample_t){0}; *reset = false;
    native_input_capture_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_INPUT_POINTER_CAPTURE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_input_read_captured_pointer(state->source, state, sample, reset));
    solar_os_native_handle_release(id);
    return result;
}


typedef struct {
    uint32_t kind, lease;
    solar_os_native_handle_t handle;
    bool registered;
    union { solar_os_input_gesture_observer_t observer; solar_os_input_pointer_filter_t filter; } callback;
    void *user;
} native_input_callback_t;
static esp_err_t input_callback_retire(native_input_callback_t *state);
static void input_observer_callback(const solar_os_input_gesture_event_t *event, void *user)
{
    native_input_callback_t *state = user;
    if (!solar_os_native_handle_acquire(state->handle, state->kind)) return;
    state->callback.observer(event, state->user);
    solar_os_native_handle_release(state->handle);
}
static bool input_filter_callback(const solar_os_input_pointer_event_t *event, void *user)
{
    native_input_callback_t *state = user;
    if (!solar_os_native_handle_acquire(state->handle, state->kind)) return false;
    bool consumed = state->callback.filter(event, state->user);
    solar_os_native_handle_release(state->handle);
    return consumed;
}
static esp_err_t input_callback_retire(native_input_callback_t *state)
{
    if (!state->registered) return ESP_OK;
    return state->kind == SOLAR_OS_NATIVE_HANDLE_INPUT_OBSERVER ?
        solar_os_input_gesture_observer_try_unregister(input_observer_callback, state) :
        solar_os_input_pointer_filter_try_unregister(input_filter_callback, state);
}
static int input_callback_destroy(void *value)
{
    native_input_callback_t *state = value;
    esp_err_t error = input_callback_retire(state);
    if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    state->registered = false;
    if (state->lease) {
        int result = solar_os_native_library_release(state->lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}
static int input_callback_create(uint32_t kind, uint32_t lease, void *user,
    solar_os_input_gesture_observer_t observer, solar_os_input_pointer_filter_t filter, solar_os_native_handle_t *out)
{
    *out = 0;
    native_input_callback_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.input.callback");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->kind = kind; state->user = user;
    if (kind == SOLAR_OS_NATIVE_HANDLE_INPUT_OBSERVER) state->callback.observer = observer;
    else state->callback.filter = filter;
    int result = solar_os_native_library_retain(lease, &state->lease);
    if (!result) result = solar_os_native_handle_create(kind, state, input_callback_destroy, out);
    if (result) { (void)input_callback_destroy(state); return result; }
    state->handle = *out;
    (void)solar_os_native_handle_acquire(*out, kind);
    /* Publishing makes callbacks possible before register returns. Initialize
     * retirement state first; a failed registration publishes no callbacks. */
    state->registered = true;
    result = solar_os_native_result(kind == SOLAR_OS_NATIVE_HANDLE_INPUT_OBSERVER ?
        solar_os_input_gesture_observer_register(input_observer_callback, state) :
        solar_os_input_pointer_filter_register(input_filter_callback, state));
    if (result) state->registered = false;
    solar_os_native_handle_release(*out);
    if (result) { (void)solar_os_native_handle_close_kind(*out, kind); *out = 0; }
    return result;
}
static int observer_open(const solar_os_native_input_observer_v1_t *in, solar_os_native_handle_t *out)
{
    if (out) *out = 0;
    if (!out || !in || in->struct_size < sizeof(*in) || !in->library_lease || !in->observer)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return input_callback_create(SOLAR_OS_NATIVE_HANDLE_INPUT_OBSERVER, in->library_lease, in->user, in->observer, NULL, out);
}
static int filter_open(const solar_os_native_input_filter_v1_t *in, solar_os_native_handle_t *out)
{
    if (out) *out = 0;
    if (!out || !in || in->struct_size < sizeof(*in) || !in->library_lease || !in->filter)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return input_callback_create(SOLAR_OS_NATIVE_HANDLE_INPUT_FILTER, in->library_lease, in->user, NULL, in->filter, out);
}
static int input_callback_close(solar_os_native_handle_t id, uint32_t kind)
{
    /* Retire even while the callback holds its handle reference. This stops
     * hot producers from continuously admitting new callback deliveries. */
    native_input_callback_t *state = solar_os_native_handle_acquire(id, kind);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t error = input_callback_retire(state);
    solar_os_native_handle_release(id);
    if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    return solar_os_native_handle_close_kind(id, kind);
}
static int observer_close(solar_os_native_handle_t id)
{ return input_callback_close(id, SOLAR_OS_NATIVE_HANDLE_INPUT_OBSERVER); }
static int filter_close(solar_os_native_handle_t id)
{ return input_callback_close(id, SOLAR_OS_NATIVE_HANDLE_INPUT_FILTER); }

static const solar_os_native_input_api_v1_t input_api = {
    .abi_version = SOLAR_OS_NATIVE_INPUT_ABI, .struct_size = sizeof(input_api),
    .key_name = solar_os_key_name, .parse_key = solar_os_key_parse,
    .composition_modifiers = solar_os_input_composition_modifiers,
    .composition_apply = solar_os_input_composition_apply,
    .composition_apply_with_modifiers = solar_os_input_composition_apply_with_modifiers,
    .producer_modifiers = producer_modifiers,
    .observer_open = observer_open, .observer_close = observer_close,
    .filter_open = filter_open, .filter_close = filter_close,
    .keyboard_open = keyboard_open,
    .keyboard_claimed = keyboard_claimed,
    .keyboard_read = keyboard_read,
    .keyboard_close = keyboard_close,
    .pointer_open = pointer_open,
    .pointer_read = pointer_read,
    .pointer_close = pointer_close,
    .producer_open = producer_open,
    .producer_info = producer_info,
    .producer_ready = producer_ready,
    .producer_release_all = producer_release_all,
    .producer_write_key = producer_write_key,
    .producer_set_key = producer_set_key,
    .producer_write_key_tap = producer_write_key_tap,
    .producer_write_char = producer_write_char,
    .producer_write_pointer = producer_write_pointer,
    .producer_write_pointer_sample = producer_write_pointer_sample,
    .producer_write_axis = producer_write_axis,
    .producer_write_gesture = producer_write_gesture,
    .producer_gestures = producer_gestures,
    .producer_calibration_default = producer_calibration_default,
    .producer_read_chars = producer_read_chars,
    .producer_close = producer_close,
    .keyboard_count = solar_os_input_keyboard_count,
    .source_count = solar_os_input_source_count,
    .source_get = solar_os_input_source_get,
    .source_get_info = solar_os_input_source_get_info,
    .source_find = solar_os_input_source_find,
    .source_get_diagnostics = solar_os_input_source_get_diagnostics,
    .source_class_name = solar_os_input_source_class_name,
    .pointer_mode_name = solar_os_input_pointer_mode_name,
    .pointer_action_name = solar_os_input_pointer_action_name,
    .axis_name = solar_os_input_axis_name,
    .gesture_name = solar_os_input_gesture_name,
    .gesture_direction_name = solar_os_input_gesture_direction_name,
    .parse_gesture = solar_os_input_parse_gesture,
    .parse_gesture_direction = solar_os_input_parse_gesture_direction,
    .pointer_is_captured = solar_os_input_pointer_is_captured,
    .pointer_apply_display_transform = input_pointer_apply_display_transform,
    .pointer_apply_orientation = input_pointer_apply_orientation,
    .pointer_calibration_get = input_pointer_calibration_get,
    .pointer_calibration_set = input_pointer_calibration_set,
    .pointer_calibration_reset = input_pointer_calibration_reset,
    .get_pressed = solar_os_input_get_pressed,
    .key_press_generation = solar_os_input_key_press_generation,
    .keyboard_layout = solar_os_input_keyboard_layout,
    .set_keyboard_layout = input_set_keyboard_layout,
    .set_keyboard_layout_list = input_set_keyboard_layout_list,
    .keyboard_layout_list = input_keyboard_layout_list,
    .keyboard_layout_name = solar_os_input_keyboard_layout_name,
    .parse_keyboard_layout = solar_os_input_parse_keyboard_layout,
    .parse_key_chord = solar_os_input_parse_key_chord,
    .translate_hid_usage = solar_os_input_translate_hid_usage,
    .translate_char_key = solar_os_input_translate_char_key,
    .key_text = input_key_text,
    .resolve_layout_key = input_resolve_layout_key,
    .translate_hid_usage_byte = solar_os_input_translate_hid_usage_byte,
    .compose_feed = input_compose_feed,
    .get_repeat = solar_os_input_get_repeat,
    .set_repeat = input_set_repeat,
};
#endif
const void *solar_os_native_input_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_input_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
