#pragma once
#include "solar_os_input_types_v1.h"

#define SOLAR_OS_INPUT_KEYMAP_KEY_MAX 256U
#define SOLAR_OS_INPUT_KEYMAP_LAYER_MAX 3U
#define SOLAR_OS_INPUT_KEYMAP_RAW 1U
#define SOLAR_OS_INPUT_KEYMAP_LAYER_TAP 2U
#define SOLAR_OS_INPUT_KEYMAP_ALT_BLOCK 4U

/* layer is the target of a LAYER_TAP selector: 0 is the historical alias for
 * layer 1, 1 selects symbols, 2 selects the function layer. It is zero on
 * ordinary entries. */
typedef struct {
    uint16_t usage;
    uint8_t key, shifted_key, flags, layer;
} solar_os_input_keymap_key_t;

/* Slots are bounded storage indices, independent of physical key IDs.
 * A zero physical ID denotes an unused slot. Matrix metadata is optional. */
typedef struct {
    uint16_t slot_count;
    uint16_t physical[SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    solar_os_input_keymap_key_t keys[SOLAR_OS_INPUT_KEYMAP_LAYER_MAX][SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    uint16_t first, stride;
    uint8_t rows, cols;
} solar_os_input_keymap_t;

typedef struct {
    bool held[SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    solar_os_input_keymap_key_t pressed_keys[SOLAR_OS_INPUT_KEYMAP_KEY_MAX + 1U];
    bool caps_lock, layer_used;
    uint8_t modifiers, active_layer;
    uint16_t layer_key;
} solar_os_input_keymap_state_t;

typedef struct {
    uint16_t physical_key, usage;
    uint8_t key, modifiers;
    bool pressed;
    uint16_t tap_usage;
    uint8_t tap_key;
} solar_os_input_keymap_transition_t;

#define SOLAR_OS_INPUT_KEYMAP_PHYSICAL (1U << 0)
#define SOLAR_OS_INPUT_KEYMAP_MODIFIERS (1U << 1)
#define SOLAR_OS_INPUT_KEYMAP_LAYERS (1U << 2)
#define SOLAR_OS_INPUT_KEYMAP_TAP_HOLD (1U << 3)
#define SOLAR_OS_INPUT_KEYMAP_MATRIX (1U << 4)

typedef struct {
    solar_os_input_source_t source;
    uint32_t capabilities;
    uint16_t key_count, first, stride;
    uint8_t rows, cols;
} solar_os_input_keymap_info_t;
