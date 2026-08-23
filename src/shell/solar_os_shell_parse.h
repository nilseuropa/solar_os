#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_SHELL_PARSE_OK = 0,
    SOLAR_OS_SHELL_PARSE_TOO_MANY_ARGUMENTS,
    SOLAR_OS_SHELL_PARSE_UNTERMINATED_SINGLE_QUOTE,
    SOLAR_OS_SHELL_PARSE_UNTERMINATED_DOUBLE_QUOTE,
    SOLAR_OS_SHELL_PARSE_DANGLING_ESCAPE,
    SOLAR_OS_SHELL_PARSE_UNSUPPORTED_OPERATOR,
} solar_os_shell_parse_error_t;

typedef struct {
    int argc;
    solar_os_shell_parse_error_t error;
    size_t column;
    char operator_text[3];
} solar_os_shell_parse_result_t;

solar_os_shell_parse_result_t solar_os_shell_tokenize(char *line,
                                                       char **argv,
                                                       int argv_max);
const char *solar_os_shell_parse_error_text(solar_os_shell_parse_error_t error);
int solar_os_shell_edit_distance(const char *left, const char *right, int limit);
const char *solar_os_shell_suggest(const char *input,
                                   const char * const *candidates,
                                   size_t candidate_count);
bool solar_os_shell_parse_rgb888(const char *text, uint32_t *rgb888);
