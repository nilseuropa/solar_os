#include "solar_os_input_keymap_mapper.h"

#include <string.h>
#include "solar_os_input.h"

static bool mapped(solar_os_input_keymap_key_t key)
{
    return key.usage != 0U || key.key != 0U || key.shifted_key != 0U || key.flags != 0U;
}

static bool valid_usage(uint16_t usage)
{
    return usage == 0U || (usage >= 4U && usage <= 0xa4U) ||
           (usage >= 0xe0U && usage <= 0xe7U);
}

esp_err_t solar_os_input_keymap_validate(const solar_os_input_keymap_t *map)
{
    if (map == NULL || map->slot_count < 1U || map->slot_count > SOLAR_OS_INPUT_KEYMAP_KEY_MAX ||
        (map->rows == 0U) != (map->cols == 0U) ||
        (map->rows != 0U && (map->first == 0U || map->stride < map->cols ||
         (uint32_t)map->first + (map->rows - 1U) * map->stride + map->cols - 1U > UINT16_MAX))) return ESP_ERR_INVALID_ARG;
    if (map->physical[0] != 0U) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 1; i <= SOLAR_OS_INPUT_KEYMAP_KEY_MAX; i++) {
        const uint16_t physical = map->physical[i];
        if (i > map->slot_count && physical != 0U) return ESP_ERR_INVALID_ARG;
        if (physical == 0U) continue;
        for (unsigned j = 1; j < i; j++)
            if (map->physical[j] == physical) return ESP_ERR_INVALID_ARG;
        if (map->rows != 0U && (physical < map->first ||
            (physical - map->first) / map->stride >= map->rows ||
            (physical - map->first) % map->stride >= map->cols)) return ESP_ERR_INVALID_ARG;
    }
    unsigned layer_keys = 0;
    for (unsigned layer = 0; layer < 2U; layer++) {
        if (mapped(map->keys[layer][0])) return ESP_ERR_INVALID_ARG;
        for (unsigned id = 1; id <= SOLAR_OS_INPUT_KEYMAP_KEY_MAX; id++) {
            const solar_os_input_keymap_key_t key = map->keys[layer][id];
            if ((map->physical[id] == 0U) &&
                mapped(key)) return ESP_ERR_INVALID_ARG;
            if (!valid_usage(key.usage) || (key.flags != 0U &&
                key.flags != SOLAR_OS_INPUT_KEYMAP_RAW &&
                key.flags != SOLAR_OS_INPUT_KEYMAP_LAYER_TAP &&
                key.flags != SOLAR_OS_INPUT_KEYMAP_ALT_BLOCK))
                return ESP_ERR_INVALID_ARG;
            if (key.flags == SOLAR_OS_INPUT_KEYMAP_RAW &&
                (key.usage != 0U || key.key != 0U || key.shifted_key != 0U))
                return ESP_ERR_INVALID_ARG;
            if (key.flags == SOLAR_OS_INPUT_KEYMAP_LAYER_TAP) {
                if (layer != 0U || key.usage >= 0xe0U || key.usage == 0x39U)
                    return ESP_ERR_INVALID_ARG;
                layer_keys++;
            }
        }
    }
    return layer_keys <= 1U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void solar_os_input_keymap_release(solar_os_input_keymap_state_t *state)
{
    if (state == NULL) return;
    const bool caps = state->caps_lock;
    memset(state, 0, sizeof(*state));
    state->caps_lock = caps;
}

bool solar_os_input_keymap_decode(solar_os_input_keymap_state_t *state,
                                     const solar_os_input_keymap_t *map,
                                     uint16_t physical, bool pressed, solar_os_input_keymap_transition_t *transition)
{
    if (state == NULL || map == NULL || transition == NULL) return false;
    memset(transition, 0, sizeof(*transition));
    const unsigned id = solar_os_input_keymap_find_slot(map, physical);
    if (id == 0U || state->held[id] == pressed) return false;
    solar_os_input_keymap_key_t key = state->pressed_keys[id];
    if (pressed) {
        key = map->keys[0][id];
        if (state->layer_key != 0U && key.flags != SOLAR_OS_INPUT_KEYMAP_LAYER_TAP) {
            if (mapped(map->keys[1][id])) key = map->keys[1][id];
        }
        if (!mapped(key)) return false;
        if (state->layer_key != 0U && key.flags != SOLAR_OS_INPUT_KEYMAP_LAYER_TAP)
            state->layer_used = true;
        if (key.flags == SOLAR_OS_INPUT_KEYMAP_ALT_BLOCK &&
            (state->modifiers & SOLAR_OS_INPUT_MOD_ALT) != 0U)
            key = (solar_os_input_keymap_key_t){.flags = SOLAR_OS_INPUT_KEYMAP_RAW};
        state->pressed_keys[id] = key;
    }
    state->held[id] = pressed;
    state->modifiers = 0U;
    for (unsigned i = 1; i <= SOLAR_OS_INPUT_KEYMAP_KEY_MAX; i++) {
        const uint16_t usage = state->pressed_keys[i].usage;
        if (state->held[i] && usage >= 0xe0U && usage <= 0xe7U)
            state->modifiers |= (uint8_t)(1U << (usage - 0xe0U));
    }
    if (key.usage == 0x39U && pressed) state->caps_lock = !state->caps_lock;
    transition->physical_key = physical;
    transition->pressed = pressed;
    transition->modifiers = state->modifiers;
    if (key.flags == SOLAR_OS_INPUT_KEYMAP_LAYER_TAP) {
        if (pressed) {
            state->layer_key = (uint16_t)id;
            state->layer_used = false;
        } else {
            if (!state->layer_used) {
                transition->tap_usage = key.usage;
                transition->tap_key = key.key;
            }
            state->layer_key = 0U;
            state->layer_used = false;
        }
        return true;
    }
    transition->usage = key.usage;
    if ((state->modifiers & (SOLAR_OS_INPUT_MOD_CTRL | SOLAR_OS_INPUT_MOD_ALT)) == 0U)
        transition->key = (state->modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U &&
            key.shifted_key != 0U ? key.shifted_key : key.key;
    return true;
}

unsigned solar_os_input_keymap_find_slot(const solar_os_input_keymap_t *map, uint16_t physical)
{
    if (map == NULL || physical == 0U || map->slot_count > SOLAR_OS_INPUT_KEYMAP_KEY_MAX) return 0U;
    for (unsigned i = 1; i <= map->slot_count; i++)
        if (map->physical[i] == physical) return i;
    return 0U;
}
