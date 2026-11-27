#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_input_types_v1.h"

esp_err_t solar_os_input_init(void);
/* Install during runtime initialization. The callback must remain valid for
 * the runtime lifetime, be nonblocking, and may only signal the event consumer.
 * It runs in the publishing task after the input lock is released. */
void solar_os_input_set_wake_callback(void (*callback)(void));
esp_err_t solar_os_input_source_open_typed(const char *name,
                                           solar_os_input_source_class_t source_class,
                                           uint32_t capabilities,
                                           bool ready,
                                           solar_os_input_source_t *source);
/* Create an exclusive producer name. Neither another create nor a legacy
 * open may alias it while active. Failure clears source. Queued final releases
 * still keep a retired ID reserved until the core consumer drains them. */
esp_err_t solar_os_input_source_create(const char *name,
    solar_os_input_source_class_t source_class, uint32_t capabilities,
    bool ready, solar_os_input_source_t *source);
esp_err_t solar_os_input_source_open(const char *name, solar_os_input_source_t *source);
esp_err_t solar_os_input_key_source_open(const char *name,
                                         solar_os_input_source_class_t source_class,
                                         solar_os_input_source_t *source);
esp_err_t solar_os_input_pointer_source_open(const char *name,
                                             solar_os_input_source_t *source);
esp_err_t solar_os_input_touch_source_open(const char *name,
                                           solar_os_input_source_t *source);
esp_err_t solar_os_input_mouse_source_open(const char *name,
                                           solar_os_input_source_t *source);
esp_err_t solar_os_input_joystick_source_open(const char *name,
                                              solar_os_input_source_t *source);
esp_err_t solar_os_input_keyboard_source_open(const char *name,
                                              bool ready,
                                              solar_os_input_source_t *source);
esp_err_t solar_os_input_source_set_ready(solar_os_input_source_t source,
                                          bool ready);
esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source,
                                                   bool ready);
size_t solar_os_input_keyboard_count(void);
size_t solar_os_input_source_count(void);
bool solar_os_input_source_get(size_t index, solar_os_input_source_info_t *info);
bool solar_os_input_source_get_info(solar_os_input_source_t source,
                                    solar_os_input_source_info_t *info);
bool solar_os_input_source_find(const char *name, solar_os_input_source_info_t *info);
bool solar_os_input_source_get_diagnostics(
    solar_os_input_source_t source,
    solar_os_input_source_diagnostics_t *diagnostics);
const char *solar_os_input_source_class_name(solar_os_input_source_class_t source_class);
const char *solar_os_input_pointer_mode_name(solar_os_input_pointer_mode_t mode);
const char *solar_os_input_pointer_action_name(solar_os_input_pointer_action_t action);
const char *solar_os_input_axis_name(solar_os_input_axis_t axis);
const char *solar_os_input_gesture_name(solar_os_input_gesture_t gesture);
const char *solar_os_input_gesture_direction_name(
    solar_os_input_gesture_direction_t direction);
bool solar_os_input_parse_gesture(const char *name,
                                  solar_os_input_gesture_t *gesture);
bool solar_os_input_parse_gesture_direction(
    const char *name,
    solar_os_input_gesture_direction_t *direction);
/* Retains final key/pointer releases and queued neutral axes until consumed;
 * the source ID is not reused while those events remain pending. */
void solar_os_input_source_close(solar_os_input_source_t source);
void solar_os_input_source_release_all(solar_os_input_source_t source);

esp_err_t solar_os_input_write_key(solar_os_input_source_t source,
                                   uint16_t physical_key,
                                   uint16_t usage,
                                   uint8_t key,
                                   uint8_t modifiers,
                                   solar_os_input_key_action_t action);
/* Set a tracked synthetic key, releasing its old logical value before a change.
 * key=0 releases it. Queue pressure leaves both the held state and queue intact;
 * setting the same key/usage/modifiers preserves the existing repeat deadline. */
esp_err_t solar_os_input_set_key(solar_os_input_source_t source,
                                 uint16_t physical_key, uint16_t usage,
                                 uint8_t key, uint8_t modifiers);
/* Atomically enqueue a synthetic press and a fully released key event.
 * Character-tap sources use PHYSICAL_NONE and USAGE_NONE: assigning a physical
 * identity would advertise held-key semantics to consumers such as emulators. */
esp_err_t solar_os_input_write_key_tap(solar_os_input_source_t source,
                                       uint16_t physical_key,
                                       uint16_t usage,
                                       uint8_t key,
                                       uint8_t modifiers);
/* Compatibility helper for sources that can only provide a character tap. */
esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char ch);
esp_err_t solar_os_input_write_pointer(solar_os_input_source_t source,
                                       const solar_os_input_pointer_event_t *event);
esp_err_t solar_os_input_write_pointer_sample(solar_os_input_source_t source,
    const solar_os_input_pointer_sample_t *sample);
/* Queried outside the input lock when a touch contact starts; NULL uses 0 degrees. */
typedef uint16_t (*solar_os_input_display_orientation_resolver_t)(const char *target);
void solar_os_input_set_display_orientation_resolver(
    solar_os_input_display_orientation_resolver_t resolver);
/* Native pixel bounds for edge-swipe recognition, queried outside the input
 * lock at contact start. Calibrated source bounds take precedence when set. */
typedef bool (*solar_os_input_display_size_resolver_t)(const char *target,
    uint16_t *width, uint16_t *height);
void solar_os_input_set_display_size_resolver(
    solar_os_input_display_size_resolver_t resolver);
/* The runtime input pump advances timed recognition for all input sources.
 * Producers only submit ordinary input samples. */
void solar_os_input_poll(void);
/* Exclusive, source-specific capture before foreground/filter delivery. Claim
 * only at a neutral button/contact boundary. Release discards pending samples.
 * Read returns NOT_FOUND when empty, INVALID_STATE after source closure, and
 * reset=true after overflow. No application callbacks run under the input lock. */
esp_err_t solar_os_input_capture_pointer(const char *name, const void *owner,
                                         solar_os_input_source_t *source);
void solar_os_input_release_pointer(solar_os_input_source_t source, const void *owner);
bool solar_os_input_pointer_is_captured(solar_os_input_source_t source);
esp_err_t solar_os_input_read_captured_pointer(solar_os_input_source_t source,
    const void *owner, solar_os_input_pointer_sample_t *sample, bool *reset);
esp_err_t solar_os_input_pointer_apply_display_transform(
    solar_os_input_pointer_event_t *event, uint16_t width, uint16_t height,
    uint16_t orientation_degrees, uint8_t scale);
esp_err_t solar_os_input_pointer_apply_orientation(
    solar_os_input_pointer_event_t *event,
    uint16_t width,
    uint16_t height,
    uint16_t orientation_degrees);
esp_err_t solar_os_input_write_axis(solar_os_input_source_t source,
                                    const solar_os_input_axis_event_t *event);
esp_err_t solar_os_input_write_gesture(
    solar_os_input_source_t source,
    const solar_os_input_gesture_event_t *event);
esp_err_t solar_os_input_source_set_gestures(solar_os_input_source_t source,
                                             uint32_t gesture_mask);
esp_err_t solar_os_input_gesture_observer_register(
    solar_os_input_gesture_observer_t observer,
    void *context);
void solar_os_input_gesture_observer_unregister(
    solar_os_input_gesture_observer_t observer,
    void *context);
esp_err_t solar_os_input_pointer_calibration_get(
    solar_os_input_source_t source,
    bool *enabled,
    solar_os_input_pointer_calibration_t *calibration);
esp_err_t solar_os_input_pointer_calibration_set(
    solar_os_input_source_t source,
    const solar_os_input_pointer_calibration_t *calibration);
/* Producer fallback: applied only if no calibration is configured. Does not
 * write NVS or replace calibration restored for this source name. */
esp_err_t solar_os_input_pointer_calibration_default(solar_os_input_source_t source,
    const solar_os_input_pointer_calibration_t *calibration);
esp_err_t solar_os_input_pointer_calibration_reset(solar_os_input_source_t source);
esp_err_t solar_os_input_pointer_filter_register(
    solar_os_input_pointer_filter_t filter,
    void *context);
void solar_os_input_pointer_filter_unregister(
    solar_os_input_pointer_filter_t filter,
    void *context);
bool solar_os_input_pointer_filter_event(
    const solar_os_input_pointer_event_t *event);

size_t solar_os_input_read_events(solar_os_input_key_event_t *events, size_t event_count);
size_t solar_os_input_read_pointer_events(solar_os_input_pointer_event_t *events,
                                          size_t event_count);
size_t solar_os_input_read_axis_events(solar_os_input_axis_event_t *events,
                                       size_t event_count);
size_t solar_os_input_read_gesture_events(solar_os_input_gesture_event_t *events,
                                          size_t event_count);
size_t solar_os_input_read_chars(char *buffer, size_t buffer_len);
size_t solar_os_input_read_source_chars(solar_os_input_source_t source,
                                        char *buffer,
                                        size_t buffer_len);
size_t solar_os_input_get_pressed(solar_os_input_key_event_t *keys, size_t key_count);

/* Exclusive source capture before desktop shortcuts/character decoding. */
esp_err_t solar_os_input_capture_keyboard(const char *name, const void *owner);
/* Capture guest keys while leaving desktop session shortcuts to SolarOS. */
esp_err_t solar_os_input_capture_keyboard_with_shortcuts(const char *name, const void *owner);
void solar_os_input_release_keyboard(const void *owner);
bool solar_os_input_keyboard_captured_by(const void *owner);
/* A display focus change revokes session capture immediately. Explicit full
 * captures, such as a script's raw keyboard claim, keep their ownership. */
void solar_os_input_release_session_keyboard(void);
bool solar_os_input_capture_key_event(const solar_os_input_key_event_t *event);
/* Main-loop wake/idle accounting for events queued directly by drivers. */
/* Captured keyboard presses and pointer samples still count as user activity. */
bool solar_os_input_take_captured_activity(void);
/* Advances on new key presses, including captured keys and modifiers, even
 * when the event queue is full. Releases and autorepeat do not advance it. */
uint32_t solar_os_input_key_press_generation(void);
/* reset is delivered before queued keys after overflow, release-all or detach. */
bool solar_os_input_read_captured_key(const void *owner,
    solar_os_input_key_event_t *event, bool *reset);

solar_os_input_keyboard_layout_t solar_os_input_keyboard_layout(void);
esp_err_t solar_os_input_set_keyboard_layout(solar_os_input_keyboard_layout_t layout);
/* Ordered, unique comma-separated layouts. Saving selects the first layout;
 * Ctrl+Space cycles at runtime without writing NVS. A single layout disables it. */
esp_err_t solar_os_input_set_keyboard_layout_list(const char *text);
esp_err_t solar_os_input_keyboard_layout_list(char *text, size_t text_len);
const char *solar_os_input_keyboard_layout_name(solar_os_input_keyboard_layout_t layout);
bool solar_os_input_parse_keyboard_layout(const char *name,
                                          solar_os_input_keyboard_layout_t *layout);
/* Parse KEY or a modifier chord such as ALT+RIGHT. */
bool solar_os_input_parse_key_chord(const char *text,
                                    uint8_t *key,
                                    uint8_t *modifiers);
/* Translate a canonical USB HID keyboard usage with the active keymap. */
uint8_t solar_os_input_translate_hid_usage(uint16_t usage,
                                           uint8_t modifiers,
                                           bool caps_lock);
/* Encode Ctrl characters and modified navigation for text consumers.
 * Already-translated keys remain unchanged. */
uint8_t solar_os_input_translate_char_key(uint8_t key, uint8_t modifiers);

/* What a key types as text: one character, or a dead key. */

/* Text for a key byte: printable ASCII and Latin-1 letters from U+00C0. */
solar_os_input_text_t solar_os_input_key_text(uint8_t key);
/* Resolve SOLAR_OS_KEY_LAYOUT_TEXT(_CAPS) from the event's usage and
 * modifiers. event->key becomes the byte key consumers get (the character if
 * a key byte can carry it, a dead key's spacing character, a Caps Lock result
 * falls back to the character without Caps Lock, else 0). Returns the text;
 * other keys return solar_os_input_key_text(event->key). */
solar_os_input_text_t solar_os_input_resolve_layout_key(solar_os_input_key_event_t *event);
/* solar_os_input_translate_hid_usage() resolved to the byte key consumers get. */
uint8_t solar_os_input_translate_hid_usage_byte(uint16_t usage,
                                                uint8_t modifiers,
                                                bool caps_lock);

/* Dead key composition for text input. Zero-initialise. */

/* Feed one typed text value; writes the 0-2 code points to type to out. A
 * dead key waits for the next character; without a composition, its spacing
 * character is typed before that character. */
size_t solar_os_input_compose_feed(solar_os_input_compose_t *state,
                                   solar_os_input_text_t text,
                                   uint16_t out[2]);

void solar_os_input_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms);
esp_err_t solar_os_input_set_repeat(uint16_t rate_cps, uint16_t delay_ms);

/* Nonblocking retirement. INVALID_STATE means a pinned callback may still
 * finish; retain its context/code and retry. Retirement blocks new deliveries.
 * NOT_FOUND also proves no registration or in-flight callback remains. The
 * legacy void unregister functions retire without waiting; callers keep their
 * context/code alive until quiescent teardown is confirmed. */
esp_err_t solar_os_input_gesture_observer_try_unregister(
    solar_os_input_gesture_observer_t observer, void *context);
esp_err_t solar_os_input_pointer_filter_try_unregister(
    solar_os_input_pointer_filter_t filter, void *context);
