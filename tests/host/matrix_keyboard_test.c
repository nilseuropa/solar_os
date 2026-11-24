#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_input.h"
#include "solar_os_key_codes.h"
#include "solar_os_matrix_keyboard_json.h"
#include "solar_os_inputronic_keyboard_profile.h"
#include "solar_os_lilygo_pager_keyboard.h"

static solar_os_matrix_key_transition_t decode(solar_os_matrix_keyboard_state_t *state,
    const solar_os_matrix_keyboard_map_t *map, unsigned id, bool pressed)
{
    solar_os_matrix_key_transition_t event;
    assert(solar_os_matrix_keyboard_decode(state, map,
        (uint8_t)(id | (pressed ? 0x80U : 0U)), &event));
    return event;
}

int main(void)
{
    solar_os_matrix_keyboard_map_t generic, inputronic, pager;
    assert(solar_os_matrix_keyboard_default_map(&generic, 8, 10) == ESP_OK);
    solar_os_inputronic_keyboard_map(&inputronic);
    solar_os_lilygo_pager_keyboard_map(&pager);
    assert(solar_os_matrix_keyboard_validate_map(&generic) == ESP_OK);
    assert(solar_os_matrix_keyboard_validate_map(&inputronic) == ESP_OK);
    assert(solar_os_matrix_keyboard_validate_map(&pager) == ESP_OK);
    assert(solar_os_matrix_keyboard_default_map(&generic, 0, 10) == ESP_ERR_INVALID_ARG);
    assert(solar_os_matrix_keyboard_default_map(&generic, 8, 11) == ESP_ERR_INVALID_ARG);
    assert(solar_os_matrix_keyboard_init_map(&generic, 255, 255) == ESP_ERR_INVALID_ARG);
    assert(solar_os_matrix_keyboard_default_map(&generic, 3, 3) == ESP_OK);
    solar_os_matrix_keyboard_state_t state = {0};
    solar_os_matrix_key_transition_t event;
    assert(decode(&state, &generic, 11, true).usage == 0x04); /* Row 1, col 0: stride 10. */
    assert(decode(&state, &generic, 11, false).usage == 0x04);
    assert(!solar_os_matrix_keyboard_decode(&state, &generic, 0x80 | 4, &event));
    assert(!solar_os_matrix_keyboard_decode(&state, &generic, 0x80 | 31, &event));
    assert(decode(&state, &generic, 21, true).modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(decode(&state, &generic, 21, false).modifiers == 0);

    /* Space is a tap; Space+Q selects '1' without an extra Space. A release
     * keeps the mapping used by its press even after the layer is released. */
    state = (solar_os_matrix_keyboard_state_t){0};
    assert(decode(&state, &pager, 31, true).usage == 0);
    assert(decode(&state, &pager, 31, false).tap_usage == 0x2c);
    decode(&state, &pager, 31, true);
    assert(decode(&state, &pager, 1, true).key == '1');
    assert(decode(&state, &pager, 31, false).tap_usage == 0);
    assert(decode(&state, &pager, 1, false).key == '1');

    assert(decode(&state, &pager, 29, true).modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(!state.caps_lock); /* Pager Caps is momentary, not Caps Lock. */
    assert(!solar_os_matrix_keyboard_decode(&state, &pager, 0x80 | 29, &event));
    assert(decode(&state, &pager, 29, false).modifiers == 0);
    decode(&state, &pager, 21, true); /* Alt */
    event = decode(&state, &pager, 26, true);
    assert(event.usage == 0 && event.key == 0); /* Reserved Alt+B. */
    decode(&state, &pager, 21, false);
    assert(decode(&state, &pager, 26, false).usage == 0);

    assert(!solar_os_matrix_keyboard_decode(&state, &pager, 0x80 | 32, &event));
    decode(&state, &pager, 31, true);
    assert(!solar_os_matrix_keyboard_decode(&state, &pager, 0x80 | 32, &event));
    assert(decode(&state, &pager, 31, false).tap_usage == 0x2c);

    /* The shipped Pager profile is unchanged. The optional override swaps
     * the selectors and supplies the custom navigation layer. */
    static const char *pager_override =
        "{\"schema\":1,"
        "\"keys\":[{\"physical\":21,\"layer_tap\":true},"
        "{\"physical\":26,\"usage\":5},"
        "{\"physical\":31,\"usage\":44,\"layer_tap\":true,\"layer\":2}],"
        "\"symbols\":[{\"physical\":21},{\"physical\":26,\"key\":33}],"
        "\"fn\":[{\"physical\":1,\"usage\":74},{\"physical\":3,\"usage\":77},"
        "{\"physical\":8,\"key\":40},{\"physical\":9,\"key\":41},"
        "{\"physical\":10,\"key\":124},{\"physical\":11,\"usage\":43},"
        "{\"physical\":16,\"usage\":80},{\"physical\":17,\"usage\":81},"
        "{\"physical\":18,\"usage\":82},{\"physical\":19,\"usage\":79},"
        "{\"physical\":20,\"key\":27},{\"physical\":22,\"usage\":226},"
        "{\"physical\":30,\"usage\":76}]}";
    solar_os_matrix_keyboard_map_t custom;
    assert(solar_os_matrix_keyboard_parse_map(pager_override, &pager, &custom) == ESP_OK);
    state = (solar_os_matrix_keyboard_state_t){0};
    decode(&state, &custom, 21, true);
    assert(decode(&state, &custom, 1, true).key == '1');
    assert(decode(&state, &custom, 21, false).tap_usage == 0);
    assert(decode(&state, &custom, 1, false).key == '1');
    assert(decode(&state, &custom, 26, true).key == 0); /* Left Alt was removed. */
    decode(&state, &custom, 26, false);

    state = (solar_os_matrix_keyboard_state_t){0};
    decode(&state, &custom, 31, true);
    assert(decode(&state, &custom, 16, true).usage == 80); /* H: Left. */
    assert(decode(&state, &custom, 16, false).usage == 80);
    assert(decode(&state, &custom, 11, true).usage == 43); /* A: Tab. */
    decode(&state, &custom, 11, false);
    const solar_os_matrix_key_transition_t alt = decode(&state, &custom, 22, true);
    assert(alt.usage == 0xe2 && (alt.modifiers & SOLAR_OS_INPUT_MOD_LEFT_ALT) != 0);
    decode(&state, &custom, 22, false);
    assert(decode(&state, &custom, 8, true).key == '(');
    decode(&state, &custom, 8, false);
    assert(decode(&state, &custom, 9, true).key == ')');
    decode(&state, &custom, 9, false);
    assert(decode(&state, &custom, 10, true).key == '|');
    decode(&state, &custom, 10, false);
    assert(decode(&state, &custom, 20, true).key == SOLAR_OS_KEY_ESCAPE);
    decode(&state, &custom, 20, false);
    assert(decode(&state, &custom, 31, false).tap_usage == 0);

    state = (solar_os_matrix_keyboard_state_t){0};
    decode(&state, &inputronic, 74, true); decode(&state, &inputronic, 74, false);
    assert(state.caps_lock);
    decode(&state, &inputronic, 75, true);
    assert(decode(&state, &inputronic, 32, true).key == '!');
    solar_os_matrix_keyboard_release(&state);
    assert(state.caps_lock && state.modifiers == 0 && !state.held[32]);
    assert(decode(&state, &inputronic, 78, true).usage == 0);

    /* Sparse overrides start from a supplied built-in map. Invalid files
     * never modify the destination, including duplicates and fractional IDs. */
    solar_os_matrix_keyboard_map_t parsed = generic;
    assert(solar_os_matrix_keyboard_parse_map(
        "{\"schema\":1,\"keys\":[{\"row\":1,\"col\":0,\"usage\":5}]}", &generic, &parsed) == ESP_OK);
    assert(parsed.keys[0][11].usage == 5 && parsed.keys[0][12].usage == generic.keys[0][12].usage);
    assert(generic.keys[0][11].usage == 4);
    const char *invalid[] = {
        "{}", "{\"schema\":2,\"keys\":[]}", "{\"schema\":1,\"keys\":null}",
        "{\"schema\":1,\"keys\":[{\"row\":0.5,\"col\":0,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"row\":3,\"col\":0,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":3,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":1}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":4,\"usage\":5}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0},{\"row\":0,\"col\":0}]}",
        "{\"schema\":1,\"keys\":[],\"schema\":1}",
        "{\"schema\":1,\"keys\":[],\"typo\":1}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"raw\":true,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"layer_tap\":true,\"raw\":true}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"key\":256}]}",
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":1e300}]}",
        "{\"schema\":1,\"keys\":[]} trailing", "[[[[[[]]]]]]",
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        solar_os_matrix_keyboard_map_t before = parsed;
        assert(solar_os_matrix_keyboard_parse_map(invalid[i], &generic, &parsed) == ESP_ERR_INVALID_ARG);
        assert(memcmp(&before, &parsed, sizeof(parsed)) == 0);
    }
    assert(solar_os_matrix_keyboard_parse_map(
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"raw\":true}]}",
        &generic, &parsed) == ESP_OK);
    assert(parsed.keys[0][1].flags == SOLAR_OS_MATRIX_KEY_RAW);
    assert(solar_os_matrix_keyboard_parse_map(
        "{\"schema\":1,\"symbols\":[{\"row\":0,\"col\":0,\"key\":64}]}",
        &pager, &parsed) == ESP_OK);
    state = (solar_os_matrix_keyboard_state_t){0};
    decode(&state, &parsed, 31, true); /* Space selects symbols by default. */
    assert(decode(&state, &parsed, 1, true).key == '@');
    decode(&state, &parsed, 1, false);
    decode(&state, &parsed, 31, false);
    assert(solar_os_matrix_keyboard_parse_map(
        "{\"schema\":1,\"keys\":[{\"physical\":31,\"usage\":44,"
        "\"layer_tap\":true,\"layer\":2}],"
        "\"fn\":[{\"row\":0,\"col\":0,\"key\":64}]}",
        &pager, &parsed) == ESP_OK);
    state = (solar_os_matrix_keyboard_state_t){0};
    decode(&state, &parsed, 31, true); /* Space selects fn after the override. */
    assert(decode(&state, &parsed, 1, true).key == '@');
    decode(&state, &parsed, 1, false);
    decode(&state, &parsed, 31, false);

    /* Two selectors are allowed when they target different layers. */
    assert(solar_os_matrix_keyboard_parse_map(
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"layer_tap\":true},"
        "{\"row\":0,\"col\":1,\"usage\":44,\"layer_tap\":true,\"layer\":2}],"
        "\"symbols\":[{\"row\":1,\"col\":0,\"key\":33}],"
        "\"fn\":[{\"row\":1,\"col\":0,\"key\":64}]}",
        &generic, &parsed) == ESP_OK);
    const char *invalid_layers[] = {
        "{\"schema\":1,\"keys\":[{\"physical\":1,\"layer_tap\":true,\"layer\":3}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":1,\"usage\":4,\"layer\":2}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":1,\"layer_tap\":true},"
            "{\"physical\":2,\"layer_tap\":true,\"layer\":1}]}",
        "{\"schema\":1,\"fn\":[{\"physical\":1,\"layer_tap\":true}]}",
        "{\"schema\":1,\"fn\":[{\"physical\":1,\"key\":33,\"layer\":2}]}",
    };
    for (unsigned i = 0; i < sizeof(invalid_layers)/sizeof(invalid_layers[0]); i++) {
        solar_os_matrix_keyboard_map_t before = parsed;
        assert(solar_os_matrix_keyboard_parse_map(invalid_layers[i], &generic, &parsed) == ESP_ERR_INVALID_ARG);
        assert(memcmp(&before, &parsed, sizeof(parsed)) == 0);
    }
    /* The Pager already has a symbol selector; a second one is rejected. */
    assert(solar_os_matrix_keyboard_parse_map(
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":44,\"layer_tap\":true}]}",
        &pager, &parsed) == ESP_ERR_INVALID_ARG);

    /* The neutral mapper neither unpacks wire bytes nor assumes a matrix or
     * a ten-column stride. IDs are 16-bit and slots are independent IDs. */
    solar_os_input_keymap_t neutral = {.slot_count=2};
    neutral.physical[1] = 50000; neutral.keys[0][1].usage = 4;
    neutral.physical[2] = 32768; neutral.keys[0][2].usage = 0xe1;
    assert(solar_os_input_keymap_validate(&neutral) == ESP_OK);
    state = (solar_os_input_keymap_state_t){0};
    assert(solar_os_input_keymap_decode(&state, &neutral, 32768, true, &event));
    assert(solar_os_input_keymap_decode(&state, &neutral, 50000, true, &event));
    assert(event.physical_key == 50000 && event.usage == 4 && event.modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(solar_os_input_keymap_parse(
        "{\"schema\":1,\"keys\":[{\"physical\":50000,\"usage\":5}]}", &neutral, &parsed) == ESP_OK);
    assert(parsed.keys[0][1].usage == 5);
    const char *bad_physical[] = {
        "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":65536,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":50000.5,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":0,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":50000,\"row\":0,\"col\":0,\"usage\":4}]}",
        "{\"schema\":1,\"keys\":[{\"physical\":50000,\"usage\":4},{\"physical\":50000,\"usage\":5}]}",
    };
    for (unsigned i = 0; i < sizeof(bad_physical)/sizeof(bad_physical[0]); i++) {
        solar_os_input_keymap_t before = parsed;
        assert(solar_os_input_keymap_parse(bad_physical[i], &neutral, &parsed) == ESP_ERR_INVALID_ARG);
        assert(memcmp(&before, &parsed, sizeof(parsed)) == 0);
    }
    neutral.physical[2] = 50000;
    assert(solar_os_input_keymap_validate(&neutral) == ESP_ERR_INVALID_ARG);
    neutral = (solar_os_input_keymap_t){.slot_count=9, .rows=3, .cols=3, .stride=3, .first=100};
    for (unsigned i = 1; i <= 9; i++) { neutral.physical[i] = (uint16_t)(99U+i); neutral.keys[0][i].usage = 4; }
    assert(solar_os_input_keymap_validate(&neutral) == ESP_OK);
    assert(solar_os_input_keymap_parse(
        "{\"schema\":1,\"keys\":[{\"row\":1,\"col\":0,\"usage\":5}]}", &neutral, &parsed) == ESP_OK);
    assert(parsed.keys[0][4].usage == 5 && parsed.physical[4] == 103);
    assert(solar_os_input_keymap_parse(
        "{\"schema\":1,\"keys\":[{\"row\":1,\"col\":0},{\"physical\":103}]}",
        &neutral, &parsed) == ESP_ERR_INVALID_ARG); /* Two selectors for one position. */
    neutral = (solar_os_input_keymap_t){.slot_count=256};
    for (unsigned i = 1; i <= 256; i++) { neutral.physical[i] = (uint16_t)(60000U+i); neutral.keys[0][i].usage = 4; }
    assert(solar_os_input_keymap_validate(&neutral) == ESP_OK);
    state = (solar_os_input_keymap_state_t){0};
    assert(solar_os_input_keymap_decode(&state, &neutral, 60256, true, &event) && event.usage == 4);
    puts("matrix keyboard tests: ok");
    return 0;
}
