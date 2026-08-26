#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

#define SOLAR_OS_INPUT_REPEAT_RATE_MIN 1U
#define SOLAR_OS_INPUT_REPEAT_RATE_MAX 60U
#define SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS 100U
#define SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS 2000U

/* USB HID keyboard modifier bits are canonical across keyboard transports. */
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

typedef uint8_t solar_os_input_source_t;

typedef enum {
    SOLAR_OS_INPUT_SOURCE_OTHER,
    SOLAR_OS_INPUT_SOURCE_KEYBOARD,
    SOLAR_OS_INPUT_SOURCE_TOUCH,
    SOLAR_OS_INPUT_SOURCE_MOUSE,
    SOLAR_OS_INPUT_SOURCE_JOYSTICK,
    SOLAR_OS_INPUT_SOURCE_DPAD,
    SOLAR_OS_INPUT_SOURCE_BUTTONS,
    SOLAR_OS_INPUT_SOURCE_CLASS_COUNT,
} solar_os_input_source_class_t;

typedef struct {
    solar_os_input_source_t source;
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_input_source_class_t source_class;
    uint32_t capabilities;
    bool ready;
} solar_os_input_source_info_t;

typedef enum {
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US,
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE,
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

esp_err_t solar_os_input_init(void);
esp_err_t solar_os_input_source_open_typed(const char *name,
                                           solar_os_input_source_class_t source_class,
                                           uint32_t capabilities,
                                           bool ready,
                                           solar_os_input_source_t *source);
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
const char *solar_os_input_source_class_name(solar_os_input_source_class_t source_class);
void solar_os_input_source_close(solar_os_input_source_t source);
void solar_os_input_source_release_all(solar_os_input_source_t source);

esp_err_t solar_os_input_write_key(solar_os_input_source_t source,
                                   uint16_t physical_key,
                                   uint16_t usage,
                                   uint8_t key,
                                   uint8_t modifiers,
                                   solar_os_input_key_action_t action);
/* Compatibility helper for sources that can only provide a character tap. */
esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char ch);
esp_err_t solar_os_input_write_pointer(solar_os_input_source_t source,
                                       const solar_os_input_pointer_event_t *event);
esp_err_t solar_os_input_write_axis(solar_os_input_source_t source,
                                    const solar_os_input_axis_event_t *event);

size_t solar_os_input_read_events(solar_os_input_key_event_t *events, size_t event_count);
size_t solar_os_input_read_pointer_events(solar_os_input_pointer_event_t *events,
                                          size_t event_count);
size_t solar_os_input_read_axis_events(solar_os_input_axis_event_t *events,
                                       size_t event_count);
size_t solar_os_input_read_chars(char *buffer, size_t buffer_len);
size_t solar_os_input_read_source_chars(solar_os_input_source_t source,
                                        char *buffer,
                                        size_t buffer_len);
size_t solar_os_input_get_pressed(solar_os_input_key_event_t *keys, size_t key_count);

solar_os_input_keyboard_layout_t solar_os_input_keyboard_layout(void);
esp_err_t solar_os_input_set_keyboard_layout(solar_os_input_keyboard_layout_t layout);
const char *solar_os_input_keyboard_layout_name(solar_os_input_keyboard_layout_t layout);
bool solar_os_input_parse_keyboard_layout(const char *name,
                                          solar_os_input_keyboard_layout_t *layout);
/* Translate a canonical USB HID keyboard usage with the active keymap. */
uint8_t solar_os_input_translate_hid_usage(uint16_t usage,
                                           uint8_t modifiers,
                                           bool caps_lock);

void solar_os_input_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms);
esp_err_t solar_os_input_set_repeat(uint16_t rate_cps, uint16_t delay_ms);
