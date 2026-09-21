#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "solar_os_graffiti.h"

static solar_os_graffiti_result_t recognize(
    void *context,
    solar_os_graffiti_zone_t zone,
    const solar_os_unistroke_point_t *points,
    size_t count)
{
    solar_os_graffiti_result_t result;
    assert(solar_os_graffiti_recognize(context, zone, points, count, &result));
    assert(result.matched);
    return result;
}

int main(void)
{
    assert(solar_os_graffiti_zone_for_start(199, 300) ==
           SOLAR_OS_GRAFFITI_LETTERS);
    assert(solar_os_graffiti_zone_for_start(200, 300) ==
           SOLAR_OS_GRAFFITI_NUMBERS);
    assert(solar_os_graffiti_zone_for_start(299, 300) ==
           SOLAR_OS_GRAFFITI_NUMBERS);

    void *context = calloc(1, solar_os_graffiti_context_size());
    assert(context != NULL);
    assert(solar_os_graffiti_init(context, solar_os_graffiti_context_size()));
    assert(solar_os_graffiti_template_count() >= 50U);

    const solar_os_unistroke_point_t a[] = {{5, 95}, {50, 5}, {95, 95}};
    solar_os_graffiti_result_t result = recognize(
        context, SOLAR_OS_GRAFFITI_LETTERS, a, 3);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_CHARACTER);
    assert(result.character == 'a');

    const solar_os_unistroke_point_t one[] = {{50, 5}, {50, 95}};
    result = recognize(context, SOLAR_OS_GRAFFITI_NUMBERS, one, 2);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_CHARACTER);
    assert(result.character == '1');
    result = recognize(context, SOLAR_OS_GRAFFITI_LETTERS, one, 2);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_CHARACTER);
    assert(result.character == 'i');

    const solar_os_unistroke_point_t shift[] = {{50, 95}, {50, 5}};
    result = recognize(context, SOLAR_OS_GRAFFITI_LETTERS, shift, 2);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_SHIFT);

    solar_os_graffiti_case_state_t state = {0};
    char character = '\0';
    assert(!solar_os_graffiti_apply(&state, &result, &character));
    assert(state.shift_pending && !state.caps_lock);
    result = (solar_os_graffiti_result_t) {
        .matched = true,
        .action = SOLAR_OS_GRAFFITI_ACTION_CHARACTER,
        .character = 'a',
    };
    assert(solar_os_graffiti_apply(&state, &result, &character));
    assert(character == 'A' && !state.shift_pending);

    result.action = SOLAR_OS_GRAFFITI_ACTION_SHIFT;
    assert(!solar_os_graffiti_apply(&state, &result, &character));
    assert(!solar_os_graffiti_apply(&state, &result, &character));
    assert(state.caps_lock);
    result.action = SOLAR_OS_GRAFFITI_ACTION_CHARACTER;
    result.character = 'b';
    assert(solar_os_graffiti_apply(&state, &result, &character));
    assert(character == 'B' && state.caps_lock);
    result.action = SOLAR_OS_GRAFFITI_ACTION_SHIFT;
    assert(!solar_os_graffiti_apply(&state, &result, &character));
    assert(!state.caps_lock);

    const solar_os_unistroke_point_t space[] = {{5, 50}, {95, 50}};
    result = recognize(context, SOLAR_OS_GRAFFITI_LETTERS, space, 2);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_SPACE);
    const solar_os_unistroke_point_t backspace[] = {{95, 50}, {5, 50}};
    result = recognize(context, SOLAR_OS_GRAFFITI_LETTERS, backspace, 2);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_BACKSPACE);
    const solar_os_unistroke_point_t enter[] = {{92, 8}, {8, 92}};
    result = recognize(context, SOLAR_OS_GRAFFITI_LETTERS, enter, 2);
    assert(result.action == SOLAR_OS_GRAFFITI_ACTION_ENTER);

    free(context);
    puts("graffiti_test: ok");
    return 0;
}
