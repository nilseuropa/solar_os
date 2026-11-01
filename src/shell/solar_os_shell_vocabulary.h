#pragma once
#include "solar_os_config.h"

#define SOLAR_OS_SHELL_WORDS(name, ...) \
    enum { solar_os_shell_##name##_word_count = \
        sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *) }; \
    extern const char *const solar_os_shell_##name##_words[solar_os_shell_##name##_word_count];
#include "solar_os_shell_vocabulary.inc"
#undef SOLAR_OS_SHELL_WORDS
