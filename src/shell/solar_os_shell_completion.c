#include "solar_os_shell_completion.h"

#include <ctype.h>
#include <string.h>

size_t solar_os_shell_completion_common_prefix(const char *first,
                                               const char *second)
{
    if (first == NULL || second == NULL) {
        return 0U;
    }

    size_t length = 0U;
    while (first[length] != '\0' && first[length] == second[length]) {
        length++;
    }
    return length;
}

bool solar_os_shell_completion_parse(const char *input,
                                     size_t input_len,
                                     char *tokens,
                                     size_t token_size,
                                     size_t token_capacity,
                                     size_t *starts,
                                     size_t *count,
                                     bool *trailing_space)
{
    if (input == NULL || tokens == NULL || token_size == 0U ||
        token_capacity == 0U || starts == NULL || count == NULL ||
        trailing_space == NULL) {
        return false;
    }

    size_t pos = 0U;
    *count = 0U;
    *trailing_space = false;

    while (pos < input_len) {
        const size_t separator_start = pos;
        while (pos < input_len && isspace((unsigned char)input[pos])) {
            pos++;
        }
        if (pos >= input_len) {
            *trailing_space = pos > separator_start;
            break;
        }
        if (*count >= token_capacity) {
            return false;
        }

        starts[*count] = pos;
        char *token = tokens + (*count * token_size);
        size_t token_len = 0U;
        char quote = '\0';
        while (pos < input_len) {
            char ch = input[pos];
            if (quote == '\0' && isspace((unsigned char)ch)) {
                break;
            }
            if ((ch == '"' || ch == '\'') && (quote == '\0' || quote == ch)) {
                quote = quote == '\0' ? ch : '\0';
                pos++;
                continue;
            }
            if (ch == '\\' && quote != '\'') {
                pos++;
                if (pos < input_len) {
                    ch = input[pos++];
                } else {
                    ch = '\\';
                }
            } else {
                pos++;
            }
            if (token_len + 1U >= token_size) {
                return false;
            }
            token[token_len++] = ch;
        }
        token[token_len] = '\0';
        (*count)++;
    }

    return true;
}

bool solar_os_shell_completion_encode_token(const char *token,
                                            char *encoded,
                                            size_t encoded_len)
{
    if (token == NULL || encoded == NULL || encoded_len == 0U) {
        return false;
    }

    bool needs_quotes = token[0] == '\0';
    for (const unsigned char *cursor = (const unsigned char *)token;
         *cursor != '\0'; cursor++) {
        if (iscntrl(*cursor)) {
            return false;
        }
        if (isspace(*cursor) || *cursor == '\'' || *cursor == '"' ||
            *cursor == '\\' || *cursor == '|' || *cursor == '<' ||
            *cursor == '>' || *cursor == '&' || *cursor == ';') {
            needs_quotes = true;
        }
    }

    if (!needs_quotes) {
        const size_t length = strlen(token);
        if (length >= encoded_len) {
            return false;
        }
        memcpy(encoded, token, length + 1U);
        return true;
    }

    size_t used = 0U;
    if (used + 1U >= encoded_len) {
        return false;
    }
    encoded[used++] = '"';
    for (const char *cursor = token; *cursor != '\0'; cursor++) {
        if (*cursor == '"' || *cursor == '\\') {
            if (used + 1U >= encoded_len) {
                return false;
            }
            encoded[used++] = '\\';
        }
        if (used + 1U >= encoded_len) {
            return false;
        }
        encoded[used++] = *cursor;
    }
    if (used + 2U > encoded_len) {
        return false;
    }
    encoded[used++] = '"';
    encoded[used] = '\0';
    return true;
}
