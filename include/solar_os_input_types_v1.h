#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_INPUT_SOURCE_INVALID 0U
#define SOLAR_OS_INPUT_SOURCE_NAME_MAX 16U
#define SOLAR_OS_INPUT_PHYSICAL_NONE 0U
#define SOLAR_OS_INPUT_USAGE_NONE 0U
#define SOLAR_OS_INPUT_MAX_PRESSED_KEYS 24U
#define SOLAR_OS_INPUT_POINTER_TARGET_MAX 16U
#define SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY (1U << 0)
#define SOLAR_OS_INPUT_POINTER_BUTTON_SECONDARY (1U << 1)
#define SOLAR_OS_INPUT_POINTER_BUTTON_MIDDLE (1U << 2)
#define SOLAR_OS_INPUT_CAP_KEY_EVENTS (1U << 0)
#define SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE (1U << 1)
#define SOLAR_OS_INPUT_CAP_POINTER_RELATIVE (1U << 2)
#define SOLAR_OS_INPUT_CAP_POINTER_BUTTONS (1U << 3)
#define SOLAR_OS_INPUT_CAP_SCROLL (1U << 4)
#define SOLAR_OS_INPUT_CAP_AXIS_EVENTS (1U << 5)
#define SOLAR_OS_INPUT_CAP_GESTURE_EVENTS (1U << 6)
#define SOLAR_OS_INPUT_GESTURE_FLAG_EDGE (1U << 0)
#define SOLAR_OS_INPUT_GESTURE_FLAG_DOUBLE (1U << 1)
#define SOLAR_OS_INPUT_GESTURE_FLAG_HAND_PRESENT (1U << 2)
#define SOLAR_OS_INPUT_GESTURE_FLAG_HAND_HELD (1U << 3)
#define SOLAR_OS_INPUT_GESTURE_FLAG_HAND_INSIDE (1U << 4)
#define SOLAR_OS_INPUT_GESTURE_FLAG_IN_PROGRESS (1U << 5)
#define SOLAR_OS_INPUT_REPEAT_RATE_MIN 1U
#define SOLAR_OS_INPUT_REPEAT_RATE_MAX 60U
#define SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS 100U
#define SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS 2000U
/* Canonical USB HID keyboard modifier bits across transports. */
#define SOLAR_OS_INPUT_MOD_LEFT_CTRL 0x01U
#define SOLAR_OS_INPUT_MOD_LEFT_SHIFT 0x02U
#define SOLAR_OS_INPUT_MOD_LEFT_ALT 0x04U
#define SOLAR_OS_INPUT_MOD_LEFT_GUI 0x08U
#define SOLAR_OS_INPUT_MOD_RIGHT_CTRL 0x10U
#define SOLAR_OS_INPUT_MOD_RIGHT_SHIFT 0x20U
#define SOLAR_OS_INPUT_MOD_RIGHT_ALT 0x40U
#define SOLAR_OS_INPUT_MOD_RIGHT_GUI 0x80U
#define SOLAR_OS_INPUT_MOD_CTRL \
    (SOLAR_OS_INPUT_MOD_LEFT_CTRL | SOLAR_OS_INPUT_MOD_RIGHT_CTRL)
#define SOLAR_OS_INPUT_MOD_SHIFT \
    (SOLAR_OS_INPUT_MOD_LEFT_SHIFT | SOLAR_OS_INPUT_MOD_RIGHT_SHIFT)
#define SOLAR_OS_INPUT_MOD_ALT \
    (SOLAR_OS_INPUT_MOD_LEFT_ALT | SOLAR_OS_INPUT_MOD_RIGHT_ALT)
#define SOLAR_OS_INPUT_KEYBOARD_LAYOUTS_MAX 5U
#define SOLAR_OS_INPUT_KEYBOARD_LAYOUT_LIST_MAX (3U * SOLAR_OS_INPUT_KEYBOARD_LAYOUTS_MAX)
#define SOLAR_OS_INPUT_GESTURE_MASK(gesture) (1UL << (unsigned)(gesture))
#define SOLAR_OS_INPUT_GESTURE_MASK_ALL \
    ((1UL << (unsigned)SOLAR_OS_INPUT_GESTURE_COUNT) - 1UL)

typedef uint8_t solar_os_input_source_t;

typedef enum {
    SOLAR_OS_INPUT_SOURCE_OTHER,
    SOLAR_OS_INPUT_SOURCE_KEYBOARD,
    SOLAR_OS_INPUT_SOURCE_TOUCH,
    SOLAR_OS_INPUT_SOURCE_MOUSE,
    SOLAR_OS_INPUT_SOURCE_JOYSTICK,
    SOLAR_OS_INPUT_SOURCE_DPAD,
    SOLAR_OS_INPUT_SOURCE_BUTTONS,
    SOLAR_OS_INPUT_SOURCE_GESTURE,
    SOLAR_OS_INPUT_SOURCE_CLASS_COUNT,
} solar_os_input_source_class_t;

typedef struct {
    solar_os_input_source_t source;
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_input_source_class_t source_class;
    uint32_t capabilities;
    uint32_t gesture_mask;
    bool ready;
} solar_os_input_source_info_t;

typedef enum {
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US,
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE,
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA,
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR,
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_RO,
} solar_os_input_keyboard_layout_t;

typedef enum {
    SOLAR_OS_INPUT_KEY_PRESS,
    SOLAR_OS_INPUT_KEY_RELEASE,
    SOLAR_OS_INPUT_KEY_REPEAT,
} solar_os_input_key_action_t;

typedef struct {
    solar_os_input_source_t source;
    /* Stable, source-local identity. Zero identifies an untracked character tap. */
    uint16_t physical_key;
    /* Canonical USB HID keyboard usage when available, otherwise zero. */
    uint16_t usage;
    /* SolarOS logical key or translated character; zero is allowed. */
    uint8_t key;
    uint8_t modifiers;
    solar_os_input_key_action_t action;
    /* For a layout code (SOLAR_OS_KEY_LAYOUT_TEXT*): the keyboard layout in
     * force when the event was queued, plus one. Zero resolves with the
     * current layout. Set by the input queue, so a layout change between
     * queueing and reading does not change what a queued key meant. */
    uint8_t layout_tag;
    /* Consumed Ctrl+Space transition. The input service switches on press;
     * the desktop clears composition without forwarding this key to an app. */
    bool layout_switch;
} solar_os_input_key_event_t;

typedef enum {
    SOLAR_OS_INPUT_POINTER_ABSOLUTE,
    SOLAR_OS_INPUT_POINTER_RELATIVE,
} solar_os_input_pointer_mode_t;

typedef enum {
    SOLAR_OS_INPUT_POINTER_MOVE,
    SOLAR_OS_INPUT_POINTER_PRESS,
    SOLAR_OS_INPUT_POINTER_RELEASE,
} solar_os_input_pointer_action_t;

typedef struct {
    solar_os_input_source_t source;
    uint8_t pointer_id;
    uint8_t buttons;
    solar_os_input_pointer_mode_t mode;
    solar_os_input_pointer_action_t action;
    int16_t x;
    int16_t y;
    int16_t delta_x;
    int16_t delta_y;
    /* Empty routes through input focus; a name routes to that display session. */
    char target[SOLAR_OS_INPUT_POINTER_TARGET_MAX];
} solar_os_input_pointer_event_t;

/* Pointer sample metadata used for capture and transformations. */
typedef struct {
    solar_os_input_pointer_event_t event;
    uint32_t time_ms;
    uint8_t modifiers;
    bool modifiers_valid;
} solar_os_input_pointer_sample_t;

/* A filter returns true to consume a display-oriented pointer event. */
typedef bool (*solar_os_input_pointer_filter_t)(
    const solar_os_input_pointer_event_t *event,
    void *context);

typedef enum {
    SOLAR_OS_INPUT_AXIS_X,
    SOLAR_OS_INPUT_AXIS_Y,
    SOLAR_OS_INPUT_AXIS_Z,
    SOLAR_OS_INPUT_AXIS_RX,
    SOLAR_OS_INPUT_AXIS_RY,
    SOLAR_OS_INPUT_AXIS_RZ,
    SOLAR_OS_INPUT_AXIS_COUNT,
} solar_os_input_axis_t;

typedef struct {
    solar_os_input_source_t source;
    solar_os_input_axis_t axis;
    int16_t value;
    int32_t delta;
} solar_os_input_axis_event_t;

typedef enum {
    SOLAR_OS_INPUT_GESTURE_FLICK,
    SOLAR_OS_INPUT_GESTURE_CIRCLE,
    SOLAR_OS_INPUT_GESTURE_WAVE,
    SOLAR_OS_INPUT_GESTURE_HOLD,
    SOLAR_OS_INPUT_GESTURE_PRESENCE,
    SOLAR_OS_INPUT_GESTURE_TAP,
    SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP,
    SOLAR_OS_INPUT_GESTURE_AIRWHEEL,
    SOLAR_OS_INPUT_GESTURE_SWIPE,
    SOLAR_OS_INPUT_GESTURE_TILT,
    SOLAR_OS_INPUT_GESTURE_WAKE,
    SOLAR_OS_INPUT_GESTURE_FREE_FALL,
    SOLAR_OS_INPUT_GESTURE_EDGE_SWIPE,
    SOLAR_OS_INPUT_GESTURE_COUNT,
} solar_os_input_gesture_t;

typedef enum {
    SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_CENTER,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_CLOCKWISE,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_COUNTERCLOCKWISE,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_HORIZONTAL,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_VERTICAL,
    SOLAR_OS_INPUT_GESTURE_DIRECTION_COUNT,
} solar_os_input_gesture_direction_t;

typedef struct {
    solar_os_input_source_t source;
    solar_os_input_gesture_t gesture;
    solar_os_input_gesture_direction_t direction;
    uint32_t flags;
    int16_t value;
    uint32_t raw;
} solar_os_input_gesture_event_t;

/* A gesture observer borrows an event without consuming foreground input. */
typedef void (*solar_os_input_gesture_observer_t)(
    const solar_os_input_gesture_event_t *event,
    void *context);

typedef struct {
    int16_t min_x;
    int16_t max_x;
    int16_t min_y;
    int16_t max_y;
    uint16_t width;
    uint16_t height;
} solar_os_input_pointer_calibration_t;

typedef struct {
    uint32_t key_events;
    uint32_t pointer_events;
    uint32_t axis_events;
    uint32_t gesture_events;
    bool has_key;
    bool has_pointer;
    bool has_axis;
    bool has_gesture;
    solar_os_input_key_event_t last_key;
    solar_os_input_pointer_event_t last_pointer;
    solar_os_input_pointer_event_t last_pointer_raw;
    solar_os_input_axis_event_t last_axis;
    solar_os_input_gesture_event_t last_gesture;
    bool calibration_enabled;
    solar_os_input_pointer_calibration_t calibration;
} solar_os_input_source_diagnostics_t;

typedef struct {
    uint16_t codepoint; /* Unicode BMP code point, 0 = none */
    uint8_t dead;       /* dead key index + 1, 0 = not a dead key */
} solar_os_input_text_t;

typedef struct {
    uint8_t dead; /* pending dead key index + 1 */
} solar_os_input_compose_t;
