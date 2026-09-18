#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "sim7670.h"
#include "solar_os_shell.h"
#include "solar_os_sim7670.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static EXT_RAM_BSS_ATTR solar_os_sim7670_info_t info;
    return solar_os_sim7670_get(0U, &info) ? info.name : NULL;
}

static bool find_info(const char *name, solar_os_sim7670_info_t *info)
{
    for (size_t i = 0; solar_os_sim7670_get(i, info); i++) {
        if (strcmp(info->name, name) == 0) {
            return true;
        }
    }
    return false;
}

void solar_os_shell_cmd_modem(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        if (solar_os_sim7670_count() == 0U) {
            solar_os_shell_io_writeln(term, "no SIM7670 modems registered");
            return;
        }
        solar_os_sim7670_info_t info;
        for (size_t i = 0; solar_os_sim7670_get(i, &info); i++) {
            solar_os_shell_io_printf(term,
                                     "%s  SIM7670  uart=%s  gnss=%s\r\n",
                                     info.name,
                                     info.uart_bus,
                                     info.gnss_powered ? "on" : "off");
        }
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: modem status [name]");
            return;
        }
        const char *name = argc == 3 ? argv[2] : default_name();
        if (name == NULL) {
            solar_os_shell_io_writeln(term, "modem: no device");
            return;
        }
        sim7670_status_t status;
        const esp_err_t ret = solar_os_sim7670_read_status(name, &status);
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term, "modem: %s\r\n", esp_err_to_name(ret));
            return;
        }
        solar_os_sim7670_info_t info;
        const bool have_info = find_info(name, &info);
        const char *sim_state = status.sim_status_valid
            ? (status.sim_ready ? "ready" : "not-ready")
            : "unknown";
        solar_os_shell_io_printf(term,
                                 "%s online=%s sim=%s registration=%s gnss=%s\r\n",
                                 name,
                                 status.online ? "yes" : "no",
                                 sim_state,
                                 sim7670_registration_name(status.registration),
                                 have_info && info.gnss_powered ? "on" : "off");
        if (status.signal_status_valid && status.rssi_valid) {
            solar_os_shell_io_printf(term,
                                     "rssi=%d dBm ",
                                     status.rssi_dbm);
        } else {
            solar_os_shell_io_write(term, "rssi=unknown ");
        }
        if (status.signal_status_valid && status.bit_error_rate != 99U) {
            solar_os_shell_io_printf(term, "ber=%u\r\n", status.bit_error_rate);
        } else {
            solar_os_shell_io_writeln(term, "ber=unknown");
        }
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "at") == 0) {
        if (argc < 3 || argc > 5) {
            solar_os_shell_io_writeln(
                term,
                "usage: modem at <quoted-command> [name] [timeout-ms]");
            return;
        }
        const char *name = argc >= 4 ? argv[3] : default_name();
        const uint32_t timeout_ms = argc == 5
            ? (uint32_t)strtoul(argv[4], NULL, 0)
            : 5000U;
        if (name == NULL || timeout_ms == 0U) {
            solar_os_shell_io_writeln(term, "modem: no device or invalid timeout");
            return;
        }
        static EXT_RAM_BSS_ATTR char response[512];
        const esp_err_t ret = solar_os_sim7670_command(name,
                                                       argv[2],
                                                       timeout_ms,
                                                       response,
                                                       sizeof(response));
        if (response[0] != '\0') {
            solar_os_shell_io_write(term, response);
            const size_t len = strlen(response);
            if (len == 0U || response[len - 1U] != '\n') {
                solar_os_shell_io_write(term, "\r\n");
            }
        }
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term, "modem: %s\r\n", esp_err_to_name(ret));
        }
        return;
    }

    solar_os_shell_io_writeln(
        term,
        "usage: modem [list] | modem status [name] | "
        "modem at <quoted-command> [name] [timeout-ms]");
}
