#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "solar_os_text_search.h"

static const char *const segments[] = {"Alpha", "middle BETA", "alpha end"};

static bool segment(void *user, size_t index, const char **text, size_t *len)
{
    (void)user;
    if (index >= 3U) {
        return false;
    }
    *text = segments[index];
    *len = strlen(*text);
    return true;
}

int main(void)
{
    solar_os_text_search_match_t match;
    assert(solar_os_text_search_find("One two ONE", 11U, "one", 0U, false,
                                     SOLAR_OS_TEXT_SEARCH_FORWARD, &match));
    assert(match.offset == 0U && !match.wrapped);
    assert(solar_os_text_search_find("One two ONE", 11U, "one", 0U, true,
                                     SOLAR_OS_TEXT_SEARCH_FORWARD, &match));
    assert(match.offset == 8U && !match.wrapped);
    assert(solar_os_text_search_find("One two ONE", 11U, "one", 8U, true,
                                     SOLAR_OS_TEXT_SEARCH_FORWARD, &match));
    assert(match.offset == 0U && match.wrapped);
    assert(solar_os_text_search_find_segments(3U, segment, NULL, "alpha", 2U, 0U,
                                              true, SOLAR_OS_TEXT_SEARCH_BACKWARD,
                                              &match));
    assert(match.segment_index == 0U && match.offset == 0U);

    solar_os_text_search_state_t state;
    solar_os_text_search_reset(&state);
    solar_os_text_search_begin_input(&state);
    assert(solar_os_text_search_input_append(&state, 'x'));
    assert(solar_os_text_search_input_append(&state, 'y'));
    assert(solar_os_text_search_input_backspace(&state));
    assert(solar_os_text_search_submit_input(&state));
    assert(strcmp(state.query, "x") == 0);
    solar_os_text_search_begin_input(&state);
    assert(strcmp(state.input, "x") == 0);
    solar_os_text_search_cancel_input(&state);
    return 0;
}
