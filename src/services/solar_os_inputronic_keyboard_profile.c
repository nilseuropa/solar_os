#include "solar_os_inputronic_keyboard_profile.h"
#include <string.h>
#include "solar_os_inputronic_keymap_generated.h"

/* Matrix wiring and printed legends: Soldered Inputronic KEYBOARD, SKU 333360.
 * Hardware protocol references (no upstream driver code is incorporated):
 * https://github.com/SolderedElectronics/SOLDERED-Inputronic-KEYBOARD-Arduino-Library
 * revision 1b77c3c6b19ccd12b82aac5d4a957e7e53ffd401, Inputronic-Keymap.h
 * and Inputronic-Shiftmap.h; TI TCA8418 datasheet SCPS215G.
 * Shared legends and mappings are in keymaps/profiles/inputronic-keyboard.json.
 * Standard FN1..FN6 entries retain physical identity without logical output;
 * custom compiled defaults can assign them a meaning.
 */
void solar_os_inputronic_keyboard_map(solar_os_matrix_keyboard_map_t *map)
{
    solar_os_matrix_keyboard_init_map(map, SOLAR_OS_INPUTRONIC_KEYMAP_ROWS,
                                    SOLAR_OS_INPUTRONIC_KEYMAP_COLS);
    for (unsigned layer = 0; layer < 2U; layer++)
        memcpy(map->keys[layer], solar_os_inputronic_default_keys[layer],
               sizeof(solar_os_inputronic_default_keys[layer]));
}
