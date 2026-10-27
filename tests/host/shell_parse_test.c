#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_parse.h"

static solar_os_shell_parse_result_t parse(char *line, char **argv, int max)
{
    return solar_os_shell_tokenize(line, argv, max);
}

int main(void)
{
    char *argv[8];

    char quoted[] = "curl \"https://example.test/a b\" 'literal value'";
    solar_os_shell_parse_result_t result = parse(quoted, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK);
    assert(result.argc == 3);
    assert(strcmp(argv[1], "https://example.test/a b") == 0);
    assert(strcmp(argv[2], "literal value") == 0);

    char escaped[] = "echo one\\ two";
    result = parse(escaped, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK);
    assert(result.argc == 2);
    assert(strcmp(argv[1], "one two") == 0);

    char unterminated[] = "echo \"unfinished";
    result = parse(unterminated, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_UNTERMINATED_DOUBLE_QUOTE);

    char dangling[] = "echo trailing\\";
    result = parse(dangling, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_DANGLING_ESCAPE);

    char operator_line[] = "status && reboot";
    result = parse(operator_line, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_UNSUPPORTED_OPERATOR);
    assert(strcmp(result.operator_text, "&&") == 0);

    char quoted_operator[] = "echo \"|\"";
    result = parse(quoted_operator, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK);
    assert(strcmp(argv[1], "|") == 0);

    char url_query[] = "curl https://example.test/?a=1&b=2";
    result = parse(url_query, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK);

    char too_many[] = "one two three";
    result = parse(too_many, argv, 2);
    assert(result.error == SOLAR_OS_SHELL_PARSE_TOO_MANY_ARGUMENTS);

    static const char * const commands[] = {"status", "stream", "storage"};
    static const char * const ambiguous[] = {"cat", "car"};
    assert(strcmp(solar_os_shell_suggest("statsu", commands, 3), "status") == 0);
    assert(strcmp(solar_os_shell_suggest("STAtus", commands, 3), "status") == 0);
    assert(solar_os_shell_suggest("can", ambiguous, 2) == NULL);
    assert(solar_os_shell_suggest("completely-different", commands, 3) == NULL);

    uint32_t color = 0;
    assert(solar_os_shell_parse_rgb888("#12AbEF", &color));
    assert(color == 0x12abef);
    assert(solar_os_shell_parse_rgb888("12abef", &color));
    assert(color == 0x12abef);
    assert(solar_os_shell_parse_rgb888("0x12ABEF", &color));
    assert(color == 0x12abef);
    assert(!solar_os_shell_parse_rgb888("#fff", &color));
    assert(!solar_os_shell_parse_rgb888("#12abeg", &color));
    assert(!solar_os_shell_parse_rgb888("0x1234567", &color));
    assert(!solar_os_shell_parse_rgb888(NULL, &color));
    assert(!solar_os_shell_parse_rgb888("#123456", NULL));

    char secret_setup[] = "  'messages'\tconfigure community sasl account password";
    result = parse(secret_setup, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK);
    assert(solar_os_shell_command_has_secrets(result.argc, (const char *const *)argv));
    char chat_secret[] = "/SaSl\tSolarIRC admin \"a password\\\\with'and\\\"quotes\"";
    result = parse(chat_secret, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK && result.argc == 4);
    assert(!strcmp(argv[3], "a password\\with'and\"quotes"));
    assert(solar_os_shell_command_has_secrets(result.argc, (const char *const *)argv));
    char chat_setup[] = "/'configure'\tSolarIRC sasl admin 'literal \\ value'";
    result = parse(chat_setup, argv, 8);
    assert(result.error == SOLAR_OS_SHELL_PARSE_OK && result.argc == 5);
    assert(!strcmp(argv[4], "literal \\ value"));
    assert(solar_os_shell_command_has_secrets(result.argc, (const char *const *)argv));
    char chat_status[] = "/status";
    result = parse(chat_status, argv, 8);
    assert(!solar_os_shell_command_has_secrets(result.argc, (const char *const *)argv));
    char secret_ble[] = "job start meshcore-ble 01:02:03:04:05:06 public 123456";
    result = parse(secret_ble, argv, 8);
    assert(solar_os_shell_command_has_secrets(result.argc, (const char *const *)argv));
    char ordinary[] = "messages instances";
    result = parse(ordinary, argv, 8);
    assert(!solar_os_shell_command_has_secrets(result.argc, (const char *const *)argv));
    assert(!solar_os_shell_command_has_secrets(0, NULL));

    puts("shell_parse_test: ok");
    return 0;
}
