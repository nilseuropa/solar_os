#include "solar_os_matrix_keyboard.h"
#include "solar_os_tca8418_keymap_generated.h"

esp_err_t solar_os_matrix_keyboard_init_map(solar_os_matrix_keyboard_map_t *map,
                                       unsigned rows, unsigned cols)
{
    if (rows > 8U || cols > 10U) return ESP_ERR_INVALID_ARG;
    return solar_os_input_keymap_init_matrix(map, rows, cols, 1U, 10U);
}

esp_err_t solar_os_matrix_keyboard_default_map(solar_os_matrix_keyboard_map_t *map,
                                               unsigned rows, unsigned cols)
{
    if (map == NULL || rows < 1U || rows > SOLAR_OS_TCA8418_KEYMAP_ROWS ||
        cols < 1U || cols > SOLAR_OS_TCA8418_KEYMAP_COLS)
        return ESP_ERR_INVALID_ARG;
    /* Reference wiring, not a layout supplied by the controller. HID usages
     * let the normal SolarOS keyboard layout translate printable keys. */
    solar_os_matrix_keyboard_init_map(map, rows, cols);
    for (unsigned layer = 0; layer < SOLAR_OS_INPUT_KEYMAP_LAYER_MAX; layer++)
        for (unsigned id = 1; id <= map->slot_count; id++)
            if (map->physical[id] != 0U)
                map->keys[layer][id] = solar_os_tca8418_default_keys[layer][id];
    return ESP_OK;
}

esp_err_t solar_os_matrix_keyboard_validate_map(const solar_os_matrix_keyboard_map_t *map)
{
    return solar_os_input_keymap_validate(map);
}
void solar_os_matrix_keyboard_release(solar_os_matrix_keyboard_state_t *state)
{
    solar_os_input_keymap_release(state);
}
bool solar_os_matrix_keyboard_decode(solar_os_matrix_keyboard_state_t *state,
    const solar_os_matrix_keyboard_map_t *map, uint8_t raw,
    solar_os_matrix_key_transition_t *transition)
{
    return solar_os_input_keymap_decode(state, map, raw & 0x7fU,
                                        (raw & 0x80U) != 0U, transition);
}
