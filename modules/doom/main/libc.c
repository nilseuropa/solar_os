// SPDX-License-Identifier: GPL-2.0-or-later
/* Private, locale-free C helpers. The foreground ABI exports only bulk
 * memory operations; never link the firmware's process-global libc state. */
#include "port.h"

int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *stpcpy(char *out, const char *in)
{ while ((*out = *in)) { out++; in++; } return out; }
char *strcpy(char *out, const char *in) { stpcpy(out, in); return out; }
char *strcat(char *out, const char *in) { stpcpy(out + strlen(out), in); return out; }
char *strncpy(char *out, const char *in, size_t n)
{
    char *result = out;
    while (n && *in) { *out++ = *in++; n--; }
    while (n--) *out++ = 0;
    return result;
}
void *memchr(const void *data, int ch, size_t n)
{
    const unsigned char *p = data;
    while (n--) { if (*p == (unsigned char)ch) return (void *)p; p++; }
    return NULL;
}
char *strchr(const char *text, int ch)
{
    do { if (*text == (char)ch) return (char *)text; } while (*text++);
    return NULL;
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        int x = tolower((unsigned char)*a++), y = tolower((unsigned char)*b++);
        if (x != y || !x) return x - y;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b) { return strncasecmp(a, b, SIZE_MAX); }
int atoi(const char *text)
{
    while (isspace(*text)) text++;
    int sign = 1; if (*text == '-' || *text == '+') { if (*text == '-') sign = -1; text++; }
    unsigned value = 0;
    while (isdigit(*text)) value = value * 10 + *text++ - '0';
    return sign * (int)value;
}
char *strtok(char *text, const char *delimiters)
{
    static char *next;
    if (!text) text = next;
    if (!text) return NULL;
    while (*text && strchr(delimiters, *text)) text++;
    if (!*text) { next = NULL; return NULL; }
    char *result = text;
    while (*text && !strchr(delimiters, *text)) text++;
    next = *text ? text + 1 : NULL;
    if (*text) *text = 0;
    return result;
}
/* libm reports domain errors through this module's own errno. */
int *__errno(void) { return &solar_os_doom_errno; }
