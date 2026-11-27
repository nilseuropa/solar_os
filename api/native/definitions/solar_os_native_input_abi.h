#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_input_types_v1.h"

#define SOLAR_OS_NATIVE_INPUT_SERVICE "input"
#define SOLAR_OS_NATIVE_INPUT_ABI 1U

/* The lease must protect callback code and any library-owned context. The
 * service retains its own lease until close succeeds. Keep other user data
 * alive until then. Callbacks run in the publishing/filtering task, without an
 * application scope; they must not block or retain the borrowed event. */
typedef struct {
    uint32_t struct_size, library_lease;
    solar_os_input_gesture_observer_t observer;
    void *user;
} solar_os_native_input_observer_v1_t;
typedef struct {
    uint32_t struct_size, library_lease;
    solar_os_input_pointer_filter_t filter;
    void *user;
} solar_os_native_input_filter_v1_t;

/* Global input values, source diagnostics, keyboard preferences and coordinate
 * transforms. Discovery IDs are ephemeral source identifiers, not owned
 * producer handles. Queries copy values into caller-owned records and retain
 * no pointers. get_pressed observes held keys without consuming foreground
 * events. Layout and repeat setters use the core's persistent preferences.
 * A zero repeat rate disables repeat; a zero delay retains the current delay.
 * Transforms and composition operate only on the caller's value records.
 * OS event pumping and boot resolver installation remain kernel operations. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*keyboard_count)(void);
    size_t (*source_count)(void);
    bool (*source_get)(size_t index, solar_os_input_source_info_t *info);
    bool (*source_get_info)(solar_os_input_source_t source, solar_os_input_source_info_t *info);
    bool (*source_find)(const char *name, solar_os_input_source_info_t *info);
    bool (*source_get_diagnostics)(solar_os_input_source_t source, solar_os_input_source_diagnostics_t *diagnostics);
    const char * (*source_class_name)(solar_os_input_source_class_t source_class);
    const char * (*pointer_mode_name)(solar_os_input_pointer_mode_t mode);
    const char * (*pointer_action_name)(solar_os_input_pointer_action_t action);
    const char * (*axis_name)(solar_os_input_axis_t axis);
    const char * (*gesture_name)(solar_os_input_gesture_t gesture);
    const char * (*gesture_direction_name)(solar_os_input_gesture_direction_t direction);
    bool (*parse_gesture)(const char *name, solar_os_input_gesture_t *gesture);
    bool (*parse_gesture_direction)(const char *name, solar_os_input_gesture_direction_t *direction);
    bool (*pointer_is_captured)(solar_os_input_source_t source);
    int (*pointer_apply_display_transform)(solar_os_input_pointer_event_t *event, uint16_t width, uint16_t height, uint16_t orientation_degrees, uint8_t scale);
    int (*pointer_apply_orientation)(solar_os_input_pointer_event_t *event, uint16_t width, uint16_t height, uint16_t orientation_degrees);
    int (*pointer_calibration_get)(solar_os_input_source_t source, bool *enabled, solar_os_input_pointer_calibration_t *calibration);
    int (*pointer_calibration_set)(solar_os_input_source_t source, const solar_os_input_pointer_calibration_t *calibration);
    int (*pointer_calibration_reset)(solar_os_input_source_t source);
    size_t (*get_pressed)(solar_os_input_key_event_t *keys, size_t key_count);
    uint32_t (*key_press_generation)(void);
    solar_os_input_keyboard_layout_t (*keyboard_layout)(void);
    int (*set_keyboard_layout)(solar_os_input_keyboard_layout_t layout);
    int (*set_keyboard_layout_list)(const char *text);
    int (*keyboard_layout_list)(char *text, size_t text_len);
    const char * (*keyboard_layout_name)(solar_os_input_keyboard_layout_t layout);
    bool (*parse_keyboard_layout)(const char *name, solar_os_input_keyboard_layout_t *layout);
    bool (*parse_key_chord)(const char *text, uint8_t *key, uint8_t *modifiers);
    uint8_t (*translate_hid_usage)(uint16_t usage, uint8_t modifiers, bool caps_lock);
    uint8_t (*translate_char_key)(uint8_t key, uint8_t modifiers);
    int (*key_text)(uint8_t key, solar_os_input_text_t *text);
    int (*resolve_layout_key)(solar_os_input_key_event_t *event, solar_os_input_text_t *text);
    uint8_t (*translate_hid_usage_byte)(uint16_t usage, uint8_t modifiers, bool caps_lock);
    size_t (*compose_feed)(solar_os_input_compose_t *state, const solar_os_input_text_t *text, uint16_t out[2]);
    void (*get_repeat)(uint16_t *rate_cps, uint16_t *delay_ms);
    int (*set_repeat)(uint16_t rate_cps, uint16_t delay_ms);
    /* Producer handles own exclusive names. Publishing is thread-safe; close
     * refuses active calls and preserves final releases for the core consumer.
     * Publish neutral axes before close; the core retains those queued values.
     * A callback-scoped creator binds the handle to that application's lifetime;
     * other creators must close it explicitly. No module callbacks are retained. */
    int (*producer_open)(const char *name, solar_os_input_source_class_t source_class,
                          uint32_t capabilities, bool ready, solar_os_native_handle_t *producer);
    int (*producer_info)(solar_os_native_handle_t producer, solar_os_input_source_info_t *info);
    int (*producer_ready)(solar_os_native_handle_t producer, bool ready);
    int (*producer_release_all)(solar_os_native_handle_t producer);
    int (*producer_write_key)(solar_os_native_handle_t producer, uint16_t physical_key, uint16_t usage, uint8_t key, uint8_t modifiers, solar_os_input_key_action_t action);
    int (*producer_set_key)(solar_os_native_handle_t producer, uint16_t physical_key, uint16_t usage, uint8_t key, uint8_t modifiers);
    int (*producer_write_key_tap)(solar_os_native_handle_t producer, uint16_t physical_key, uint16_t usage, uint8_t key, uint8_t modifiers);
    int (*producer_write_char)(solar_os_native_handle_t producer, char ch);
    int (*producer_write_pointer)(solar_os_native_handle_t producer, const solar_os_input_pointer_event_t *event);
    int (*producer_write_pointer_sample)(solar_os_native_handle_t producer, const solar_os_input_pointer_sample_t *sample);
    int (*producer_write_axis)(solar_os_native_handle_t producer, const solar_os_input_axis_event_t *event);
    int (*producer_write_gesture)(solar_os_native_handle_t producer, const solar_os_input_gesture_event_t *event);
    int (*producer_gestures)(solar_os_native_handle_t producer, uint32_t gesture_mask);
    int (*producer_calibration_default)(solar_os_native_handle_t producer, const solar_os_input_pointer_calibration_t *calibration);
    size_t (*producer_read_chars)(solar_os_native_handle_t producer, char *buffer, size_t buffer_len);
    int (*producer_close)(solar_os_native_handle_t producer);
    /* Captures own core routing claims. Creation inside an app callback binds
     * cleanup to that app; other callers close explicitly. Reads retain no
     * output pointers. Keyboard reset is a successful read with an empty event.
     * A detached keyboard can deliver reset while the claim remains owned.
     * With preserve_shortcuts, focus changes revoke the claim. Pointer capture
     * requires a neutral contact/button boundary and returns INVALID_STATE
     * after detach. Inspect reset even after a NOT_FOUND pointer read. */
    int (*keyboard_open)(const char *name, bool preserve_shortcuts, solar_os_native_handle_t *capture);
    bool (*keyboard_claimed)(solar_os_native_handle_t capture);
    int (*keyboard_read)(solar_os_native_handle_t capture, solar_os_input_key_event_t *event, bool *reset);
    int (*keyboard_close)(solar_os_native_handle_t capture);
    int (*pointer_open)(const char *name, solar_os_native_handle_t *capture);
    int (*pointer_read)(solar_os_native_handle_t capture, solar_os_input_pointer_sample_t *sample, bool *reset);
    int (*pointer_close)(solar_os_native_handle_t capture);
    /* Close retires registration immediately. BUSY means pinned deliveries
     * still hold callback/context/code; keep the handle and retry. */
    int (*observer_open)(const solar_os_native_input_observer_v1_t *descriptor, solar_os_native_handle_t *observer);
    int (*observer_close)(solar_os_native_handle_t observer);
    int (*filter_open)(const solar_os_native_input_filter_v1_t *descriptor, solar_os_native_handle_t *filter);
    int (*filter_close)(solar_os_native_handle_t filter);
    const char *(*key_name)(uint8_t key);
    bool (*parse_key)(const char *name, uint8_t *key);
    /* Board chord composition aggregates independent source modifier masks.
     * Set only your producer's mask; release_all and close clear it. Queries
     * snapshot under the input service lock, without consuming events or
     * allocating memory. */
    uint8_t (*composition_modifiers)(void);
    uint8_t (*composition_apply)(uint8_t key);
    uint8_t (*composition_apply_with_modifiers)(uint8_t key, uint8_t modifiers);
    int (*producer_modifiers)(solar_os_native_handle_t producer, uint8_t modifiers);
} solar_os_native_input_api_v1_t;
