#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_completion.h"
#include "solar_os_shell_parse.h"

static void assert_completion_parse(const char *input,
                                    size_t expected_count,
                                    bool expected_trailing_space,
                                    const char *expected_last,
                                    size_t expected_last_start)
{
    char tokens[8][64] = {{0}};
    size_t starts[8] = {0};
    size_t count = 0;
    bool trailing_space = false;

    assert(solar_os_shell_completion_parse(input,
                                           strlen(input),
                                           &tokens[0][0],
                                           sizeof(tokens[0]),
                                           8,
                                           starts,
                                           &count,
                                           &trailing_space));
    assert(count == expected_count);
    assert(trailing_space == expected_trailing_space);
    if (expected_count > 0U) {
        assert(strcmp(tokens[expected_count - 1U], expected_last) == 0);
        assert(starts[expected_count - 1U] == expected_last_start);
    }
}

static void assert_token_round_trip(const char *token, const char *expected_encoded)
{
    char encoded[128];
    assert(solar_os_shell_completion_encode_token(token, encoded, sizeof(encoded)));
    assert(strcmp(encoded, expected_encoded) == 0);

    char line[160];
    snprintf(line, sizeof(line), "cat %s", encoded);
    char *argv[4];
    const solar_os_shell_parse_result_t result = solar_os_shell_tokenize(line, argv, 4);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK);
    assert(result.argc == 2);
    assert(strcmp(argv[1], token) == 0);
}

int main(void)
{
    assert(solar_os_shell_completion_common_prefix("", "radio") == 0U);
    assert(solar_os_shell_completion_common_prefix("radio", "radio") == 5U);
    assert(solar_os_shell_completion_common_prefix("radio", "ramfs") == 2U);
    assert(solar_os_shell_completion_common_prefix("mount", "unmount") == 0U);
    char aggregate[] = "radio";
    aggregate[solar_os_shell_completion_common_prefix(aggregate, "radar")] = '\0';
    aggregate[solar_os_shell_completion_common_prefix(aggregate, "radius")] = '\0';
    assert(aggregate[0] == 'r' && aggregate[1] == 'a' &&
           aggregate[2] == 'd' && aggregate[3] == '\0');
    assert(solar_os_shell_completion_common_prefix(NULL, "value") == 0U);
    assert(solar_os_shell_completion_common_prefix("value", NULL) == 0U);

    assert_completion_parse("cat My\\ F", 2U, false, "My F", 4U);
    assert_completion_parse("cat \"My F", 2U, false, "My F", 4U);
    assert_completion_parse("cat \"My \"", 2U, false, "My ", 4U);
    assert_completion_parse("cat ", 1U, true, "cat", 0U);
    assert_completion_parse("cat \"My Folder/\"f", 2U, false, "My Folder/f", 4U);

    assert_token_round_trip("readme.txt", "readme.txt");
    assert_token_round_trip("My File.txt", "\"My File.txt\"");
    assert_token_round_trip("dir/My Folder/", "\"dir/My Folder/\"");
    assert_token_round_trip("a'b", "\"a'b\"");
    assert_token_round_trip("a\"b\\c", "\"a\\\"b\\\\c\"");
    assert_token_round_trip("|", "\"|\"");
    puts("shell completion tests: ok");
    return 0;
}
