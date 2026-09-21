#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "solar_os_input_actions.h"

bool solar_os_input_action_matches(
    const solar_os_input_action_binding_t *binding,
    const char *source_name,
    const solar_os_input_gesture_event_t *event,
    bool previously_triggered,
    uint64_t last_trigger_ms,
    uint64_t now_ms);
