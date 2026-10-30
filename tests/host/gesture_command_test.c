#include <assert.h>
#include <string.h>
#include "solar_os_shell_parse.h"
size_t strlcat(char *, const char *, size_t);
#include "../../src/shell/solar_os_shell_input.c"

static void roundtrip(char *line, int expected, const char *last)
{
    char *argv[12], command[256];
    solar_os_shell_parse_result_t parsed = solar_os_shell_tokenize(line, argv, 12);
    assert(parsed.error == SOLAR_OS_SHELL_PARSE_OK);
    assert(input_build_command(parsed.argc, argv, 1, command, sizeof(command)));
    parsed = solar_os_shell_tokenize(command, argv, 12);
    assert(parsed.error == SOLAR_OS_SHELL_PARSE_OK && parsed.argc == expected);
    assert(!strcmp(argv[parsed.argc - 1], last));
}

int main(void)
{
    char quoted[] = "-- \"audio tone 440 500\"";
    char separate[] = "-- audio tone 440 500";
    char argument[] = "-- echo \"two words\"";
    char nested[] = "-- 'echo \"two words\"'";
    char escaped[] = "-- echo 'a\"b\\c'";
    roundtrip(quoted, 4, "500");
    roundtrip(separate, 4, "500");
    roundtrip(argument, 2, "two words");
    roundtrip(nested, 2, "two words");
    roundtrip(escaped, 2, "a\"b\\c");
    char command[8];
    char *long_command[] = {"audio tone 440 500"};
    char *empty[] = {""};
    assert(!input_build_command(1, long_command, 0, command, sizeof(command)));
    assert(!input_build_command(1, empty, 0, command, sizeof(command)));
    assert(!input_build_command(0, empty, 0, command, sizeof(command)));
    return 0;
}
