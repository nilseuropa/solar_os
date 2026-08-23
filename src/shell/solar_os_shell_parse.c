#include "solar_os_shell_parse.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool shell_operator_token(const char *value, char operator_text[3])
{
    static const char * const operators[] = {"|", "||", "<", ">", ">>", "&&", ";"};
    for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++) {
        if (strcmp(value, operators[i]) == 0) {
            operator_text[0] = operators[i][0];
            operator_text[1] = operators[i][1];
            operator_text[2] = '\0';
            return true;
        }
    }
    return false;
}

solar_os_shell_parse_result_t solar_os_shell_tokenize(char *line,
                                                       char **argv,
                                                       int argv_max)
{
    solar_os_shell_parse_result_t result = {0};
    if (line == NULL || argv == NULL || argv_max <= 0) {
        return result;
    }

    char *read = line;
    char *write = line;
    while (*read != '\0') {
        while (isspace((unsigned char)*read)) {
            read++;
        }
        if (*read == '\0') {
            break;
        }
        if (result.argc >= argv_max) {
            result.error = SOLAR_OS_SHELL_PARSE_TOO_MANY_ARGUMENTS;
            result.column = (size_t)(read - line);
            return result;
        }

        const size_t token_column = (size_t)(read - line);
        argv[result.argc++] = write;
        char quote = '\0';
        bool literalized = false;
        while (*read != '\0') {
            const char ch = *read;
            if (quote == '\0' && isspace((unsigned char)ch)) {
                break;
            }
            if ((ch == '"' || ch == '\'') && (quote == '\0' || quote == ch)) {
                literalized = true;
                quote = quote == '\0' ? ch : '\0';
                read++;
                continue;
            }
            if (ch == '\\' && quote != '\'') {
                literalized = true;
                if (read[1] == '\0') {
                    result.error = SOLAR_OS_SHELL_PARSE_DANGLING_ESCAPE;
                    result.column = (size_t)(read - line);
                    return result;
                }
                read++;
                *write++ = *read++;
                continue;
            }
            *write++ = *read++;
        }
        if (quote != '\0') {
            result.error = quote == '\'' ?
                SOLAR_OS_SHELL_PARSE_UNTERMINATED_SINGLE_QUOTE :
                SOLAR_OS_SHELL_PARSE_UNTERMINATED_DOUBLE_QUOTE;
            result.column = (size_t)(read - line);
            return result;
        }
        if (isspace((unsigned char)*read)) {
            read++;
        }
        *write++ = '\0';

        if (!literalized && shell_operator_token(argv[result.argc - 1], result.operator_text)) {
            result.error = SOLAR_OS_SHELL_PARSE_UNSUPPORTED_OPERATOR;
            result.column = token_column;
            return result;
        }
    }
    return result;
}

const char *solar_os_shell_parse_error_text(solar_os_shell_parse_error_t error)
{
    switch (error) {
    case SOLAR_OS_SHELL_PARSE_TOO_MANY_ARGUMENTS:
        return "too many arguments";
    case SOLAR_OS_SHELL_PARSE_UNTERMINATED_SINGLE_QUOTE:
        return "unterminated single quote";
    case SOLAR_OS_SHELL_PARSE_UNTERMINATED_DOUBLE_QUOTE:
        return "unterminated double quote";
    case SOLAR_OS_SHELL_PARSE_DANGLING_ESCAPE:
        return "trailing backslash has nothing to escape";
    case SOLAR_OS_SHELL_PARSE_UNSUPPORTED_OPERATOR:
        return "shell operator is not supported";
    case SOLAR_OS_SHELL_PARSE_OK:
    default:
        return "invalid command line";
    }
}

int solar_os_shell_edit_distance(const char *left, const char *right, int limit)
{
    if (left == NULL || right == NULL || limit < 0) {
        return limit + 1;
    }
    const size_t left_len = strlen(left);
    const size_t right_len = strlen(right);
    if (left_len > 63 || right_len > 63 ||
        (left_len > right_len ? left_len - right_len : right_len - left_len) > (size_t)limit) {
        return limit + 1;
    }

    uint8_t previous[64];
    uint8_t current[64];
    uint8_t before_previous[64];
    for (size_t j = 0; j <= right_len; j++) {
        previous[j] = (uint8_t)j;
        before_previous[j] = (uint8_t)j;
    }

    for (size_t i = 1; i <= left_len; i++) {
        current[0] = (uint8_t)i;
        uint8_t row_min = current[0];
        for (size_t j = 1; j <= right_len; j++) {
            const int left_ch = tolower((unsigned char)left[i - 1]);
            const int right_ch = tolower((unsigned char)right[j - 1]);
            const uint8_t substitution = (uint8_t)(previous[j - 1] + (left_ch == right_ch ? 0 : 1));
            const uint8_t insertion = (uint8_t)(current[j - 1] + 1);
            const uint8_t deletion = (uint8_t)(previous[j] + 1);
            uint8_t value = substitution < insertion ? substitution : insertion;
            if (deletion < value) {
                value = deletion;
            }
            if (i > 1 && j > 1 &&
                tolower((unsigned char)left[i - 1]) == tolower((unsigned char)right[j - 2]) &&
                tolower((unsigned char)left[i - 2]) == tolower((unsigned char)right[j - 1])) {
                const uint8_t transposition = (uint8_t)(before_previous[j - 2] + 1);
                if (transposition < value) {
                    value = transposition;
                }
            }
            current[j] = value;
            if (value < row_min) {
                row_min = value;
            }
        }
        if (row_min > (uint8_t)limit) {
            return limit + 1;
        }
        memcpy(before_previous, previous, right_len + 1);
        memcpy(previous, current, right_len + 1);
    }
    return previous[right_len] <= limit ? previous[right_len] : limit + 1;
}

const char *solar_os_shell_suggest(const char *input,
                                   const char * const *candidates,
                                   size_t candidate_count)
{
    if (input == NULL || input[0] == '\0' || candidates == NULL) {
        return NULL;
    }
    const int limit = strlen(input) >= 5 ? 2 : 1;
    const char *best = NULL;
    int best_distance = limit + 1;
    bool ambiguous = false;
    for (size_t i = 0; i < candidate_count; i++) {
        if (candidates[i] == NULL || candidates[i][0] == '\0') {
            continue;
        }
        const int distance = solar_os_shell_edit_distance(input, candidates[i], limit);
        if (distance < best_distance) {
            best = candidates[i];
            best_distance = distance;
            ambiguous = false;
        } else if (distance == best_distance && distance <= limit &&
                   best != NULL && strcmp(best, candidates[i]) != 0) {
            ambiguous = true;
        }
    }
    return best_distance <= limit && !ambiguous ? best : NULL;
}

bool solar_os_shell_parse_rgb888(const char *text, uint32_t *rgb888)
{
    if (text == NULL || rgb888 == NULL) {
        return false;
    }

    if (text[0] == '#') {
        text++;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (strlen(text) != 6) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < 6; i++) {
        const unsigned char ch = (unsigned char)text[i];
        uint8_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = (uint8_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (uint8_t)(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (uint8_t)(ch - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }

    *rgb888 = value;
    return true;
}
