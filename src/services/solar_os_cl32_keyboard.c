#include "solar_os_cl32_keyboard.h"

#include <string.h>

#include "solar_os_input.h"
#include "solar_os_keys.h"

#define CL32_KEY_FN 101U
#define CL32_KEY_FILE 116U
#define CL32_KEY_MENU 118U
#define CL32_KEY_OK 119U
#define CL32_KEY_CANCEL 120U
#define CL32_KEY_SHIFT 127U

typedef struct {
    uint16_t usage;
    uint8_t key;
} cl32_key_mapping_t;

static void toggle_advance(solar_os_cl32_toggle_t *toggle)
{
    if (*toggle == SOLAR_OS_CL32_TOGGLE_OFF) {
        *toggle = SOLAR_OS_CL32_TOGGLE_ONCE;
    } else if (*toggle == SOLAR_OS_CL32_TOGGLE_ONCE) {
        *toggle = SOLAR_OS_CL32_TOGGLE_LOCKED;
    } else {
        *toggle = SOLAR_OS_CL32_TOGGLE_OFF;
    }
}

static bool base_mapping(uint8_t physical_key, cl32_key_mapping_t *mapping)
{
    if (mapping == NULL) {
        return false;
    }
    *mapping = (cl32_key_mapping_t) {0};

    if (physical_key >= 4U && physical_key <= 29U) {
        mapping->usage = physical_key;
        return true;
    }

    switch (physical_key) {
    case 40U:
    case 42U:
    case 43U:
    case 44U:
        mapping->usage = physical_key;
        return true;
    case 46U:
        mapping->usage = physical_key;
        mapping->key = '=';
        return true;
    case 55U:
        mapping->usage = physical_key;
        mapping->key = '.';
        return true;
    case 79U:
        mapping->usage = physical_key;
        mapping->key = SOLAR_OS_KEY_RIGHT;
        return true;
    case 80U:
        mapping->usage = physical_key;
        mapping->key = SOLAR_OS_KEY_LEFT;
        return true;
    case 81U:
        mapping->usage = physical_key;
        mapping->key = SOLAR_OS_KEY_DOWN;
        return true;
    case 82U:
        mapping->usage = physical_key;
        mapping->key = SOLAR_OS_KEY_UP;
        return true;
    case 84U:
        mapping->usage = physical_key;
        mapping->key = '/';
        return true;
    case 85U:
        mapping->usage = physical_key;
        mapping->key = '*';
        return true;
    case 86U:
        mapping->usage = physical_key;
        mapping->key = '-';
        return true;
    case 87U:
        mapping->usage = physical_key;
        mapping->key = '+';
        return true;
    case 89U:
    case 90U:
    case 91U:
    case 92U:
    case 93U:
    case 94U:
    case 95U:
    case 96U:
    case 97U:
        mapping->usage = physical_key;
        mapping->key = (uint8_t)('1' + physical_key - 89U);
        return true;
    case 98U:
        mapping->usage = physical_key;
        mapping->key = '0';
        return true;
    case CL32_KEY_OK:
        mapping->usage = 40U;
        return true;
    case CL32_KEY_CANCEL:
        mapping->usage = 41U;
        return true;
    default:
        return false;
    }
}

static bool fn_mapping(uint8_t physical_key, cl32_key_mapping_t *mapping)
{
    static const uint8_t letter_keys[] = {
        '|', '>', '~', ',', '@', '?', '\'', ':', '&', ';', '{', '}', ']', '[',
        '(', ')', '!', 0xa3U, '#', '$', '^', '<', '"', '\\', '%', 0,
    };

    if (mapping == NULL) {
        return false;
    }
    *mapping = (cl32_key_mapping_t) {0};

    if (physical_key >= 4U && physical_key <= 29U) {
        const uint8_t key = letter_keys[physical_key - 4U];
        if (key == 0U) {
            return false;
        }
        mapping->usage = physical_key;
        mapping->key = key;
        return true;
    }

    if (physical_key >= 89U && physical_key <= 98U) {
        const uint8_t function_index = physical_key - 89U;
        mapping->usage = (uint16_t)(0x3aU + function_index);
        mapping->key = (uint8_t)(SOLAR_OS_KEY_F1 + function_index);
        return true;
    }

    switch (physical_key) {
    case 79U:
        mapping->usage = 77U;
        mapping->key = SOLAR_OS_KEY_END;
        return true;
    case 80U:
        mapping->usage = 74U;
        mapping->key = SOLAR_OS_KEY_HOME;
        return true;
    case 81U:
        mapping->usage = 78U;
        mapping->key = SOLAR_OS_KEY_PAGE_DOWN;
        return true;
    case 82U:
        mapping->usage = 75U;
        mapping->key = SOLAR_OS_KEY_PAGE_UP;
        return true;
    default:
        return base_mapping(physical_key, mapping);
    }
}

void solar_os_cl32_keyboard_reset(solar_os_cl32_keyboard_t *keyboard)
{
    if (keyboard != NULL) {
        memset(keyboard, 0, sizeof(*keyboard));
    }
}

solar_os_cl32_key_result_t solar_os_cl32_keyboard_decode(
    solar_os_cl32_keyboard_t *keyboard,
    uint8_t wire_event,
    solar_os_cl32_key_transition_t *transition)
{
    if (keyboard == NULL || transition == NULL) {
        return SOLAR_OS_CL32_KEY_UNSUPPORTED;
    }

    const bool pressed = (wire_event & 0x80U) != 0U;
    const uint8_t physical_key = wire_event & 0x7fU;
    memset(transition, 0, sizeof(*transition));
    transition->physical_key = physical_key;
    transition->pressed = pressed;

    if (physical_key == 0U) {
        return SOLAR_OS_CL32_KEY_UNSUPPORTED;
    }
    if (physical_key == CL32_KEY_SHIFT) {
        if (pressed) {
            toggle_advance(&keyboard->shift);
        }
        return SOLAR_OS_CL32_KEY_NONE;
    }
    if (physical_key == CL32_KEY_FN) {
        if (pressed) {
            toggle_advance(&keyboard->fn);
        }
        return SOLAR_OS_CL32_KEY_NONE;
    }

    uint16_t modifier_usage = 0U;
    uint8_t modifier = 0U;
    if (physical_key == CL32_KEY_FILE) {
        modifier_usage = 0xe2U;
        modifier = SOLAR_OS_INPUT_MOD_LEFT_ALT;
    } else if (physical_key == CL32_KEY_MENU) {
        modifier_usage = 0xe0U;
        modifier = SOLAR_OS_INPUT_MOD_LEFT_CTRL;
    }
    if (modifier != 0U) {
        if (pressed) {
            keyboard->held_modifiers |= modifier;
        } else {
            keyboard->held_modifiers &= (uint8_t)~modifier;
        }
        transition->usage = modifier_usage;
        transition->modifiers = keyboard->held_modifiers;
        return SOLAR_OS_CL32_KEY_TRANSITION;
    }

    cl32_key_mapping_t mapping;
    if (!pressed) {
        if (!base_mapping(physical_key, &mapping)) {
            return SOLAR_OS_CL32_KEY_UNSUPPORTED;
        }
        transition->modifiers = keyboard->held_modifiers;
        return SOLAR_OS_CL32_KEY_TRANSITION;
    }

    const bool fn_active = keyboard->fn != SOLAR_OS_CL32_TOGGLE_OFF;
    const bool shift_active = keyboard->shift != SOLAR_OS_CL32_TOGGLE_OFF;
    const bool mapped = fn_active
        ? fn_mapping(physical_key, &mapping)
        : base_mapping(physical_key, &mapping);

    if (fn_active && keyboard->fn == SOLAR_OS_CL32_TOGGLE_ONCE) {
        keyboard->fn = SOLAR_OS_CL32_TOGGLE_OFF;
    } else if (!fn_active && shift_active &&
               keyboard->shift == SOLAR_OS_CL32_TOGGLE_ONCE) {
        keyboard->shift = SOLAR_OS_CL32_TOGGLE_OFF;
    }
    if (!mapped) {
        return SOLAR_OS_CL32_KEY_UNSUPPORTED;
    }

    transition->usage = mapping.usage;
    transition->key = mapping.key;
    transition->modifiers = keyboard->held_modifiers;
    if (!fn_active && shift_active &&
        physical_key >= 4U && physical_key <= 29U) {
        transition->modifiers |= SOLAR_OS_INPUT_MOD_LEFT_SHIFT;
    }
    return SOLAR_OS_CL32_KEY_TRANSITION;
}
