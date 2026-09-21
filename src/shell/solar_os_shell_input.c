#include "solar_os_shell_commands.h"

#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_input.h"
#include "solar_os_input_actions.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char *const input_subcommands[] = {
    "status", "test", "calibrate", "emit",
    "keyboard", "touch", "mouse", "joystick", "dpad", "buttons", "gesture",
};

static const char *const input_usage =
    "input [status|test <source>|calibrate <source> [set ...|reset]|emit <key>|"
    "keyboard|touch|mouse|joystick|dpad|buttons|gesture]";

static const char *const gesture_subcommands[] = {
    "status", "bind", "bindings", "unbind",
};

static const char *const gesture_usage =
    "gesture [status|bind source=<name|*> gesture=<name> "
    "[direction=<name|*>] [cooldown=<ms>] -- <command>|bindings|unbind <id|all>]";

static const char *const gesture_bind_usage =
    "gesture bind source=<name|*> gesture=<name> [direction=<name|*>] "
    "[cooldown=<0..3600000>] -- <command> [args...]";

static bool input_append_command_token(char *line,
                                       size_t line_len,
                                       const char *token)
{
    if (line == NULL || line_len == 0 || token == NULL || token[0] == '\0') {
        return false;
    }
    const size_t used = strlen(line);
    const size_t token_len = strlen(token);
    bool quote = false;
    size_t escaped_len = token_len;
    for (const char *p = token; *p != '\0'; p++) {
        if (isspace((unsigned char)*p)) {
            quote = true;
        }
        if (*p == '"' || *p == '\'') {
            quote = true;
        }
        if (*p == '"' || *p == '\\') {
            quote = true;
            escaped_len++;
        }
    }
    const size_t append_len = token_len + (quote ? escaped_len - token_len + 2U : 0U);
    if (used + (used > 0U ? 1U : 0U) + append_len + 1U > line_len) {
        return false;
    }
    char *out = &line[used];
    if (used > 0U) {
        *out++ = ' ';
    }
    if (!quote) {
        memcpy(out, token, token_len + 1U);
        return true;
    }
    *out++ = '"';
    for (const char *p = token; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            *out++ = '\\';
        }
        *out++ = *p;
    }
    *out++ = '"';
    *out = '\0';
    return true;
}

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
    if ((capabilities & SOLAR_OS_INPUT_CAP_GESTURE_EVENTS) != 0) {
        input_append_capability(buffer, buffer_len, "gestures");
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
                             "events: key=%" PRIu32 " pointer=%" PRIu32 " axis=%" PRIu32 " gesture=%" PRIu32 "\n",
                             diagnostics.key_events,
                             diagnostics.pointer_events,
                             diagnostics.axis_events,
                             diagnostics.gesture_events);
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
    if (diagnostics.has_gesture) {
        const solar_os_input_gesture_event_t *event = &diagnostics.last_gesture;
        solar_os_shell_io_printf(io,
                                 "last gesture: gesture=%s direction=%s flags=0x%08" PRIx32 " value=%d raw=0x%08" PRIx32 "\n",
                                 solar_os_input_gesture_name(event->gesture),
                                 solar_os_input_gesture_direction_name(event->direction),
                                 event->flags,
                                 event->value,
                                 event->raw);
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

static void input_emit_key(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc != 3) {
        if (argc < 3) {
            solar_os_shell_diag_missing(io, "input emit", "key", "input emit <key|chord>");
        } else {
            solar_os_shell_diag_unexpected(io, "input emit", argv[3], "input emit <key|chord>");
        }
        return;
    }
    const esp_err_t err = solar_os_input_actions_emit_key(argv[2]);
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_diag_invalid(io,
                                    "input emit",
                                    "key",
                                    argv[2],
                                    "one character, a named key, or a chord such as ALT+RIGHT",
                                    "input emit <key|chord>",
                                    false);
    } else if (err != ESP_OK) {
        solar_os_shell_diag_esp(io, "emit local key", err, argv[2], NULL);
    }
}

static const char *input_option_value(const char *arg, const char *name)
{
    const size_t name_len = strlen(name);
    return strncmp(arg, name, name_len) == 0 && arg[name_len] == '='
        ? &arg[name_len + 1U]
        : NULL;
}

static void gesture_bind(solar_os_shell_io_t *io, int argc, char **argv)
{
    const char *source = "*";
    solar_os_input_gesture_t gesture = SOLAR_OS_INPUT_GESTURE_FLICK;
    bool gesture_set = false;
    bool any_direction = true;
    solar_os_input_gesture_direction_t direction =
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE;
    uint32_t cooldown_ms = SOLAR_OS_INPUT_ACTION_COOLDOWN_DEFAULT_MS;
    int separator = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            separator = i;
            break;
        }
        const char *value = input_option_value(argv[i], "source");
        if (value != NULL) {
            source = value;
            continue;
        }
        value = input_option_value(argv[i], "gesture");
        if (value != NULL) {
            if (!solar_os_input_parse_gesture(value, &gesture)) {
                solar_os_shell_diag_invalid(io,
                                            "gesture bind",
                                            "gesture",
                                            value,
                                            "flick, circle, wave, hold, presence, tap, double-tap, or airwheel",
                                            gesture_bind_usage,
                                            false);
                return;
            }
            gesture_set = true;
            continue;
        }
        value = input_option_value(argv[i], "direction");
        if (value != NULL) {
            any_direction = strcmp(value, "*") == 0 || strcmp(value, "any") == 0;
            if (!any_direction &&
                !solar_os_input_parse_gesture_direction(value, &direction)) {
                solar_os_shell_diag_invalid(io,
                                            "gesture bind",
                                            "direction",
                                            value,
                                            "*, none, west, east, north, south, center, clockwise, counterclockwise, horizontal, or vertical",
                                            gesture_bind_usage,
                                            false);
                return;
            }
            continue;
        }
        value = input_option_value(argv[i], "cooldown");
        if (value != NULL) {
            long parsed = 0;
            if (!input_parse_long(value,
                                  0,
                                  SOLAR_OS_INPUT_ACTION_COOLDOWN_MAX_MS,
                                  &parsed)) {
                solar_os_shell_diag_invalid(io,
                                            "gesture bind",
                                            "cooldown",
                                            value,
                                            "milliseconds from 0 to 3600000",
                                            gesture_bind_usage,
                                            false);
                return;
            }
            cooldown_ms = (uint32_t)parsed;
            continue;
        }
        solar_os_shell_diag_invalid(io,
                                    "gesture bind",
                                    "option",
                                    argv[i],
                                    "source=, gesture=, direction=, or cooldown=",
                                    gesture_bind_usage,
                                    false);
        return;
    }

    if (!gesture_set) {
        solar_os_shell_diag_missing(io,
                                    "gesture bind",
                                    "gesture=<name>",
                                    gesture_bind_usage);
        return;
    }
    if (separator < 0 || separator + 1 >= argc) {
        solar_os_shell_diag_missing(io,
                                    "gesture bind",
                                    "-- <command>",
                                    gesture_bind_usage);
        return;
    }

    char command[SOLAR_OS_INPUT_ACTION_COMMAND_MAX] = {0};
    for (int i = separator + 1; i < argc; i++) {
        if (!input_append_command_token(command, sizeof(command), argv[i])) {
            solar_os_shell_diag_problem(io,
                                        "gesture bind",
                                        "command is empty or too long",
                                        gesture_bind_usage,
                                        NULL);
            return;
        }
    }

    uint32_t id = 0;
    const esp_err_t err = solar_os_input_actions_bind(source,
                                                       gesture,
                                                       any_direction,
                                                       direction,
                                                       cooldown_ms,
                                                       command,
                                                       &id);
    if (err != ESP_OK) {
        solar_os_shell_diag_esp(io,
                                "create gesture binding",
                                err,
                                NULL,
                                err == ESP_ERR_NO_MEM
                                    ? "remove an unused binding and try again"
                                    : NULL);
        return;
    }
    if (solar_os_input_actions_running()) {
        solar_os_shell_io_printf(io, "gesture binding %" PRIu32 " created\n", id);
    } else {
        solar_os_shell_io_printf(
            io,
            "gesture binding %" PRIu32
            " created; start with: job start gesture-listener\n",
            id);
    }
}

static void gesture_list_bindings(solar_os_shell_io_t *io)
{
    solar_os_shell_io_printf(io,
                             "listener: %s\n",
                             solar_os_input_actions_running()
                                 ? "running"
                                 : "stopped");
    const size_t count = solar_os_input_actions_count();
    if (count == 0U) {
        solar_os_shell_io_writeln(io, "no gesture bindings");
        return;
    }
    solar_os_shell_io_writeln(
        io, "ID SOURCE           GESTURE     DIRECTION        COOLDOWN FIRED DROPPED COMMAND");
    for (size_t i = 0; i < count; i++) {
        solar_os_input_action_binding_t binding;
        if (!solar_os_input_actions_get(i, &binding)) {
            continue;
        }
        solar_os_shell_io_printf(
            io,
            "%-2" PRIu32 " %-16s %-11s %-16s %8" PRIu32 " %5" PRIu32 " %7" PRIu32 " %s\n",
            binding.id,
            binding.any_source ? "*" : binding.source,
            solar_os_input_gesture_name(binding.gesture),
            binding.any_direction
                ? "*"
                : solar_os_input_gesture_direction_name(binding.direction),
            binding.cooldown_ms,
            binding.trigger_count,
            binding.dropped_count,
            binding.command);
    }
}

static void gesture_print_sources(solar_os_shell_io_t *io)
{
    size_t shown = 0U;
    solar_os_shell_io_writeln(io, "SOURCE           READY GESTURES");
    for (size_t i = 0; i < solar_os_input_source_count(); i++) {
        solar_os_input_source_info_t source;
        if (!solar_os_input_source_get(i, &source) ||
            (source.capabilities & SOLAR_OS_INPUT_CAP_GESTURE_EVENTS) == 0U) {
            continue;
        }
        char gestures[96] = {0};
        for (int value = SOLAR_OS_INPUT_GESTURE_FLICK;
             value < SOLAR_OS_INPUT_GESTURE_COUNT;
             value++) {
            if ((source.gesture_mask &
                 SOLAR_OS_INPUT_GESTURE_MASK(value)) == 0U) {
                continue;
            }
            if (gestures[0] != '\0') {
                strlcat(gestures, ",", sizeof(gestures));
            }
            strlcat(gestures,
                    solar_os_input_gesture_name(
                        (solar_os_input_gesture_t)value),
                    sizeof(gestures));
        }
        solar_os_shell_io_printf(io,
                                 "%-16s %-5s %s\n",
                                 source.name,
                                 source.ready ? "yes" : "no",
                                 gestures[0] != '\0' ? gestures : "-");
        shown++;
    }
    if (shown == 0U) {
        solar_os_shell_io_writeln(io, "no gesture sources");
    }
}

static void gesture_unbind(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc != 3) {
        if (argc < 3) {
            solar_os_shell_diag_missing(io,
                                        "gesture unbind",
                                        "binding ID or all",
                                        "gesture unbind <id|all>");
        } else {
            solar_os_shell_diag_unexpected(io,
                                           "gesture unbind",
                                           argv[3],
                                           "gesture unbind <id|all>");
        }
        return;
    }
    if (strcmp(argv[2], "all") == 0) {
        const size_t removed = solar_os_input_actions_clear();
        solar_os_shell_io_printf(io, "removed %u gesture binding%s\n",
                                 (unsigned)removed,
                                 removed == 1U ? "" : "s");
        return;
    }
    long parsed = 0;
    if (!input_parse_long(argv[2], 1, LONG_MAX, &parsed)) {
        solar_os_shell_diag_invalid(io,
                                    "gesture unbind",
                                    "binding ID",
                                    argv[2],
                                    "positive integer or all",
                                    "gesture unbind <id|all>",
                                    false);
        return;
    }
    const uint32_t id = (uint32_t)parsed;
    const esp_err_t err = solar_os_input_actions_unbind(id);
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(io, "gesture unbind: no such binding: %" PRIu32 "\n", id);
    } else if (err != ESP_OK) {
        solar_os_shell_diag_esp(io, "remove gesture binding", err, argv[2], NULL);
    } else {
        solar_os_shell_io_printf(io, "gesture binding %" PRIu32 " removed\n", id);
    }
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
    if (argc >= 2 && strcmp(argv[1], "emit") == 0) {
        input_emit_key(io, argc, argv);
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

void solar_os_shell_cmd_gesture(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        gesture_print_sources(io);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "bind") == 0) {
        gesture_bind(io, argc, argv);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "bindings") == 0) {
        gesture_list_bindings(io);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "unbind") == 0) {
        gesture_unbind(io, argc, argv);
        return;
    }

    solar_os_shell_diag_subcommand(io,
                                   "gesture",
                                   argc,
                                   argv,
                                   gesture_usage,
                                   gesture_subcommands,
                                   sizeof(gesture_subcommands) /
                                       sizeof(gesture_subcommands[0]));
}
