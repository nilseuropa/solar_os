#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SOLAR_OS_IRC_LINE_MAX 512U
#define SOLAR_OS_IRC_PARAM_MAX 15U
typedef enum {
    SOLAR_OS_IRC_CASE_ASCII,
    SOLAR_OS_IRC_CASE_STRICT_RFC1459,
    SOLAR_OS_IRC_CASE_RFC1459,
} solar_os_irc_casemapping_t;
typedef struct {
    char *prefix;
    char *command;
    char *params[SOLAR_OS_IRC_PARAM_MAX];
    size_t count;
} solar_os_irc_message_t;

/* In-place parser; pointers are valid until the input buffer is reused. */
bool solar_os_irc_parse(char *line, solar_os_irc_message_t *message);
bool solar_os_irc_atom_valid(const char *text, size_t capacity);
bool solar_os_irc_nick_valid(const char *text, size_t capacity);
bool solar_os_irc_target_valid(const char *text, size_t capacity);
void solar_os_irc_casefold(const char *text, char *out, size_t capacity,
                           solar_os_irc_casemapping_t mapping);
void solar_os_irc_clean_text(char *text);
