#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solar_os_input.h"

typedef bool (*solar_os_shell_gesture_source_getter_t)(
    size_t index,
    solar_os_input_source_info_t *info,
    void *context);
typedef void (*solar_os_shell_gesture_candidate_emitter_t)(
    const char *candidate,
    void *context);

void solar_os_shell_gesture_completion_emit(
    const char *prefix,
    const char *selected_source,
    size_t source_count,
    solar_os_shell_gesture_source_getter_t source_getter,
    void *source_context,
    solar_os_shell_gesture_candidate_emitter_t candidate_emitter,
    void *candidate_context);
