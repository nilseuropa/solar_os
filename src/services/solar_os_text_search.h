#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SOLAR_OS_TEXT_SEARCH_QUERY_MAX 64U

typedef enum {
    SOLAR_OS_TEXT_SEARCH_FORWARD,
    SOLAR_OS_TEXT_SEARCH_BACKWARD,
} solar_os_text_search_direction_t;

typedef struct {
    size_t segment_index;
    size_t offset;
    size_t length;
    bool wrapped;
} solar_os_text_search_match_t;

typedef struct {
    bool input_active;
    bool match_valid;
    size_t input_len;
    size_t query_len;
    char input[SOLAR_OS_TEXT_SEARCH_QUERY_MAX];
    char query[SOLAR_OS_TEXT_SEARCH_QUERY_MAX];
    solar_os_text_search_match_t match;
} solar_os_text_search_state_t;

typedef bool (*solar_os_text_search_segment_fn)(void *user,
                                                size_t segment_index,
                                                const char **text,
                                                size_t *text_len);

void solar_os_text_search_reset(solar_os_text_search_state_t *state);
void solar_os_text_search_begin_input(solar_os_text_search_state_t *state);
void solar_os_text_search_cancel_input(solar_os_text_search_state_t *state);
bool solar_os_text_search_input_append(solar_os_text_search_state_t *state, char ch);
bool solar_os_text_search_input_backspace(solar_os_text_search_state_t *state);
bool solar_os_text_search_submit_input(solar_os_text_search_state_t *state);
bool solar_os_text_search_set_query(solar_os_text_search_state_t *state,
                                    const char *query);

bool solar_os_text_search_find(const char *text,
                               size_t text_len,
                               const char *query,
                               size_t anchor_offset,
                               bool skip_anchor,
                               solar_os_text_search_direction_t direction,
                               solar_os_text_search_match_t *match);
bool solar_os_text_search_find_segments(size_t segment_count,
                                        solar_os_text_search_segment_fn segment,
                                        void *user,
                                        const char *query,
                                        size_t anchor_segment,
                                        size_t anchor_offset,
                                        bool skip_anchor,
                                        solar_os_text_search_direction_t direction,
                                        solar_os_text_search_match_t *match);
