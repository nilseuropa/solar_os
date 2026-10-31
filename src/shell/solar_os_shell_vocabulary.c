#include "solar_os_shell_vocabulary.h"

#define SOLAR_OS_SHELL_WORDS(name, ...) \
    const char *const solar_os_shell_##name##_words[] = {__VA_ARGS__};
#include "solar_os_shell_vocabulary.inc"
#undef SOLAR_OS_SHELL_WORDS
