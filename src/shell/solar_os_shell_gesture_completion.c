#include "solar_os_shell_gesture_completion.h"

#include <stdio.h>
#include <string.h>

static bool gesture_completion_starts_with(const char *value,
                                           const char *prefix)
{
    return strncmp(value, prefix, strlen(prefix)) == 0;
}

static void gesture_completion_emit(
    const char *candidate,
    const char *prefix,
    solar_os_shell_gesture_candidate_emitter_t emitter,
    void *context)
{
    if (gesture_completion_starts_with(candidate, prefix)) {
        emitter(candidate, context);
    }
}

static bool gesture_completion_source_selected(const char *selected_source,
                                               const char *source_name)
{
    return selected_source == NULL || strcmp(selected_source, "*") == 0 ||
        strcmp(selected_source, source_name) == 0;
}

void solar_os_shell_gesture_completion_emit(
    const char *prefix,
    const char *selected_source,
    size_t source_count,
    solar_os_shell_gesture_source_getter_t source_getter,
    void *source_context,
    solar_os_shell_gesture_candidate_emitter_t candidate_emitter,
    void *candidate_context)
{
    if (prefix == NULL || source_getter == NULL || candidate_emitter == NULL) {
        return;
    }

    if (gesture_completion_starts_with(prefix, "source=")) {
        gesture_completion_emit("source=*",
                                prefix,
                                candidate_emitter,
                                candidate_context);
        for (size_t i = 0; i < source_count; i++) {
            solar_os_input_source_info_t info;
            if (!source_getter(i, &info, source_context) ||
                (info.capabilities & SOLAR_OS_INPUT_CAP_GESTURE_EVENTS) == 0U) {
                continue;
            }
            char candidate[sizeof("source=") + SOLAR_OS_INPUT_SOURCE_NAME_MAX];
            (void)snprintf(candidate, sizeof(candidate), "source=%s", info.name);
            gesture_completion_emit(candidate,
                                    prefix,
                                    candidate_emitter,
                                    candidate_context);
        }
        return;
    }

    if (gesture_completion_starts_with(prefix, "gesture=")) {
        uint32_t gesture_mask = 0U;
        for (size_t i = 0; i < source_count; i++) {
            solar_os_input_source_info_t info;
            if (!source_getter(i, &info, source_context) ||
                (info.capabilities & SOLAR_OS_INPUT_CAP_GESTURE_EVENTS) == 0U ||
                !gesture_completion_source_selected(selected_source, info.name)) {
                continue;
            }
            gesture_mask |= info.gesture_mask;
        }
        for (int value = SOLAR_OS_INPUT_GESTURE_FLICK;
             value < SOLAR_OS_INPUT_GESTURE_COUNT;
             value++) {
            if ((gesture_mask & SOLAR_OS_INPUT_GESTURE_MASK(value)) == 0U) {
                continue;
            }
            char candidate[32];
            (void)snprintf(candidate,
                           sizeof(candidate),
                           "gesture=%s",
                           solar_os_input_gesture_name(
                               (solar_os_input_gesture_t)value));
            gesture_completion_emit(candidate,
                                    prefix,
                                    candidate_emitter,
                                    candidate_context);
        }
        return;
    }

    if (gesture_completion_starts_with(prefix, "direction=")) {
        static const char *const directions[] = {
            "direction=*", "direction=none", "direction=west",
            "direction=east", "direction=north", "direction=south",
            "direction=center", "direction=clockwise",
            "direction=counterclockwise", "direction=horizontal",
            "direction=vertical",
        };
        for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); i++) {
            gesture_completion_emit(directions[i],
                                    prefix,
                                    candidate_emitter,
                                    candidate_context);
        }
        return;
    }

    static const char *const options[] = {
        "source=", "gesture=", "direction=", "cooldown=", "--",
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        gesture_completion_emit(options[i],
                                prefix,
                                candidate_emitter,
                                candidate_context);
    }
}
