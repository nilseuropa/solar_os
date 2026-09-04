#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_CL32_TOGGLE_OFF,
    SOLAR_OS_CL32_TOGGLE_ONCE,
    SOLAR_OS_CL32_TOGGLE_LOCKED,
} solar_os_cl32_toggle_t;

typedef struct {
    solar_os_cl32_toggle_t shift;
    solar_os_cl32_toggle_t fn;
    uint8_t held_modifiers;
} solar_os_cl32_keyboard_t;

typedef struct {
    uint8_t physical_key;
    uint16_t usage;
    uint8_t key;
    uint8_t modifiers;
    bool pressed;
} solar_os_cl32_key_transition_t;

typedef enum {
    SOLAR_OS_CL32_KEY_NONE,
    SOLAR_OS_CL32_KEY_TRANSITION,
    SOLAR_OS_CL32_KEY_UNSUPPORTED,
} solar_os_cl32_key_result_t;

void solar_os_cl32_keyboard_reset(solar_os_cl32_keyboard_t *keyboard);
solar_os_cl32_key_result_t solar_os_cl32_keyboard_decode(
    solar_os_cl32_keyboard_t *keyboard,
    uint8_t wire_event,
    solar_os_cl32_key_transition_t *transition);
