#include <assert.h>
#include <stdio.h>

#include "solar_os_file_search.h"

static const char *names[] = {"..", "alpha", "ALPINE", "myalpha", "beta", "a.txt"};
#define COUNT (sizeof(names) / sizeof(names[0]))

static bool segment(void *user, size_t index, const char **text, size_t *length)
{
    (void)user;
    *text = names[index];
    *length = strlen(*text);
    return true;
}

static void type(solar_os_text_search_state_t *search, size_t *cursor, const char *text)
{
    while (*text != '\0') {
        assert(solar_os_file_search_key(search, (uint8_t)*text++, COUNT,
                                        segment, NULL, cursor));
    }
}

int main(void)
{
    assert(solar_os_file_search_matches("Alpha.txt", "alp"));
    assert(!solar_os_file_search_matches("myalpha", "alp"));
    assert(solar_os_file_search_matches("Alpha.txt", "a?ph*txt"));
    assert(solar_os_file_search_matches("Alpha.txt", "*txt"));
    assert(!solar_os_file_search_matches("Alpha.txt", "*txt?"));
    assert(!solar_os_file_search_matches("a", "a?"));
    assert(solar_os_file_search_matches("résumé.txt", "r?sum?."));
    assert(solar_os_file_search_matches("résumé.txt", "*?sum?"));
    assert(!solar_os_file_search_matches("é", "??"));
    assert(solar_os_file_search_matches("[notes]", "[n"));
    assert(solar_os_file_search_matches("{a,b}|c", "{a,b}|"));
    assert(solar_os_file_search_matches("", "**"));

    solar_os_text_search_state_t search[2] = {0};
    size_t cursor = 0;
    solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
    assert(search[0].input_active && search[0].input_len == 0);
    assert(cursor == 0);
    type(&search[0], &cursor, "alp");
    assert(cursor == 1 && !strcmp(search[0].input, "alp"));
    solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
    assert(cursor == 2 && !strcmp(search[0].input, "alp"));
    solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
    assert(cursor == 1); /* Wrap; never select the substring in myalpha. */
    type(&search[0], &cursor, "z");
    assert(cursor == 1 && !strcmp(search[0].input, "alp"));

    assert(solar_os_file_search_key(&search[0], '\b', COUNT, segment, NULL, &cursor));
    assert(!strcmp(search[0].input, "al") && cursor == 1);
    assert(solar_os_file_search_key(&search[0], SOLAR_OS_KEY_DELETE,
                                   COUNT, segment, NULL, &cursor));
    assert(!strcmp(search[0].input, "a"));
    type(&search[0], &cursor, "?*");
    assert(!strcmp(search[0].input, "a?*"));
    assert(solar_os_file_search_key(&search[0], 0U, COUNT, segment, NULL, &cursor));
    assert(solar_os_file_search_key(&search[0], SOLAR_OS_KEY_CTRL,
                                   COUNT, segment, NULL, &cursor));
    assert(search[0].input_active);
    assert(solar_os_file_search_key(&search[0], SOLAR_OS_KEY_ESCAPE,
                                   COUNT, segment, NULL, &cursor));
    assert(!search[0].input_active && cursor == 1);
    assert(!strcmp(search[0].query, "a?*"));

    solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
    assert(search[0].input_len == 0);
    solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
    assert(cursor == 2 && !strcmp(search[0].input, "a?*"));
    assert(!solar_os_file_search_key(&search[0], SOLAR_OS_KEY_ENTER,
                                    COUNT, segment, NULL, &cursor));
    assert(!search[0].input_active && cursor == 2); /* Enter reaches open. */

    const uint8_t commands[] = {SOLAR_OS_KEY_UP, SOLAR_OS_KEY_DOWN,
        SOLAR_OS_KEY_PAGE_DOWN, SOLAR_OS_KEY_HOME, '\t', SOLAR_OS_KEY_F3,
        SOLAR_OS_KEY_F8, SOLAR_OS_KEY_F10, SOLAR_OS_KEY_APP_EXIT};
    for (size_t i = 0; i < sizeof(commands); i++) {
        solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
        assert(!solar_os_file_search_key(&search[0], commands[i],
                                         COUNT, segment, NULL, &cursor));
        assert(!search[0].input_active);
    }

    solar_os_file_search_begin(&search[1], COUNT, segment, NULL, &cursor);
    type(&search[1], &cursor, "beta");
    assert(cursor == 4);
    solar_os_file_search_stop(&search[1]);
    assert(!strcmp(search[1].query, "beta"));
    assert(!strcmp(search[0].query, "a?*")); /* Histories belong to each pane. */

    memset(&search[0], 0, sizeof(search[0]));
    cursor = 0;
    solar_os_file_search_begin(&search[0], 0, segment, NULL, &cursor);
    assert(!search[0].input_active);
    solar_os_file_search_begin(&search[0], COUNT, segment, NULL, &cursor);
    for (size_t i = 0; i < SOLAR_OS_TEXT_SEARCH_QUERY_MAX + 10U; i++) {
        type(&search[0], &cursor, "*");
    }
    assert(search[0].input_len == SOLAR_OS_TEXT_SEARCH_QUERY_MAX - 1U);
    assert(search[0].input[search[0].input_len] == '\0');
    puts("file quick search tests: ok");
    return 0;
}
