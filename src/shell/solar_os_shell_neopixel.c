#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const neopixel_commands[] = {
    "status", "list", "set", "fill", "clear", "show",
};

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_neopixel.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void neopixel_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  neopixel [status|list] [name]");
    solar_os_shell_io_writeln(term, "  neopixel set <name> <index> <red> <green> <blue>");
    solar_os_shell_io_writeln(term, "  neopixel fill <name> <red> <green> <blue>");
    solar_os_shell_io_writeln(term, "  neopixel clear <name>");
    solar_os_shell_io_writeln(term, "  neopixel show <name>");
}

static bool parse_uint(const char *text, unsigned max, unsigned *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > max) {
        return false;
    }
    *value = (unsigned)parsed;
    return true;
}

static bool print_device(solar_os_shell_io_t *term, const char *name)
{
    const size_t count = solar_os_neopixel_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_neopixel_info_t info;
        if (!solar_os_neopixel_get(i, &info) ||
            (name != NULL && strcmp(name, info.name) != 0)) {
            continue;
        }
        solar_os_shell_io_table_printf(term,
                                 "%s GPIO%d pixels=%u order=GRB\n",
                                 info.name,
                                 info.data_pin,
                                 (unsigned)info.pixel_count);
        return name != NULL;
    }
    return false;
}

static void print_error(solar_os_shell_io_t *term, esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_writeln(term, "neopixel: device not found");
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "neopixel: invalid pixel index or color");
    } else {
        solar_os_shell_io_printf(term, "neopixel failed: %s\n", solar_os_shell_error_text(err));
    }
}

void solar_os_shell_cmd_neopixel(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "list") == 0) {
        if (argc > 3) {
            neopixel_usage(term);
            return;
        }
        const char *name = argc == 3 ? argv[2] : NULL;
        const size_t count = solar_os_neopixel_count();
        if (count == 0) {
            solar_os_shell_io_writeln(term, "no NeoPixel devices attached");
            return;
        }
        if (name != NULL && !print_device(term, name)) {
            solar_os_shell_io_writeln(term, "neopixel: device not found");
            return;
        }
        if (name == NULL) {
            for (size_t i = 0; i < count; i++) {
                solar_os_neopixel_info_t info;
                if (solar_os_neopixel_get(i, &info)) {
                    solar_os_shell_io_table_printf(term,
                                             "%s GPIO%d pixels=%u order=GRB\n",
                                             info.name,
                                             info.data_pin,
                                             (unsigned)info.pixel_count);
                }
            }
        }
        return;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(argv[1], "set") == 0 && argc == 7) {
        unsigned index = 0;
        unsigned red = 0;
        unsigned green = 0;
        unsigned blue = 0;
        if (!parse_uint(argv[3], SOLAR_OS_NEOPIXEL_MAX_PIXELS - 1U, &index) ||
            !parse_uint(argv[4], 255, &red) ||
            !parse_uint(argv[5], 255, &green) ||
            !parse_uint(argv[6], 255, &blue)) {
            neopixel_usage(term);
            return;
        }
        err = solar_os_neopixel_set(argv[2], index, red, green, blue);
        if (err == ESP_OK) {
            err = solar_os_neopixel_show(argv[2]);
        }
    } else if (strcmp(argv[1], "fill") == 0 && argc == 6) {
        unsigned red = 0;
        unsigned green = 0;
        unsigned blue = 0;
        if (!parse_uint(argv[3], 255, &red) ||
            !parse_uint(argv[4], 255, &green) ||
            !parse_uint(argv[5], 255, &blue)) {
            neopixel_usage(term);
            return;
        }
        err = solar_os_neopixel_fill(argv[2], red, green, blue);
        if (err == ESP_OK) {
            err = solar_os_neopixel_show(argv[2]);
        }
    } else if (strcmp(argv[1], "clear") == 0 && argc == 3) {
        err = solar_os_neopixel_clear(argv[2]);
    } else if (strcmp(argv[1], "show") == 0 && argc == 3) {
        err = solar_os_neopixel_show(argv[2]);
    } else {
        solar_os_shell_diag_subcommand(term,
                                       "neopixel",
                                       argc,
                                       argv,
                                       "neopixel status|list|set|fill|clear|show",
                                       neopixel_commands,
                                       sizeof(neopixel_commands) / sizeof(neopixel_commands[0]));
        return;
    }

    if (err != ESP_OK) {
        print_error(term, err);
    }
}
