#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_gesture_completion.h"

#define CANDIDATE_MAX 32U
#define CANDIDATE_LEN 40U

typedef struct {
    char values[CANDIDATE_MAX][CANDIDATE_LEN];
    size_t count;
} candidate_list_t;

static const solar_os_input_source_info_t sources[] = {
    {
        .source = 1U,
        .source_class = SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        .capabilities = SOLAR_OS_INPUT_CAP_KEY_EVENTS,
        .ready = true,
        .name = "keyboard0",
    },
    {
        .source = 2U,
        .source_class = SOLAR_OS_INPUT_SOURCE_GESTURE,
        .capabilities = SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,
        .gesture_mask =
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_FLICK) |
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_TAP),
        .ready = true,
        .name = "skywriter0",
    },
    {
        .source = 3U,
        .source_class = SOLAR_OS_INPUT_SOURCE_GESTURE,
        .capabilities = SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,
        .gesture_mask =
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_WAVE),
        .ready = true,
        .name = "imu0",
    },
};

const char *solar_os_input_gesture_name(solar_os_input_gesture_t gesture)
{
    static const char *const names[] = {
        [SOLAR_OS_INPUT_GESTURE_FLICK] = "flick",
        [SOLAR_OS_INPUT_GESTURE_CIRCLE] = "circle",
        [SOLAR_OS_INPUT_GESTURE_WAVE] = "wave",
        [SOLAR_OS_INPUT_GESTURE_HOLD] = "hold",
        [SOLAR_OS_INPUT_GESTURE_PRESENCE] = "presence",
        [SOLAR_OS_INPUT_GESTURE_TAP] = "tap",
        [SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP] = "double-tap",
        [SOLAR_OS_INPUT_GESTURE_AIRWHEEL] = "airwheel",
    };
    assert(gesture >= SOLAR_OS_INPUT_GESTURE_FLICK);
    assert(gesture < SOLAR_OS_INPUT_GESTURE_COUNT);
    return names[gesture];
}

static bool get_source(size_t index,
                       solar_os_input_source_info_t *info,
                       void *context)
{
    (void)context;
    if (index >= sizeof(sources) / sizeof(sources[0])) {
        return false;
    }
    *info = sources[index];
    return true;
}

static void collect_candidate(const char *candidate, void *context)
{
    candidate_list_t *list = (candidate_list_t *)context;
    assert(list->count < CANDIDATE_MAX);
    assert(strlen(candidate) < CANDIDATE_LEN);
    strcpy(list->values[list->count++], candidate);
}

static candidate_list_t complete(const char *prefix,
                                 const char *selected_source)
{
    candidate_list_t list = {0};
    solar_os_shell_gesture_completion_emit(
        prefix,
        selected_source,
        sizeof(sources) / sizeof(sources[0]),
        get_source,
        NULL,
        collect_candidate,
        &list);
    return list;
}

static bool contains(const candidate_list_t *list, const char *candidate)
{
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->values[i], candidate) == 0) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    candidate_list_t list = complete("", NULL);
    assert(list.count == 5U);
    assert(contains(&list, "source="));
    assert(contains(&list, "gesture="));
    assert(contains(&list, "direction="));
    assert(contains(&list, "cooldown="));
    assert(contains(&list, "--"));

    list = complete("ge", NULL);
    assert(list.count == 1U);
    assert(contains(&list, "gesture="));

    list = complete("source=", NULL);
    assert(list.count == 3U);
    assert(contains(&list, "source=*"));
    assert(contains(&list, "source=skywriter0"));
    assert(contains(&list, "source=imu0"));
    assert(!contains(&list, "source=keyboard0"));

    list = complete("source=i", NULL);
    assert(list.count == 1U);
    assert(contains(&list, "source=imu0"));

    list = complete("gesture=", "skywriter0");
    assert(list.count == 2U);
    assert(contains(&list, "gesture=flick"));
    assert(contains(&list, "gesture=tap"));
    assert(!contains(&list, "gesture=wave"));

    list = complete("gesture=", "*");
    assert(list.count == 3U);
    assert(contains(&list, "gesture=flick"));
    assert(contains(&list, "gesture=tap"));
    assert(contains(&list, "gesture=wave"));

    list = complete("gesture=", "missing");
    assert(list.count == 0U);

    list = complete("direction=ea", NULL);
    assert(list.count == 1U);
    assert(contains(&list, "direction=east"));

    puts("gesture completion tests: ok");
    return 0;
}
