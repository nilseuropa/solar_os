#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_input.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char *const input_subcommands[] = {
    "status", "test", "calibrate", "keyboard", "touch", "mouse", "joystick",
    "dpad", "buttons",
};

static const char *const input_usage =
    "input [status|test <source>|calibrate <source> [set <min-x> <max-x> "
    "<min-y> <max-y> <width> <height>|reset]|keyboard|touch|mouse|joystick|dpad|buttons]";

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

static const char *input_key_action_name(solar_os_input_key_action_t action)
{
    static const char *const names[] = {"press", "release", "repeat"};
    return action <= SOLAR_OS_INPUT_KEY_REPEAT ? names[action] : "invalid";
}

static bool input_find_source(solar_os_shell_io_t *io,
                              const char *name,
                              solar_os_input_source_info_t *info)
{
    if (solar_os_input_source_find(name, info)) {
        return true;
    }
    solar_os_shell_diag_unknown(io,
                                "input",
                                "source",
                                name,
                                NULL,
                                input_usage);
    return false;
}

static void input_print_calibration(
    solar_os_shell_io_t *io,
    const char *name,
    const solar_os_input_source_diagnostics_t *diagnostics)
{
    if (!diagnostics->calibration_enabled) {
        solar_os_shell_io_printf(io, "calibration: %s off\n", name);
        return;
    }
    const solar_os_input_pointer_calibration_t *calibration =
        &diagnostics->calibration;
    solar_os_shell_io_printf(io,
                             "calibration: %s x=%d..%d y=%d..%d size=%ux%u\n",
                             name,
                             calibration->min_x,
                             calibration->max_x,
                             calibration->min_y,
                             calibration->max_y,
                             calibration->width,
                             calibration->height);
}

static void input_test_source(solar_os_shell_io_t *io, const char *name)
{
    solar_os_input_source_info_t info;
    if (!input_find_source(io, name, &info)) {
        return;
    }
    solar_os_input_source_diagnostics_t diagnostics;
    if (!solar_os_input_source_get_diagnostics(info.source, &diagnostics)) {
        solar_os_shell_diag_problem(io,
                                    "input test",
                                    "source detached while reading it",
                                    input_usage,
                                    "run input status and try again");
        return;
    }
    char capabilities[64];
    input_format_capabilities(info.capabilities, capabilities, sizeof(capabilities));
    solar_os_shell_io_printf(io,
                             "source: %s class=%s ready=%s capabilities=%s\n",
                             info.name,
                             solar_os_input_source_class_name(info.source_class),
                             info.ready ? "yes" : "no",
                             capabilities);
    solar_os_shell_io_printf(io,
                             "events: key=%" PRIu32 " pointer=%" PRIu32 " axis=%" PRIu32 "\n",
                             diagnostics.key_events,
                             diagnostics.pointer_events,
                             diagnostics.axis_events);
    if (diagnostics.has_key) {
        const solar_os_input_key_event_t *event = &diagnostics.last_key;
        solar_os_shell_io_printf(io,
                                 "last key: action=%s physical=%u usage=%u key=%u modifiers=0x%02x\n",
                                 input_key_action_name(event->action),
                                 event->physical_key,
                                 event->usage,
                                 event->key,
                                 event->modifiers);
    }
    if (diagnostics.has_pointer) {
        const solar_os_input_pointer_event_t *raw = &diagnostics.last_pointer_raw;
        const solar_os_input_pointer_event_t *event = &diagnostics.last_pointer;
        solar_os_shell_io_printf(io,
                                 "last pointer: mode=%s action=%s raw=(%d,%d) value=(%d,%d) delta=(%d,%d) buttons=0x%02x\n",
                                 solar_os_input_pointer_mode_name(event->mode),
                                 solar_os_input_pointer_action_name(event->action),
                                 raw->x,
                                 raw->y,
                                 event->x,
                                 event->y,
                                 event->delta_x,
                                 event->delta_y,
                                 event->buttons);
    }
    if (diagnostics.has_axis) {
        const solar_os_input_axis_event_t *event = &diagnostics.last_axis;
        solar_os_shell_io_printf(io,
                                 "last axis: axis=%s value=%d delta=%" PRId32 "\n",
                                 solar_os_input_axis_name(event->axis),
                                 event->value,
                                 event->delta);
    }
    if ((info.capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) != 0) {
        input_print_calibration(io, info.name, &diagnostics);
    }
}

static bool input_parse_long(const char *text, long minimum, long maximum, long *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static void input_calibrate_source(solar_os_shell_io_t *io, int argc, char **argv)
{
    solar_os_input_source_info_t info;
    if (!input_find_source(io, argv[2], &info)) {
        return;
    }
    if ((info.capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) == 0) {
        solar_os_shell_diag_problem(io,
                                    "input calibrate",
                                    "source is not an absolute pointer",
                                    input_usage,
                                    "select a touch or other absolute-pointer source");
        return;
    }
    if (argc == 3) {
        solar_os_input_source_diagnostics_t diagnostics;
        if (solar_os_input_source_get_diagnostics(info.source, &diagnostics)) {
            input_print_calibration(io, info.name, &diagnostics);
        }
        return;
    }
    if (argc == 4 && strcmp(argv[3], "reset") == 0) {
        const esp_err_t err = solar_os_input_pointer_calibration_reset(info.source);
        if (err != ESP_OK) {
            solar_os_shell_diag_esp(io, "reset pointer calibration", err, info.name, NULL);
        } else {
            solar_os_shell_io_printf(io, "calibration: %s reset\n", info.name);
        }
        return;
    }
    if (argc == 10 && strcmp(argv[3], "set") == 0) {
        long values[6];
        for (size_t i = 0; i < 6; i++) {
            const long minimum = i < 4 ? INT16_MIN : 1;
            const long maximum = i < 4 ? INT16_MAX : 32768;
            if (!input_parse_long(argv[i + 4U], minimum, maximum, &values[i])) {
                solar_os_shell_diag_invalid(io,
                                            "input calibrate",
                                            "calibration value",
                                            argv[i + 4U],
                                            i < 4 ? "-32768..32767" : "1..32768",
                                            input_usage,
                                            false);
                return;
            }
        }
        solar_os_input_pointer_calibration_t calibration = {
            .min_x = (int16_t)values[0],
            .max_x = (int16_t)values[1],
            .min_y = (int16_t)values[2],
            .max_y = (int16_t)values[3],
            .width = (uint16_t)values[4],
            .height = (uint16_t)values[5],
        };
        const esp_err_t err = solar_os_input_pointer_calibration_set(
            info.source, &calibration);
        if (err != ESP_OK) {
            solar_os_shell_diag_esp(io,
                                    "save pointer calibration",
                                    err,
                                    info.name,
                                    "minimum values must be less than maximum values");
        } else {
            solar_os_input_source_diagnostics_t diagnostics;
            if (solar_os_input_source_get_diagnostics(info.source, &diagnostics)) {
                input_print_calibration(io, info.name, &diagnostics);
            }
        }
        return;
    }
    solar_os_shell_diag_problem(io,
                                "input calibrate",
                                "invalid calibration arguments",
                                input_usage,
                                "use set with six values, or reset");
}

void solar_os_shell_cmd_input(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        input_print_sources(io, false, SOLAR_OS_INPUT_SOURCE_OTHER);
        return;
    }

    if (argc == 3 && strcmp(argv[1], "test") == 0) {
        input_test_source(io, argv[2]);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "calibrate") == 0) {
        input_calibrate_source(io, argc, argv);
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
                                   input_usage,
                                   input_subcommands,
                                   sizeof(input_subcommands) /
                                       sizeof(input_subcommands[0]));
}
