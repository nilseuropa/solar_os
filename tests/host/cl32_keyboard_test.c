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

static void press_state_key(solar_os_cl32_keyboard_t *keyboard,
                            uint8_t physical_key)
{
    solar_os_cl32_key_transition_t transition;
    assert(decode(keyboard,
                  physical_key,
                  true,
                  &transition) == SOLAR_OS_CL32_KEY_NONE);
    assert(decode(keyboard,
                  physical_key,
                  false,
                  &transition) == SOLAR_OS_CL32_KEY_NONE);
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

    press_state_key(&keyboard, 127U);
    assert(keyboard.shift == SOLAR_OS_CL32_TOGGLE_ONCE);
    expect_transition(&keyboard,
                      4U,
                      true,
                      4U,
                      0U,
                      SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(keyboard.shift == SOLAR_OS_CL32_TOGGLE_OFF);
    expect_transition(&keyboard, 4U, false, 0U, 0U, 0U);

    press_state_key(&keyboard, 127U);
    press_state_key(&keyboard, 127U);
    assert(keyboard.shift == SOLAR_OS_CL32_TOGGLE_LOCKED);
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
    press_state_key(&keyboard, 127U);
    assert(keyboard.shift == SOLAR_OS_CL32_TOGGLE_OFF);

    press_state_key(&keyboard, 101U);
    assert(keyboard.fn == SOLAR_OS_CL32_TOGGLE_ONCE);
    expect_transition(&keyboard, 4U, true, 4U, '|', 0U);
    assert(keyboard.fn == SOLAR_OS_CL32_TOGGLE_OFF);

    press_state_key(&keyboard, 101U);
    press_state_key(&keyboard, 101U);
    assert(keyboard.fn == SOLAR_OS_CL32_TOGGLE_LOCKED);
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
    press_state_key(&keyboard, 101U);
    assert(keyboard.fn == SOLAR_OS_CL32_TOGGLE_OFF);

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

    solar_os_cl32_keyboard_reset(&keyboard);
    assert(keyboard.shift == SOLAR_OS_CL32_TOGGLE_OFF);
    assert(keyboard.fn == SOLAR_OS_CL32_TOGGLE_OFF);
    assert(keyboard.held_modifiers == 0U);

    puts("CL-32 keyboard tests: ok");
    return 0;
}
