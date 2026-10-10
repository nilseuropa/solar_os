#pragma once
#include "solar_os_input_keymap_mapper.h"

/* TCA8418 matrix/wire compatibility adapter; the mapper itself uses IDs. */
#define SOLAR_OS_MATRIX_KEY_COUNT 80U
#define SOLAR_OS_MATRIX_KEY_RAW SOLAR_OS_INPUT_KEYMAP_RAW
#define SOLAR_OS_MATRIX_KEY_LAYER_TAP SOLAR_OS_INPUT_KEYMAP_LAYER_TAP
#define SOLAR_OS_MATRIX_KEY_ALT_BLOCK SOLAR_OS_INPUT_KEYMAP_ALT_BLOCK

typedef solar_os_input_keymap_key_t solar_os_matrix_key_t;
typedef solar_os_input_keymap_t solar_os_matrix_keyboard_map_t;
typedef solar_os_input_keymap_state_t solar_os_matrix_keyboard_state_t;
typedef solar_os_input_keymap_transition_t solar_os_matrix_key_transition_t;

esp_err_t solar_os_matrix_keyboard_init_map(solar_os_matrix_keyboard_map_t *map, unsigned rows, unsigned cols);
esp_err_t solar_os_matrix_keyboard_default_map(solar_os_matrix_keyboard_map_t *map, unsigned rows, unsigned cols);
esp_err_t solar_os_matrix_keyboard_validate_map(const solar_os_matrix_keyboard_map_t *map);
void solar_os_matrix_keyboard_release(solar_os_matrix_keyboard_state_t *state);
bool solar_os_matrix_keyboard_decode(solar_os_matrix_keyboard_state_t *state,
    const solar_os_matrix_keyboard_map_t *map, uint8_t raw, solar_os_matrix_key_transition_t *transition);
