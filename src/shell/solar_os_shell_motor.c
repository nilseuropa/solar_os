#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_motor.h"

static void usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage: motor [list] | status <name> | drive <name> <-1..1> | coast|brake|clear <name>");
}

void solar_os_shell_cmd_motor(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_motor_info_t info;
        if (!solar_os_motor_get(0U, &info)) solar_os_shell_io_writeln(io, "no motors attached");
        for (size_t i = 0; solar_os_motor_get(i, &info); i++) {
            solar_os_shell_io_table_printf(io, "%s driver=%s drive=%u brake=%u clear=%u\r\n",
                info.name, info.driver, info.can_drive, info.can_brake, info.can_clear);
        }
        return;
    }
    esp_err_t ret;
    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_motor_status_t status;
        ret = solar_os_motor_status(argv[2], &status);
        if (ret == ESP_OK) {
            solar_os_shell_io_printf(io,
                "%s driver=%s mode=%s drive=%.4f drive-valid=%u faults=0x%lx blocking=0x%lx int=%s",
                status.info.name, status.info.driver, solar_os_motor_mode_name(status.mode),
                (double)status.drive, status.drive_valid, (unsigned long)status.faults,
                (unsigned long)status.blocking_faults,
                !status.fault_input_present ? "unwired" : status.fault_input_asserted ? "asserted" : "inactive");
            if (status.voltage_valid) solar_os_shell_io_printf(io, " nominal-mV=%lu", (unsigned long)status.voltage_mv);
            if (status.raw_valid) solar_os_shell_io_printf(io, " control=0x%lx fault=0x%lx",
                (unsigned long)status.raw_control, (unsigned long)status.raw_fault);
            solar_os_shell_io_writeln(io, "");
        }
    } else if (argc == 4 && strcmp(argv[1], "drive") == 0) {
        char *end;
        errno = 0;
        const float value = strtof(argv[3], &end);
        if (errno || end == argv[3] || *end != '\0' || !isfinite(value) || value < -1.0f || value > 1.0f) { usage(io); return; }
        ret = solar_os_motor_drive(argv[2], value);
    } else if (argc == 3 && strcmp(argv[1], "coast") == 0) ret = solar_os_motor_coast(argv[2]);
    else if (argc == 3 && strcmp(argv[1], "brake") == 0) ret = solar_os_motor_brake(argv[2]);
    else if (argc == 3 && strcmp(argv[1], "clear") == 0) ret = solar_os_motor_clear(argv[2]);
    else { usage(io); return; }
    if (ret == ESP_ERR_INVALID_STATE && argc == 4 && strcmp(argv[1], "drive") == 0) {
        solar_os_motor_status_t status;
        if (solar_os_motor_status(argv[2], &status) == ESP_OK && status.blocking_faults != 0U) {
            solar_os_shell_io_printf(io,
                "motor: fault blocks drive (faults=0x%lx); inspect 'motor status %s' before 'motor clear %s'\r\n",
                (unsigned long)status.blocking_faults, argv[2], argv[2]);
            return;
        }
    }
    if (ret != ESP_OK) solar_os_shell_io_printf(io, "motor: %s\r\n", solar_os_shell_error_text(ret));
}
