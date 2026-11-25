#pragma once
#include "solar_os_input_types_v1.h"

#define SOLAR_OS_INPUT_ACTION_COMMAND_MAX 192U

typedef struct {
    uint32_t id;
    bool any_source;
    char source[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_input_gesture_t gesture;
    bool any_direction;
    solar_os_input_gesture_direction_t direction;
    uint32_t cooldown_ms;
    uint32_t trigger_count;
    uint32_t dropped_count;
    char command[SOLAR_OS_INPUT_ACTION_COMMAND_MAX];
} solar_os_input_action_binding_t;
