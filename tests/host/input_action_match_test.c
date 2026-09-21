#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_input_action_match.h"

int main(void)
{
    solar_os_input_action_binding_t binding = {
        .id = 1U,
        .gesture = SOLAR_OS_INPUT_GESTURE_FLICK,
        .direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,
        .cooldown_ms = 250U,
    };
    strcpy(binding.source, "skywriter0");
    solar_os_input_gesture_event_t event = {
        .gesture = SOLAR_OS_INPUT_GESTURE_FLICK,
        .direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,
    };

    assert(solar_os_input_action_matches(&binding,
                                         "skywriter0",
                                         &event,
                                         false,
                                         0U,
                                         1000U));
    assert(!solar_os_input_action_matches(&binding,
                                          "imu0",
                                          &event,
                                          false,
                                          0U,
                                          1000U));
    event.gesture = SOLAR_OS_INPUT_GESTURE_WAVE;
    assert(!solar_os_input_action_matches(&binding,
                                          "skywriter0",
                                          &event,
                                          false,
                                          0U,
                                          1000U));
    event.gesture = SOLAR_OS_INPUT_GESTURE_FLICK;
    event.direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST;
    assert(!solar_os_input_action_matches(&binding,
                                          "skywriter0",
                                          &event,
                                          false,
                                          0U,
                                          1000U));

    event.direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST;
    assert(!solar_os_input_action_matches(&binding,
                                          "skywriter0",
                                          &event,
                                          true,
                                          900U,
                                          1149U));
    assert(solar_os_input_action_matches(&binding,
                                         "skywriter0",
                                         &event,
                                         true,
                                         900U,
                                         1150U));

    binding.any_source = true;
    binding.any_direction = true;
    event.direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH;
    assert(solar_os_input_action_matches(&binding,
                                         "imu0",
                                         &event,
                                         false,
                                         0U,
                                         1000U));
    assert(!solar_os_input_action_matches(NULL,
                                          "imu0",
                                          &event,
                                          false,
                                          0U,
                                          1000U));
    assert(!solar_os_input_action_matches(&binding,
                                          NULL,
                                          &event,
                                          false,
                                          0U,
                                          1000U));
    assert(!solar_os_input_action_matches(&binding,
                                          "imu0",
                                          NULL,
                                          false,
                                          0U,
                                          1000U));

    puts("input action match tests: ok");
    return 0;
}
