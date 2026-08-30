#include "solar_os_text_search.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static bool text_search_matches_at(const char *text,
                                   size_t text_len,
                                   size_t offset,
                                   const char *query,
                                   size_t query_len)
{
    if (text == NULL || query == NULL || query_len == 0U ||
        offset > text_len || query_len > text_len - offset) {
        return false;
    }

    for (size_t i = 0U; i < query_len; i++) {
        if (tolower((unsigned char)text[offset + i]) !=
            tolower((unsigned char)query[i])) {
            return false;
        }
    }
    return true;
}

void solar_os_text_search_reset(solar_os_text_search_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void solar_os_text_search_begin_input(solar_os_text_search_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memcpy(state->input, state->query, state->query_len + 1U);
    state->input_len = state->query_len;
    state->input_active = true;
}

void solar_os_text_search_cancel_input(solar_os_text_search_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memcpy(state->input, state->query, state->query_len + 1U);
    state->input_len = state->query_len;
    state->input_active = false;
}

bool solar_os_text_search_input_append(solar_os_text_search_state_t *state, char ch)
{
    if (state == NULL || !state->input_active ||
        state->input_len + 1U >= sizeof(state->input)) {
        return false;
    }
    state->input[state->input_len++] = ch;
    state->input[state->input_len] = '\0';
    return true;
}

bool solar_os_text_search_input_backspace(solar_os_text_search_state_t *state)
{
    if (state == NULL || !state->input_active || state->input_len == 0U) {
        return false;
    }
    state->input[--state->input_len] = '\0';
    return true;
}

bool solar_os_text_search_set_query(solar_os_text_search_state_t *state,
                                    const char *query)
{
    if (state == NULL || query == NULL) {
        return false;
    }
    const size_t query_len = strlen(query);
    if (query_len == 0U || query_len >= sizeof(state->query)) {
        state->query_len = 0U;
        state->query[0] = '\0';
        state->match_valid = false;
        return false;
    }
    memcpy(state->query, query, query_len + 1U);
    state->query_len = query_len;
    state->match_valid = false;
    return true;
}

bool solar_os_text_search_submit_input(solar_os_text_search_state_t *state)
{
    if (state == NULL || !state->input_active) {
        return false;
    }
    state->input_active = false;
    return solar_os_text_search_set_query(state, state->input);
}

static bool text_search_forward(size_t segment_count,
                                solar_os_text_search_segment_fn segment,
                                void *user,
                                const char *query,
                                size_t query_len,
                                size_t start_segment,
                                size_t start_offset,
                                solar_os_text_search_match_t *match)
{
    for (size_t segment_index = start_segment; segment_index < segment_count; segment_index++) {
        const char *text = NULL;
        size_t text_len = 0U;
        if (!segment(user, segment_index, &text, &text_len) || text == NULL ||
            query_len > text_len) {
            continue;
        }
        const size_t first = segment_index == start_segment ? start_offset : 0U;
        if (first > text_len - query_len) {
            continue;
        }
        for (size_t offset = first; offset <= text_len - query_len; offset++) {
            if (text_search_matches_at(text, text_len, offset, query, query_len)) {
                *match = (solar_os_text_search_match_t){
                    .segment_index = segment_index,
                    .offset = offset,
                    .length = query_len,
                };
                return true;
            }
        }
    }
    return false;
}

static bool text_search_backward(solar_os_text_search_segment_fn segment,
                                 void *user,
                                 const char *query,
                                 size_t query_len,
                                 size_t start_segment,
                                 size_t start_offset,
                                 solar_os_text_search_match_t *match)
{
    for (size_t segment_count = start_segment + 1U; segment_count > 0U; segment_count--) {
        const size_t segment_index = segment_count - 1U;
        const char *text = NULL;
        size_t text_len = 0U;
        if (!segment(user, segment_index, &text, &text_len) || text == NULL ||
            query_len > text_len) {
            continue;
        }
        size_t last = text_len - query_len;
        if (segment_index == start_segment && start_offset < last) {
            last = start_offset;
        }
        for (size_t offset_count = last + 1U; offset_count > 0U; offset_count--) {
            const size_t offset = offset_count - 1U;
            if (text_search_matches_at(text, text_len, offset, query, query_len)) {
                *match = (solar_os_text_search_match_t){
                    .segment_index = segment_index,
                    .offset = offset,
                    .length = query_len,
                };
                return true;
            }
        }
    }
    return false;
}

bool solar_os_text_search_find_segments(size_t segment_count,
                                        solar_os_text_search_segment_fn segment,
                                        void *user,
                                        const char *query,
                                        size_t anchor_segment,
                                        size_t anchor_offset,
                                        bool skip_anchor,
                                        solar_os_text_search_direction_t direction,
                                        solar_os_text_search_match_t *match)
{
    if (segment_count == 0U || segment == NULL || query == NULL || match == NULL) {
        return false;
    }
    const size_t query_len = strlen(query);
    if (query_len == 0U) {
        return false;
    }
    if (anchor_segment >= segment_count) {
        anchor_segment = direction == SOLAR_OS_TEXT_SEARCH_FORWARD ? 0U : segment_count - 1U;
        anchor_offset = direction == SOLAR_OS_TEXT_SEARCH_FORWARD ? 0U : SIZE_MAX;
        skip_anchor = false;
    }

    if (direction == SOLAR_OS_TEXT_SEARCH_FORWARD) {
        if (skip_anchor && anchor_offset < SIZE_MAX) {
            anchor_offset++;
        }
        if (text_search_forward(segment_count, segment, user, query, query_len,
                                anchor_segment, anchor_offset, match)) {
            return true;
        }
        if (text_search_forward(segment_count, segment, user, query, query_len,
                                0U, 0U, match)) {
            match->wrapped = true;
            return true;
        }
    } else {
        if (skip_anchor) {
            if (anchor_offset > 0U) {
                anchor_offset--;
            } else if (anchor_segment > 0U) {
                anchor_segment--;
                anchor_offset = SIZE_MAX;
            } else {
                anchor_segment = segment_count - 1U;
                anchor_offset = SIZE_MAX;
                if (text_search_backward(segment, user, query, query_len,
                                         anchor_segment, anchor_offset, match)) {
                    match->wrapped = true;
                    return true;
                }
                return false;
            }
        }
        if (text_search_backward(segment, user, query, query_len,
                                 anchor_segment, anchor_offset, match)) {
            return true;
        }
        if (text_search_backward(segment, user, query, query_len,
                                 segment_count - 1U, SIZE_MAX, match)) {
            match->wrapped = true;
            return true;
        }
    }
    return false;
}

typedef struct {
    const char *text;
    size_t text_len;
} text_search_single_segment_t;

static bool text_search_single_segment(void *user,
                                       size_t segment_index,
                                       const char **text,
                                       size_t *text_len)
{
    text_search_single_segment_t *single = (text_search_single_segment_t *)user;
    if (single == NULL || segment_index != 0U || text == NULL || text_len == NULL) {
        return false;
    }
    *text = single->text;
    *text_len = single->text_len;
    return true;
}

bool solar_os_text_search_find(const char *text,
                               size_t text_len,
                               const char *query,
                               size_t anchor_offset,
                               bool skip_anchor,
                               solar_os_text_search_direction_t direction,
                               solar_os_text_search_match_t *match)
{
    text_search_single_segment_t single = {
        .text = text,
        .text_len = text_len,
    };
    return solar_os_text_search_find_segments(1U,
                                              text_search_single_segment,
                                              &single,
                                              query,
                                              0U,
                                              anchor_offset,
                                              skip_anchor,
                                              direction,
                                              match);
}
