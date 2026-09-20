#include "solar_os_shell_startup_policy.h"

#include <stddef.h>
#include <string.h>

const char *solar_os_shell_startup_source_name(solar_os_shell_startup_source_t source)
{
    switch (source) {
    case SOLAR_OS_SHELL_STARTUP_FLASH:
        return "flash";
    case SOLAR_OS_SHELL_STARTUP_SD:
        return "sd";
    case SOLAR_OS_SHELL_STARTUP_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

bool solar_os_shell_parse_startup_source(const char *name,
                                         solar_os_shell_startup_source_t *source)
{
    if (name == NULL || source == NULL) {
        return false;
    }
    if (strcmp(name, "auto") == 0) {
        *source = SOLAR_OS_SHELL_STARTUP_AUTO;
        return true;
    }
    if (strcmp(name, "flash") == 0) {
        *source = SOLAR_OS_SHELL_STARTUP_FLASH;
        return true;
    }
    if (strcmp(name, "sd") == 0) {
        *source = SOLAR_OS_SHELL_STARTUP_SD;
        return true;
    }
    return false;
}

solar_os_shell_startup_source_t solar_os_shell_resolve_startup_source(
    solar_os_shell_startup_source_t source,
    bool board_has_sd,
    bool sd_mounted)
{
    if (source == SOLAR_OS_SHELL_STARTUP_AUTO) {
        return board_has_sd && sd_mounted ?
            SOLAR_OS_SHELL_STARTUP_SD : SOLAR_OS_SHELL_STARTUP_FLASH;
    }
    return source;
}
