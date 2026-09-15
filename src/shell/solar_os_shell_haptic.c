#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_haptic.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static EXT_RAM_BSS_ATTR solar_os_haptic_info_t info;
    return solar_os_haptic_get(0U, &info) ? info.name : NULL;
}

void solar_os_shell_cmd_haptic(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        if (solar_os_haptic_count() == 0U) {
            solar_os_shell_io_writeln(term, "no haptic devices registered");
            return;
        }
        solar_os_haptic_info_t info;
        for (size_t i = 0; solar_os_haptic_get(i, &info); i++) {
            solar_os_shell_io_printf(term,
                                     "%s  %s  effects=1..%u\r\n",
                                     info.name,
                                     info.driver,
                                     info.effect_count);
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: haptic stop [name]");
            return;
        }
        const char *name = argc == 3 ? argv[2] : default_name();
        const esp_err_t ret = name != NULL ?
            solar_os_haptic_stop(name) : ESP_ERR_NOT_FOUND;
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "haptic: %s\r\n",
                                     esp_err_to_name(ret));
        }
        return;
    }
    if (argc < 3 || argc > 4 || strcmp(argv[1], "play") != 0) {
        solar_os_shell_io_writeln(
            term,
            "usage: haptic [list] | haptic play <effect> [name] | "
            "haptic stop [name]");
        return;
    }
    char *end = NULL;
    const unsigned long effect = strtoul(argv[2], &end, 0);
    const char *name = argc == 4 ? argv[3] : default_name();
    if (name == NULL || end == argv[2] || *end != '\0' || effect > UINT16_MAX) {
        solar_os_shell_io_writeln(term, "haptic: invalid effect or no device");
        return;
    }
    const esp_err_t ret = solar_os_haptic_play_effect(name, (uint16_t)effect);
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "haptic: %s\r\n", esp_err_to_name(ret));
    }
}
