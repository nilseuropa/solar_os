#include "solar_os_input_action_match.h"

#include <string.h>

bool solar_os_input_action_matches(
    const solar_os_input_action_binding_t *binding,
    const char *source_name,
    const solar_os_input_gesture_event_t *event,
    bool previously_triggered,
    uint64_t last_trigger_ms,
    uint64_t now_ms)
{
    if (binding == NULL || source_name == NULL || event == NULL ||
        binding->gesture != event->gesture ||
        (!binding->any_source &&
         strcmp(binding->source, source_name) != 0) ||
        (!binding->any_direction &&
         binding->direction != event->direction)) {
        return false;
    }
    return !previously_triggered ||
        now_ms - last_trigger_ms >= binding->cooldown_ms;
}
