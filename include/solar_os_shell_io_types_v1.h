#pragma once
#include "solar_os_application_types_v1.h"

typedef struct solar_os_shell_io solar_os_shell_io_t;

typedef enum {
    SOLAR_OS_SHELL_IO_KIND_NONE,
    SOLAR_OS_SHELL_IO_KIND_TERMINAL,
    SOLAR_OS_SHELL_IO_KIND_PORT,
} solar_os_shell_io_kind_t;

typedef enum {
    SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO,
    SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB,
    SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI,
    SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100,
} solar_os_shell_terminal_profile_t;

typedef enum {
    SOLAR_OS_SHELL_CHARSET_UTF8,
    SOLAR_OS_SHELL_CHARSET_ASCII,
} solar_os_shell_charset_t;
