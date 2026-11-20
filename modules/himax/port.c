/* The native loader exports only the standard memory/string-length helpers. */
int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return (unsigned char)*left - (unsigned char)*right;
}
int strncmp(const char *left, const char *right, __SIZE_TYPE__ length)
{
    while (length && *left && *left == *right) { ++left; ++right; --length; }
    return length ? (unsigned char)*left - (unsigned char)*right : 0;
}
char *strchr(const char *text, int ch)
{
    do { if ((unsigned char)*text == (unsigned char)ch) return (char *)text; } while (*text++);
    return (void *)0;
}
char *strrchr(const char *text, int ch)
{
    const char *last = (void *)0;
    do { if ((unsigned char)*text == (unsigned char)ch) last = text; } while (*text++);
    return (char *)last;
}
