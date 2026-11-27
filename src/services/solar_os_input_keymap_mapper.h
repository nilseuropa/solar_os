#pragma once

#include "solar_os_input.h"

#include "solar_os_input_keymap_types_v1.h"

unsigned solar_os_input_keymap_find_slot(const solar_os_input_keymap_t *map, uint16_t physical);
esp_err_t solar_os_input_keymap_init_matrix(solar_os_input_keymap_t *map,
    uint32_t rows, uint32_t cols, uint32_t first, uint32_t stride);
esp_err_t solar_os_input_keymap_validate(const solar_os_input_keymap_t *map);
void solar_os_input_keymap_release(solar_os_input_keymap_state_t *state);
bool solar_os_input_keymap_decode(solar_os_input_keymap_state_t *state,
    const solar_os_input_keymap_t *map, uint16_t physical, bool pressed,
    solar_os_input_keymap_transition_t *transition);
