/* The native loader exports only the standard memory/string-length helpers. */
int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return (unsigned char)*left - (unsigned char)*right;
}
