#pragma once

#include "solar_os_input.h"

#define SOLAR_OS_INPUT_KEYMAP_KEY_MAX 256U
#define SOLAR_OS_INPUT_KEYMAP_RAW 1U
#define SOLAR_OS_INPUT_KEYMAP_LAYER_TAP 2U
#define SOLAR_OS_INPUT_KEYMAP_ALT_BLOCK 4U

typedef struct {
    uint16_t usage;
    uint8_t key, shifted_key, flags;
} solar_os_input_keymap_key_t;

/* Slots are bounded storage indices, independent of physical key IDs.
 * A zero physical ID denotes an unused slot. Matrix metadata is optional. */
typedef struct {
    uint16_t slot_count;
    uint16_t physical[SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    solar_os_input_keymap_key_t keys[2][SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    uint16_t first, stride;
    uint8_t rows, cols;
} solar_os_input_keymap_t;

typedef struct {
    bool held[SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    solar_os_input_keymap_key_t pressed_keys[SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    bool caps_lock, layer_used;
    uint8_t modifiers;
    uint16_t layer_key;
} solar_os_input_keymap_state_t;

typedef struct {
    uint16_t physical_key, usage;
    uint8_t key, modifiers;
    bool pressed;
    uint16_t tap_usage;
    uint8_t tap_key;
} solar_os_input_keymap_transition_t;

unsigned solar_os_input_keymap_find_slot(const solar_os_input_keymap_t *map, uint16_t physical);
esp_err_t solar_os_input_keymap_validate(const solar_os_input_keymap_t *map);
void solar_os_input_keymap_release(solar_os_input_keymap_state_t *state);
bool solar_os_input_keymap_decode(solar_os_input_keymap_state_t *state,
    const solar_os_input_keymap_t *map, uint16_t physical, bool pressed,
    solar_os_input_keymap_transition_t *transition);
