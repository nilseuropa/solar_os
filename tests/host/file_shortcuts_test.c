#include <assert.h>
#include <stdio.h>

#include "solar_os_file_shortcuts.h"

static solar_os_input_key_event_t key_event(uint16_t usage,
                                            uint8_t key,
                                            uint8_t modifiers)
{
    return (solar_os_input_key_event_t) {
        .usage = usage,
        .key = key,
        .modifiers = modifiers,
        .action = SOLAR_OS_INPUT_KEY_PRESS,
    };
}

int main(void)
{
    solar_os_input_key_event_t event = key_event(
        SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE,
        's',
        SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_SEARCH);

    event = key_event(SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE,
                      0U,
                      SOLAR_OS_INPUT_MOD_RIGHT_ALT);
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_SEARCH);

    event = key_event(SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE, 's', 0U);
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_NONE);

    event = key_event(0x14U, '@', SOLAR_OS_INPUT_MOD_RIGHT_ALT);
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_NONE);

    event = key_event(SOLAR_OS_FILE_SHORTCUT_ENTER_USAGE,
                      SOLAR_OS_KEY_ENTER,
                      SOLAR_OS_INPUT_MOD_RIGHT_ALT);
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_FULLSCREEN);

    event.action = SOLAR_OS_INPUT_KEY_RELEASE;
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_NONE);

    const uint8_t search_modifiers[] = {
        SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_MOD_RIGHT_CTRL,
        SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_MOD_RIGHT_ALT,
    };
    for (size_t i = 0; i < sizeof(search_modifiers); i++) {
        event = key_event(SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE,
                          's', search_modifiers[i]);
        assert(solar_os_file_shortcut_from_key_event(&event) ==
               SOLAR_OS_FILE_SHORTCUT_SEARCH);
        event.action = SOLAR_OS_INPUT_KEY_REPEAT;
        event.key = 0x13U;
        assert(solar_os_file_shortcut_from_key_event(&event) ==
               SOLAR_OS_FILE_SHORTCUT_SEARCH);
    }
    event = key_event(SOLAR_OS_FILE_SHORTCUT_ENTER_USAGE,
                      SOLAR_OS_KEY_ENTER, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_file_shortcut_from_key_event(&event) ==
           SOLAR_OS_FILE_SHORTCUT_NONE);

    puts("file shortcut tests: ok");
    return 0;
}
