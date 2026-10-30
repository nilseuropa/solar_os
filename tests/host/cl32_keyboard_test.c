#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "solar_os_cl32_keyboard.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"

static solar_os_cl32_key_result_t decode(
    solar_os_cl32_keyboard_t *keyboard,
    uint8_t physical_key,
    bool pressed,
    solar_os_cl32_key_transition_t *transition)
{
    const uint8_t wire_event = physical_key | (pressed ? 0x80U : 0U);
    return solar_os_cl32_keyboard_decode(keyboard, wire_event, transition);
}

static void expect_transition(solar_os_cl32_keyboard_t *keyboard,
                              uint8_t physical_key,
                              bool pressed,
                              uint16_t usage,
                              uint8_t key,
                              uint8_t modifiers)
{
    solar_os_cl32_key_transition_t transition;
    assert(decode(keyboard,
                  physical_key,
                  pressed,
                  &transition) == SOLAR_OS_CL32_KEY_TRANSITION);
    assert(transition.physical_key == physical_key);
    assert(transition.pressed == pressed);
    assert(transition.usage == usage);
    assert(transition.key == key);
    assert(transition.modifiers == modifiers);
}

static void set_fn(solar_os_cl32_keyboard_t *keyboard, bool pressed)
{
    solar_os_cl32_key_transition_t transition;
    assert(decode(keyboard,
                  101U,
                  pressed,
                  &transition) == SOLAR_OS_CL32_KEY_NONE);
    assert(keyboard->fn_held == pressed);
}

static void expect_session_tap(solar_os_cl32_keyboard_t *keyboard,
                               uint8_t physical_key, uint16_t usage, uint8_t key)
{
    solar_os_cl32_key_transition_t transition;
    const uint8_t held_modifiers = keyboard->held_modifiers;
    assert(decode(keyboard, physical_key, true, &transition) == SOLAR_OS_CL32_KEY_TAP);
    assert(transition.physical_key == physical_key && transition.pressed);
    assert(transition.usage == usage && transition.key == key);
    assert(transition.modifiers == SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(keyboard->held_modifiers == held_modifiers);
    expect_transition(keyboard, physical_key, false, 0U, 0U, held_modifiers);
}

int main(void)
{
    solar_os_cl32_keyboard_t keyboard;
    solar_os_cl32_key_transition_t transition;
    solar_os_cl32_keyboard_reset(&keyboard);

    assert(solar_os_cl32_keyboard_decode(NULL, 0x84U, &transition) ==
           SOLAR_OS_CL32_KEY_UNSUPPORTED);
    assert(solar_os_cl32_keyboard_decode(&keyboard, 0x84U, NULL) ==
           SOLAR_OS_CL32_KEY_UNSUPPORTED);
    assert(decode(&keyboard, 0U, true, &transition) ==
           SOLAR_OS_CL32_KEY_UNSUPPORTED);

    expect_transition(&keyboard, 4U, true, 4U, 0U, 0U);
    expect_transition(&keyboard, 4U, false, 0U, 0U, 0U);
    expect_transition(&keyboard, 89U, true, 89U, '1', 0U);
    expect_transition(&keyboard, 98U, true, 98U, '0', 0U);
    expect_transition(&keyboard, 119U, true, 40U, 0U, 0U);
    expect_transition(&keyboard, 120U, true, 41U, 0U, 0U);

    expect_transition(&keyboard, 127U, true, 0xe1U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard,
                      4U,
                      true,
                      4U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard, 4U, false, 0U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard,
                      5U,
                      true,
                      5U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard,
                      6U,
                      true,
                      6U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard, 80U, true, 80U, SOLAR_OS_KEY_LEFT,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard, 127U, false, 0xe1U, 0U, 0U);
    expect_transition(&keyboard, 80U, false, 0U, 0U, 0U);
    expect_transition(&keyboard, 4U, true, 4U, 0U, 0U);

    /* Repeated presses must not toggle a held modifier off or latch it on. */
    for (unsigned i = 0; i < 3U; i++) {
        expect_transition(&keyboard, 127U, true, 0xe1U, 0U,
                          SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
        expect_transition(&keyboard, 127U, true, 0xe1U, 0U,
                          SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
        expect_transition(&keyboard, 127U, false, 0xe1U, 0U, 0U);
        expect_transition(&keyboard, 4U, true, 4U, 0U, 0U);
    }

    set_fn(&keyboard, true);
    expect_transition(&keyboard, 4U, true, 4U, '|', 0U);
    expect_transition(&keyboard, 5U, true, 5U, '>', 0U);
    expect_transition(&keyboard, 4U, false, 0U, 0U, 0U);
    set_fn(&keyboard, true);
    for (uint8_t i = 0U; i < 10U; i++) {
        expect_transition(&keyboard,
                          (uint8_t)(89U + i),
                          true,
                          (uint16_t)(0x3aU + i),
                          (uint8_t)(SOLAR_OS_KEY_F1 + i),
                          0U);
    }
    expect_transition(&keyboard, 79U, true, 77U, SOLAR_OS_KEY_END, 0U);
    expect_transition(&keyboard, 80U, true, 74U, SOLAR_OS_KEY_HOME, 0U);
    expect_transition(&keyboard, 81U, true, 78U, SOLAR_OS_KEY_PAGE_DOWN, 0U);
    expect_transition(&keyboard, 82U, true, 75U, SOLAR_OS_KEY_PAGE_UP, 0U);
    expect_session_tap(&keyboard, 119U, 80U, SOLAR_OS_KEY_LEFT);
    expect_session_tap(&keyboard, 120U, 79U, SOLAR_OS_KEY_RIGHT);
    /* Release Fn before its key: the key still has a physical release. */
    set_fn(&keyboard, false);
    expect_transition(&keyboard, 82U, false, 0U, 0U, 0U);
    expect_transition(&keyboard, 82U, true, 82U, SOLAR_OS_KEY_UP, 0U);
    expect_transition(&keyboard, 89U, true, 89U, '1', 0U);
    expect_transition(&keyboard, 4U, true, 4U, 0U, 0U);
    expect_transition(&keyboard, 119U, true, 40U, 0U, 0U);
    expect_transition(&keyboard, 120U, true, 41U, 0U, 0U);
    for (unsigned i = 0; i < 3U; i++) {
        set_fn(&keyboard, true);
        set_fn(&keyboard, false);
        expect_transition(&keyboard, 4U, true, 4U, 0U, 0U);
    }

    expect_transition(&keyboard,
                      116U,
                      true,
                      0xe2U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_ALT);
    expect_transition(&keyboard,
                      43U,
                      true,
                      43U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_ALT);
    expect_transition(&keyboard, 43U, false, 0U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_ALT);
    expect_transition(&keyboard, 116U, false, 0xe2U, 0U, 0U);

    expect_transition(&keyboard,
                      118U,
                      true,
                      0xe0U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    expect_transition(&keyboard,
                      6U,
                      true,
                      6U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    expect_transition(&keyboard, 6U, false, 0U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    expect_transition(&keyboard, 118U, false, 0xe0U, 0U, 0U);

    /* Releasing each modifier must preserve the others still held. */
    const uint8_t alt_ctrl = SOLAR_OS_INPUT_MOD_LEFT_ALT |
                            SOLAR_OS_INPUT_MOD_LEFT_CTRL;
    const uint8_t all_modifiers = alt_ctrl | SOLAR_OS_INPUT_MOD_LEFT_SHIFT;
    expect_transition(&keyboard, 116U, true, 0xe2U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_ALT);
    expect_transition(&keyboard, 118U, true, 0xe0U, 0U, alt_ctrl);
    expect_transition(&keyboard, 127U, true, 0xe1U, 0U, all_modifiers);
    set_fn(&keyboard, true);
    /* The session chord stays exact even with other physical modifiers held. */
    expect_session_tap(&keyboard, 119U, 80U, SOLAR_OS_KEY_LEFT);
    expect_session_tap(&keyboard, 120U, 79U, SOLAR_OS_KEY_RIGHT);
    expect_transition(&keyboard, 89U, true, 0x3aU, SOLAR_OS_KEY_F1,
                      all_modifiers);
    expect_transition(&keyboard, 127U, false, 0xe1U, 0U, alt_ctrl);
    expect_transition(&keyboard, 90U, true, 0x3bU, SOLAR_OS_KEY_F2, alt_ctrl);
    set_fn(&keyboard, false);
    expect_transition(&keyboard, 90U, false, 0U, 0U, alt_ctrl);
    expect_transition(&keyboard, 5U, true, 5U, 0U, alt_ctrl);
    expect_transition(&keyboard, 116U, false, 0xe2U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    expect_transition(&keyboard, 118U, false, 0xe0U, 0U, 0U);

    /* Releasing Fn first must leave Shift active for subsequent keys. */
    expect_transition(&keyboard, 127U, true, 0xe1U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    set_fn(&keyboard, true);
    expect_transition(&keyboard, 82U, true, 75U, SOLAR_OS_KEY_PAGE_UP,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    set_fn(&keyboard, false);
    expect_transition(&keyboard, 82U, false, 0U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    expect_transition(&keyboard, 4U, true, 4U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);

    /* Reset while held clears both the Fn layer and HID modifiers. */
    set_fn(&keyboard, true);
    expect_transition(&keyboard, 127U, true, 0xe1U, 0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    solar_os_cl32_keyboard_reset(&keyboard);
    assert(!keyboard.fn_held);
    assert(keyboard.held_modifiers == 0U);
    expect_transition(&keyboard, 4U, true, 4U, 0U, 0U);

    puts("CL-32 keyboard tests: ok");
    return 0;
}
