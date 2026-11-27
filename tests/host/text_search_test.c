#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "solar_os_native_text_search_abi.h"
#include "solar_os_native_ui.h"

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
    const solar_os_native_text_search_api_v1_t *api = solar_os_native_ui_get_service(
        SOLAR_OS_NATIVE_TEXT_SEARCH_SERVICE, SOLAR_OS_NATIVE_TEXT_SEARCH_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_ui_get_service(SOLAR_OS_NATIVE_TEXT_SEARCH_SERVICE,
                                           SOLAR_OS_NATIVE_TEXT_SEARCH_ABI, sizeof(*api) + 1));
    solar_os_text_search_match_t match;
    assert(api->find("One two ONE", 11U, "one", 0U, false,
                     SOLAR_OS_TEXT_SEARCH_FORWARD, &match));
    assert(match.offset == 0U && !match.wrapped);
    assert(api->find("One two ONE", 11U, "one", 0U, true,
                     SOLAR_OS_TEXT_SEARCH_FORWARD, &match));
    assert(match.offset == 8U && !match.wrapped);
    assert(api->find("One two ONE", 11U, "one", 8U, true,
                     SOLAR_OS_TEXT_SEARCH_FORWARD, &match));
    assert(match.offset == 0U && match.wrapped);
    assert(api->find_segments(3U, segment, NULL, "alpha", 2U, 0U, true,
                              SOLAR_OS_TEXT_SEARCH_BACKWARD, &match));
    assert(match.segment_index == 0U && match.offset == 0U);

    solar_os_text_search_state_t state;
    api->reset(&state);
    api->begin_input(&state);
    assert(api->input_append(&state, 'x'));
    assert(api->input_append(&state, 'y'));
    assert(api->input_backspace(&state));
    assert(api->submit_input(&state));
    assert(strcmp(state.query, "x") == 0);
    api->begin_input(&state);
    assert(strcmp(state.input, "x") == 0);
    api->cancel_input(&state);
    assert(api->set_query(&state, "shared search"));
    assert(api->set_query(&state, state.query + 7) && !strcmp(state.query, "search"));
    api->begin_input(&state);
    state.input_len = SIZE_MAX;
    assert(!api->input_append(&state, 'y'));
    assert(!api->input_backspace(&state));
    assert(!api->submit_input(&state));
    api->cancel_input(&state);
    assert(!strcmp(state.input, state.query));
    assert(!api->find("One", 3U, "one", 0U, false, 3, &match));
    return 0;
}
