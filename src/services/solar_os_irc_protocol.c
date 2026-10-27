#include "solar_os_irc_protocol.h"
#include <string.h>

bool solar_os_irc_atom_valid(const char *text, size_t capacity)
{
    if (!text || !text[0] || strnlen(text, capacity) >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p <= 32 || *p == 127 || *p == ':' || *p == ',') return false;
    return true;
}

bool solar_os_irc_target_valid(const char *text, size_t capacity)
{
    return solar_os_irc_atom_valid(text, capacity) && text[0] != '@' &&
        text[0] != '+' && text[0] != '%' && text[0] != '~';
}

bool solar_os_irc_nick_valid(const char *text, size_t capacity)
{
    /* RFC 2812 section 2.3.1 character grammar; accept longer nicknames up to
     * the configured buffer limit as recommended by section 1.2.1. */
    if (!text || !text[0] || strnlen(text, capacity) >= capacity) return false;
    for (size_t i = 0; text[i]; i++) {
        unsigned char c = text[i];
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool special = (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7d);
        if (!letter && !special && !(i && ((c >= '0' && c <= '9') || c == '-'))) return false;
    }
    return true;
}

bool solar_os_irc_parse(char *line, solar_os_irc_message_t *message)
{
    if (!line || !message) return false;
    memset(message, 0, sizeof(*message));
    char *p = line;
    /* Ignore unsolicited tags. No history/echo capabilities are requested. */
    if (*p == '@') {
        p = strchr(p, ' '); if (!p) return false;
        while (*p == ' ') p++;
    }
    if (*p == ':') {
        message->prefix = ++p;
        p = strchr(p, ' '); if (!p) return false;
        *p++ = 0; while (*p == ' ') p++;
    }
    if (!*p) return false;
    message->command = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (message->count == SOLAR_OS_IRC_PARAM_MAX) return false;
        if (*p == ':') { message->params[message->count++] = p + 1; break; }
        message->params[message->count++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return true;
}

void solar_os_irc_casefold(const char *text, char *out, size_t capacity,
                           solar_os_irc_casemapping_t mapping)
{
    if (!out || !capacity) return;
    size_t i = 0;
    for (; text && text[i] && i + 1 < capacity; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (mapping != SOLAR_OS_IRC_CASE_ASCII) {
            if (c == '[') c = '{'; else if (c == ']') c = '}';
            else if (c == '\\') c = '|';
            else if (c == '^' && mapping == SOLAR_OS_IRC_CASE_RFC1459) c = '~';
        }
        out[i] = (char)c;
    }
    out[i] = 0;
}

void solar_os_irc_clean_text(char *text)
{
    /* Drop IRC formatting and terminal control bytes before publication. */
    char *out = text;
    for (unsigned char *p = (unsigned char *)text; *p; p++) {
        if (*p == 3) {
            for (unsigned n = 0; n < 2 && p[1] >= '0' && p[1] <= '9'; n++) p++;
            if (p[1] == ',' && p[2] >= '0' && p[2] <= '9') {
                p++;
                for (unsigned n = 0; n < 2 && p[1] >= '0' && p[1] <= '9'; n++) p++;
            }
        } else if (*p >= 32 && *p != 127) *out++ = (char)*p;
        else if (*p == '\t') *out++ = ' ';
    }
    *out = 0;
}
