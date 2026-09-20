#pragma once

#include <stdbool.h>

typedef enum {
    SOLAR_OS_SHELL_STARTUP_FLASH = 0,
    SOLAR_OS_SHELL_STARTUP_SD = 1,
    SOLAR_OS_SHELL_STARTUP_AUTO = 2,
} solar_os_shell_startup_source_t;

#define SOLAR_OS_SHELL_STARTUP_DEFAULT SOLAR_OS_SHELL_STARTUP_AUTO

const char *solar_os_shell_startup_source_name(solar_os_shell_startup_source_t source);
bool solar_os_shell_parse_startup_source(const char *name,
                                         solar_os_shell_startup_source_t *source);
solar_os_shell_startup_source_t solar_os_shell_resolve_startup_source(
    solar_os_shell_startup_source_t source,
    bool board_has_sd,
    bool sd_mounted);
