#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_controls.h"
#include "solar_os_parameters.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char control_usage[] =
    "control list|parameters|bindings\n"
    "  control create <name> <stream> <min> <max> [smooth=ms] "
    "[deadband=value] [invert]\n"
    "  control delete <name> | clear\n"
    "  control get <name> | set <name> <0..65535>\n"
    "  control bind <name> midi <channel> <cc>\n"
    "  control bind <name> parameter <app.parameter> [pickup=on|off]\n"
    "  control unbind <name>\n"
    "  control parameter get <path> | set <path> <value>\n";

static bool control_parse_float(const char *text, float *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const float parsed = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static void control_print_error(solar_os_shell_io_t *io,
                                const char *operation,
                                esp_err_t err)
{
    solar_os_shell_io_printf(io, "control: %s failed: %s\n",
                             operation, solar_os_shell_error_text(err));
}

static void control_list(solar_os_shell_io_t *io)
{
    const size_t count = solar_os_control_count();
    if (count == 0U) {
        solar_os_shell_io_table_writeln(io, "No controls configured");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_control_info_t info;
        if (!solar_os_control_get_info(i, &info)) {
            continue;
        }
        solar_os_shell_io_table_printf(io, "%s: source=%s range=%.3g..%.3g",
                                 info.config.name,
                                 info.config.source[0] != '\0' ?
                                     info.config.source : "manual",
                                 (double)info.config.input_minimum,
                                 (double)info.config.input_maximum);
        if (info.has_value) {
            solar_os_shell_io_table_printf(io, " value=%u raw=%.3g",
                                     (unsigned)info.normalized,
                                     (double)info.source_value);
        }
        solar_os_shell_io_table_printf(io,
                                 " samples=%" PRIu32 " updates=%" PRIu32,
                                 info.samples,
                                 info.updates);
        if (info.last_error != ESP_OK) {
            solar_os_shell_io_table_printf(io, " error=%s",
                                     esp_err_to_name(info.last_error));
        }
        solar_os_shell_io_table_writeln(io, "");
    }
}

static void control_list_parameters(solar_os_shell_io_t *io)
{
    const size_t count = solar_os_parameter_count();
    if (count == 0U) {
        solar_os_shell_io_table_writeln(io, "No native parameters available");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_parameter_info_t info;
        float value = 0.0f;
        if (!solar_os_parameter_get_info(i, &info)) {
            continue;
        }
        const esp_err_t err = solar_os_parameter_get(info.path, &value);
        solar_os_shell_io_table_printf(
            io,
            "%s: %s %.4g..%.4g step=%.4g curve=%s",
            info.path,
            info.label,
            (double)info.minimum,
            (double)info.maximum,
            (double)info.step,
            solar_os_parameter_curve_name(info.curve));
        if (err == ESP_OK) {
            solar_os_shell_io_table_printf(io, " value=%.4g%s%s",
                                     (double)value,
                                     info.unit[0] != '\0' ? " " : "",
                                     info.unit);
        }
        solar_os_shell_io_table_writeln(io, "");
    }
}

static void control_list_bindings(solar_os_shell_io_t *io)
{
    const size_t count = solar_os_control_binding_count();
    if (count == 0U) {
        solar_os_shell_io_table_writeln(io, "No control bindings configured");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_control_binding_info_t binding;
        if (!solar_os_control_binding_get(i, &binding)) {
            continue;
        }
        solar_os_shell_io_table_printf(io, "%s -> ", binding.control);
        if (binding.target == SOLAR_OS_CONTROL_TARGET_MIDI_CC) {
            solar_os_shell_io_table_printf(io, "midi ch=%u cc=%u",
                                     (unsigned)binding.midi_channel,
                                     (unsigned)binding.midi_controller);
        } else {
            solar_os_shell_io_table_printf(io, "%s pickup=%s%s",
                                     binding.parameter,
                                     binding.pickup ? "on" : "off",
                                     binding.pickup && binding.pickup_latched ?
                                         " latched" : "");
        }
        solar_os_shell_io_table_printf(io, " applied=%" PRIu32,
                                 binding.applied);
        if (binding.last_error != ESP_OK) {
            solar_os_shell_io_table_printf(io, " error=%s",
                                     esp_err_to_name(binding.last_error));
        }
        solar_os_shell_io_table_writeln(io, "");
    }
}

static void control_create(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc < 6) {
        solar_os_shell_diag_problem(io, "control", "not enough arguments",
                                    control_usage, NULL);
        return;
    }
    solar_os_control_config_t config = {0};
    strlcpy(config.name, argv[2], sizeof(config.name));
    if (strcmp(argv[3], "manual") != 0 && strcmp(argv[3], "-") != 0) {
        strlcpy(config.source, argv[3], sizeof(config.source));
    }
    if (!control_parse_float(argv[4], &config.input_minimum) ||
        !control_parse_float(argv[5], &config.input_maximum)) {
        solar_os_shell_diag_invalid(io, "control", "range", argv[4],
                                    "finite numeric min and max", control_usage,
                                    false);
        return;
    }
    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "invert") == 0) {
            config.inverted = true;
        } else if (strncmp(argv[i], "smooth=", 7U) == 0) {
            size_t value = 0U;
            if (!solar_os_shell_parse_size_arg(argv[i] + 7U, 0U, 60000U,
                                               &value)) {
                solar_os_shell_diag_invalid(io, "control", "smooth", argv[i],
                                            "smooth=0..60000", control_usage,
                                            false);
                return;
            }
            config.smoothing_ms = (uint32_t)value;
        } else if (strncmp(argv[i], "deadband=", 9U) == 0) {
            if (!control_parse_float(argv[i] + 9U, &config.deadband)) {
                solar_os_shell_diag_invalid(io, "control", "deadband", argv[i],
                                            "deadband=number", control_usage,
                                            false);
                return;
            }
        } else {
            solar_os_shell_diag_unexpected(io, "control", argv[i],
                                           control_usage);
            return;
        }
    }
    const esp_err_t err = solar_os_control_create(&config);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(io, "Control %s created\n", config.name);
    } else if (err == ESP_ERR_NOT_FOUND && config.source[0] != '\0') {
        solar_os_shell_io_printf(io,
                                 "control: create failed: %s stream does not exist\n",
                                 config.source);
    } else if (err == ESP_ERR_NOT_SUPPORTED && config.source[0] != '\0') {
        solar_os_shell_io_printf(io,
                                 "control: create failed: %s is not a scalar stream\n",
                                 config.source);
    } else {
        control_print_error(io, "create", err);
    }
}

static void control_bind(solar_os_shell_io_t *io, int argc, char **argv)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (argc == 6 && strcmp(argv[3], "midi") == 0) {
        uint8_t channel = 0U;
        uint8_t controller = 0U;
        if (solar_os_shell_parse_u8(argv[4], &channel) && channel >= 1U &&
            channel <= 16U &&
            solar_os_shell_parse_u8(argv[5], &controller) &&
            controller <= 127U) {
            err = solar_os_control_bind_midi_cc(argv[2], channel, controller,
                                                NULL);
        }
    } else if ((argc == 5 || argc == 6) &&
               strcmp(argv[3], "parameter") == 0) {
        bool pickup = false;
        if (argc == 6) {
            if (strcmp(argv[5], "pickup=on") == 0) {
                pickup = true;
            } else if (strcmp(argv[5], "pickup=off") != 0) {
                solar_os_shell_diag_invalid(io, "control", "pickup", argv[5],
                                            "pickup=on or pickup=off",
                                            control_usage, false);
                return;
            }
        }
        err = solar_os_control_bind_parameter(argv[2], argv[4], pickup, NULL);
    } else {
        solar_os_shell_diag_problem(io, "control", "invalid binding",
                                    control_usage, NULL);
        return;
    }
    if (err == ESP_OK) {
        solar_os_shell_io_printf(io, "Control %s bound\n", argv[2]);
    } else {
        control_print_error(io, "bind", err);
    }
}

static void control_parameter(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[2], "get") == 0) {
        float value = 0.0f;
        const esp_err_t err = solar_os_parameter_get(argv[3], &value);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io, "%s = %.6g\n", argv[3],
                                     (double)value);
        } else {
            control_print_error(io, "parameter get", err);
        }
        return;
    }
    if (argc == 5 && strcmp(argv[2], "set") == 0) {
        float value = 0.0f;
        if (!control_parse_float(argv[4], &value)) {
            solar_os_shell_diag_invalid(io, "control", "value", argv[4],
                                        "number", control_usage, false);
            return;
        }
        esp_err_t err = solar_os_parameter_set(argv[3], value);
        if (err == ESP_OK) {
            err = solar_os_parameter_get(argv[3], &value);
        }
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io, "%s = %.6g\n", argv[3],
                                     (double)value);
        } else {
            control_print_error(io, "parameter set", err);
        }
        return;
    }
    solar_os_shell_diag_problem(io, "control", "invalid parameter command",
                                control_usage, NULL);
}

void solar_os_shell_cmd_control(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        control_list(io);
    } else if (argc == 2 && strcmp(argv[1], "parameters") == 0) {
        control_list_parameters(io);
    } else if (argc == 2 && strcmp(argv[1], "bindings") == 0) {
        control_list_bindings(io);
    } else if (argc >= 2 && strcmp(argv[1], "create") == 0) {
        control_create(io, argc, argv);
    } else if (argc == 3 && strcmp(argv[1], "delete") == 0) {
        const esp_err_t err = solar_os_control_delete(argv[2]);
        if (err != ESP_OK) {
            control_print_error(io, "delete", err);
        }
    } else if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        solar_os_control_clear();
        solar_os_shell_io_writeln(io, "Controls cleared");
    } else if (argc == 3 && strcmp(argv[1], "get") == 0) {
        uint16_t value = 0U;
        const esp_err_t err = solar_os_control_get(argv[2], &value);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io, "%s = %u\n", argv[2],
                                     (unsigned)value);
        } else {
            control_print_error(io, "get", err);
        }
    } else if (argc == 4 && strcmp(argv[1], "set") == 0) {
        size_t value = 0U;
        if (!solar_os_shell_parse_size_arg(argv[3], 0U,
                                           SOLAR_OS_CONTROL_NORMALIZED_MAX,
                                           &value)) {
            solar_os_shell_diag_invalid(io, "control", "value", argv[3],
                                        "0..65535", control_usage, false);
            return;
        }
        const esp_err_t err = solar_os_control_set(argv[2], (uint16_t)value);
        if (err != ESP_OK) {
            control_print_error(io, "set", err);
        }
    } else if (argc >= 2 && strcmp(argv[1], "bind") == 0) {
        control_bind(io, argc, argv);
    } else if (argc == 3 && strcmp(argv[1], "unbind") == 0) {
        size_t removed = 0U;
        const esp_err_t err = solar_os_control_unbind(argv[2], &removed);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "Control %s unbound (%u removed)\n",
                                     argv[2], (unsigned)removed);
        } else {
            control_print_error(io, "unbind", err);
        }
    } else if (argc >= 2 && strcmp(argv[1], "parameter") == 0) {
        control_parameter(io, argc, argv);
    } else {
        solar_os_shell_diag_problem(io, "control", "invalid command",
                                    control_usage, NULL);
    }
}
