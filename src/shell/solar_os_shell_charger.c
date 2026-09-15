#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_charger.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static EXT_RAM_BSS_ATTR solar_os_charger_info_t info;
    return solar_os_charger_get(0U, &info) ? info.name : NULL;
}

static void print_range(solar_os_shell_io_t *term,
                        const char *label,
                        const solar_os_charger_range_t *range,
                        const char *unit)
{
    solar_os_shell_io_printf(term, "%s=%u..%u/%u%s ", label,
                             range->minimum, range->maximum, range->step, unit);
}

static void list(solar_os_shell_io_t *term)
{
    solar_os_charger_info_t info;
    for (size_t i = 0; solar_os_charger_get(i, &info); i++) {
        solar_os_shell_io_printf(term, "%s  %s  ", info.name, info.driver);
        print_range(term, "input", &info.input_current_limit_ma, "mA");
        print_range(term, "charge", &info.charge_current_ma, "mA");
        print_range(term, "voltage", &info.charge_voltage_mv, "mV");
        solar_os_shell_io_write(term, "\r\n");
    }
}

static void status(solar_os_shell_io_t *term, const char *name)
{
    solar_os_charger_status_t value;
    const esp_err_t ret = name != NULL ?
        solar_os_charger_read_status(name, &value) : ESP_ERR_NOT_FOUND;
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "charger: %s\r\n", esp_err_to_name(ret));
        return;
    }
    solar_os_shell_io_printf(
        term,
        "%s enabled=%s input=%s power-good=%s state=%s "
        "input-limit=%umA charge-current=%umA charge-voltage=%umV "
        "fault=0x%02x\r\n",
        name,
        value.enabled ? "yes" : "no",
        value.input_present ? "yes" : "no",
        value.power_good ? "yes" : "no",
        solar_os_charger_state_name(value.state),
        value.input_current_limit_ma,
        value.charge_current_ma,
        value.charge_voltage_mv,
        value.fault);
}

static bool parse_u16(const char *text, uint16_t *value)
{
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

void solar_os_shell_cmd_charger(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        list(term);
        return;
    }
    if (strcmp(argv[1], "status") == 0 && argc <= 3) {
        status(term, argc == 3 ? argv[2] : default_name());
        return;
    }
    if (strcmp(argv[1], "enable") == 0 && argc >= 3 && argc <= 4) {
        bool enabled;
        if (strcmp(argv[2], "on") == 0) {
            enabled = true;
        } else if (strcmp(argv[2], "off") == 0) {
            enabled = false;
        } else {
            solar_os_shell_io_writeln(term, "charger: expected on or off");
            return;
        }
        const char *name = argc == 4 ? argv[3] : default_name();
        const esp_err_t ret = name != NULL ?
            solar_os_charger_set_enabled(name, enabled) : ESP_ERR_NOT_FOUND;
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term, "charger: %s\r\n", esp_err_to_name(ret));
        }
        return;
    }
    if (argc >= 3 && argc <= 4) {
        uint16_t value;
        if (!parse_u16(argv[2], &value)) {
            solar_os_shell_io_writeln(term, "charger: invalid value");
            return;
        }
        const char *name = argc == 4 ? argv[3] : default_name();
        esp_err_t ret = ESP_ERR_INVALID_ARG;
        if (name == NULL) {
            ret = ESP_ERR_NOT_FOUND;
        } else if (strcmp(argv[1], "input-limit") == 0) {
            ret = solar_os_charger_set_input_current_limit(name, value);
        } else if (strcmp(argv[1], "current") == 0) {
            ret = solar_os_charger_set_charge_current(name, value);
        } else if (strcmp(argv[1], "voltage") == 0) {
            ret = solar_os_charger_set_charge_voltage(name, value);
        } else {
            goto usage;
        }
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term, "charger: %s\r\n", esp_err_to_name(ret));
        }
        return;
    }

usage:
    solar_os_shell_io_writeln(
        term,
        "usage: charger [list] | charger status [name] | "
        "charger enable <on|off> [name] | "
        "charger <input-limit|current|voltage> <value> [name]");
}
