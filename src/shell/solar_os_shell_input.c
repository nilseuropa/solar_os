#include "solar_os_shell_commands.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_input.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char *const input_subcommands[] = {
    "status", "keyboard", "touch", "mouse", "joystick", "dpad", "buttons",
};

static bool input_parse_class(const char *text,
                              solar_os_input_source_class_t *source_class)
{
    if (text == NULL || source_class == NULL) {
        return false;
    }
    for (int value = SOLAR_OS_INPUT_SOURCE_KEYBOARD;
         value < SOLAR_OS_INPUT_SOURCE_CLASS_COUNT;
         value++) {
        const solar_os_input_source_class_t candidate =
            (solar_os_input_source_class_t)value;
        if (strcmp(text, solar_os_input_source_class_name(candidate)) == 0) {
            *source_class = candidate;
            return true;
        }
    }
    return false;
}

static void input_append_capability(char *buffer,
                                    size_t buffer_len,
                                    const char *name)
{
    if (buffer == NULL || buffer_len == 0 || name == NULL) {
        return;
    }
    if (buffer[0] != '\0') {
        strlcat(buffer, ",", buffer_len);
    }
    strlcat(buffer, name, buffer_len);
}

static void input_format_capabilities(uint32_t capabilities,
                                      char *buffer,
                                      size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';
    if ((capabilities & SOLAR_OS_INPUT_CAP_KEY_EVENTS) != 0) {
        input_append_capability(buffer, buffer_len, "keys");
    }
    if ((capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) != 0) {
        input_append_capability(buffer, buffer_len, "absolute");
    }
    if ((capabilities & SOLAR_OS_INPUT_CAP_POINTER_RELATIVE) != 0) {
        input_append_capability(buffer, buffer_len, "relative");
    }
    if ((capabilities & SOLAR_OS_INPUT_CAP_POINTER_BUTTONS) != 0) {
        input_append_capability(buffer, buffer_len, "pointer-buttons");
    }
    if ((capabilities & SOLAR_OS_INPUT_CAP_SCROLL) != 0) {
        input_append_capability(buffer, buffer_len, "scroll");
    }
    if ((capabilities & SOLAR_OS_INPUT_CAP_AXIS_EVENTS) != 0) {
        input_append_capability(buffer, buffer_len, "axes");
    }
    if (buffer[0] == '\0') {
        strlcpy(buffer, "-", buffer_len);
    }
}

static void input_print_sources(solar_os_shell_io_t *io,
                                bool filter,
                                solar_os_input_source_class_t source_class)
{
    size_t shown = 0;
    solar_os_shell_io_writeln(io, "SOURCE           CLASS      READY CAPABILITIES");
    const size_t count = solar_os_input_source_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_input_source_info_t info;
        if (!solar_os_input_source_get(i, &info) ||
            (filter && info.source_class != source_class)) {
            continue;
        }
        char capabilities[64];
        input_format_capabilities(info.capabilities,
                                  capabilities,
                                  sizeof(capabilities));
        solar_os_shell_io_printf(io,
                                 "%-16s %-10s %-5s %s\n",
                                 info.name,
                                 solar_os_input_source_class_name(info.source_class),
                                 info.ready ? "yes" : "no",
                                 capabilities);
        shown++;
    }
    if (shown == 0) {
        if (filter) {
            solar_os_shell_io_printf(io,
                                     "no %s sources\n",
                                     solar_os_input_source_class_name(source_class));
        } else {
            solar_os_shell_io_writeln(io, "no input sources");
        }
    }
}

void solar_os_shell_cmd_input(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        input_print_sources(io, false, SOLAR_OS_INPUT_SOURCE_OTHER);
        return;
    }

    solar_os_input_source_class_t source_class;
    if (argc >= 2 && input_parse_class(argv[1], &source_class)) {
        if (argc == 2 || (argc == 3 && strcmp(argv[2], "status") == 0)) {
            input_print_sources(io, true, source_class);
            return;
        }
        if (argc > 3) {
            solar_os_shell_diag_unexpected(io,
                                           "input",
                                           argv[3],
                                           "input <class> [status]");
        } else {
            solar_os_shell_diag_unknown(io,
                                        "input",
                                        "subcommand",
                                        argv[2],
                                        NULL,
                                        "input <class> [status]");
        }
        return;
    }

    solar_os_shell_diag_subcommand(io,
                                   "input",
                                   argc,
                                   argv,
                                   "input [status|keyboard|touch|mouse|joystick|dpad|buttons]",
                                   input_subcommands,
                                   sizeof(input_subcommands) /
                                       sizeof(input_subcommands[0]));
}
