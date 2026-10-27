#include "solar_os_input_composition.h"

#include "solar_os_board.h"

static volatile uint8_t source_modifiers[UINT8_MAX + 1U];

#ifdef SOLAR_OS_BOARD_INPUT_CHORDS
static const solar_os_input_chord_t board_chords[] = SOLAR_OS_BOARD_INPUT_CHORDS;
#endif

void solar_os_input_composition_set_modifiers(solar_os_input_source_t source,
                                              uint8_t modifiers)
{
    if (source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        source_modifiers[source] = modifiers;
    }
}

uint8_t solar_os_input_composition_modifiers(void)
{
    uint8_t modifiers = 0U;
    for (size_t i = 0; i < sizeof(source_modifiers); i++) {
        modifiers |= source_modifiers[i];
    }
    return modifiers;
}

uint8_t solar_os_input_composition_apply(uint8_t key)
{
    return solar_os_input_composition_apply_with_modifiers(
        key, solar_os_input_composition_modifiers());
}

uint8_t solar_os_input_composition_apply_with_modifiers(uint8_t key, uint8_t modifiers)
{
#ifdef SOLAR_OS_BOARD_INPUT_CHORDS
    for (size_t i = 0; i < sizeof(board_chords) / sizeof(board_chords[0]); i++) {
        /* Ctrl chords retain their normal meaning instead of falling through
         * to an Alt-only convenience mapping (for example Ctrl+Alt+Up). */
        if ((modifiers & SOLAR_OS_INPUT_MOD_CTRL) != 0U &&
            (board_chords[i].modifiers & SOLAR_OS_INPUT_MOD_CTRL) == 0U) continue;
        if (board_chords[i].input_key == key &&
            (modifiers & board_chords[i].modifiers) == board_chords[i].modifiers) {
            return board_chords[i].output_key;
        }
    }
#else
    (void)modifiers;
#endif
    return key;
}
