#pragma once

#include <ctype.h>
#include <string.h>

#include "solar_os_keys.h"
#include "solar_os_text_search.h"

static inline const char *solar_os_file_search_next_char(const char *text)
{
    const unsigned char first = (unsigned char)*text++;
    if (first >= 0xc2U && first <= 0xf4U) {
        while (((unsigned char)*text & 0xc0U) == 0x80U) {
            text++;
        }
    }
    return text;
}

/* Panel quick search has different rules from a document's substring search:
 * an implicit trailing '*' makes a pattern match the start of a filename. */
static inline bool solar_os_file_search_matches(const char *name, const char *pattern)
{
    const char *star = NULL;
    const char *retry = NULL;
    while (*pattern != '\0') {
        if (*pattern == '*') {
            star = ++pattern;
            retry = name;
        } else if (*pattern == '?' && *name != '\0') {
            pattern++;
            name = solar_os_file_search_next_char(name);
        } else if (*name != '\0' &&
                   tolower((unsigned char)*pattern) == tolower((unsigned char)*name)) {
            pattern++;
            name++;
        } else if (star != NULL && *retry != '\0') {
            retry = solar_os_file_search_next_char(retry);
            name = retry;
            pattern = star;
        } else {
            return false;
        }
    }
    return true;
}

static inline bool solar_os_file_search_find(solar_os_text_search_state_t *state,
                                             size_t count,
                                             solar_os_text_search_segment_fn segment,
                                             void *user,
                                             size_t *cursor,
                                             bool next)
{
    if (count == 0U) {
        return false;
    }
    size_t index = *cursor < count ? *cursor : 0U;
    if (next) {
        index = index + 1U < count ? index + 1U : 0U;
    }
    for (size_t remaining = count; remaining > 0U; remaining--) {
        const char *name = NULL;
        size_t length = 0U;
        if (segment(user, index, &name, &length) && name != NULL &&
            solar_os_file_search_matches(name, state->input)) {
            *cursor = index;
            return true;
        }
        index = index + 1U < count ? index + 1U : 0U;
    }
    return false;
}

static inline void solar_os_file_search_begin(solar_os_text_search_state_t *state,
                                              size_t count,
                                              solar_os_text_search_segment_fn segment,
                                              void *user,
                                              size_t *cursor)
{
    if (!state->input_active) {
        if (count > 0U) {
            state->input_active = true;
            state->input_len = 0U;
            state->input[0] = '\0';
        }
        return;
    }
    /* A second search key recalls the previous pattern in this pane. */
    if (state->input_len == 0U) {
        memcpy(state->input, state->query, state->query_len + 1U);
        state->input_len = state->query_len;
    }
    (void)solar_os_file_search_find(state, count, segment, user, cursor, true);
}

static inline void solar_os_file_search_stop(solar_os_text_search_state_t *state)
{
    if (state->input_active && state->input_len > 0U) {
        memcpy(state->query, state->input, state->input_len + 1U);
        state->query_len = state->input_len;
    }
    state->input_active = false;
}

/* Return false for commands so the app can navigate, open, or operate on the
 * selected entry with the same key that ends quick search. */
static inline bool solar_os_file_search_key(solar_os_text_search_state_t *state,
                                            uint8_t ch,
                                            size_t count,
                                            solar_os_text_search_segment_fn segment,
                                            void *user,
                                            size_t *cursor)
{
    /* Modifier-only key events must not interrupt a repeated Ctrl/Alt+S. */
    if (ch == 0U || ch == SOLAR_OS_KEY_CTRL) {
        return true;
    }
    if (ch == SOLAR_OS_KEY_ESCAPE || ch == 0x07U) {
        solar_os_file_search_stop(state);
        return true;
    }
    if (ch == '\b' || ch == 0x7fU || ch == SOLAR_OS_KEY_DELETE) {
        (void)solar_os_text_search_input_backspace(state);
        (void)solar_os_file_search_find(state, count, segment, user, cursor, false);
        return true;
    }
    /* 0x80..0xaf are normalized navigation/control keys, not text. */
    if ((ch >= ' ' && ch < 0x7fU) || ch >= 0xb0U) {
        if (solar_os_text_search_input_append(state, (char)ch) &&
            !solar_os_file_search_find(state, count, segment, user, cursor, false)) {
            /* MC ignores a character that would leave no matching filename. */
            (void)solar_os_text_search_input_backspace(state);
        }
        return true;
    }
    solar_os_file_search_stop(state);
    return false;
}
