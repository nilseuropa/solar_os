#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_pocsag_job.h"
#include "solar_os_shell_io.h"

void solar_os_shell_cmd_pocsag(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (argc != 2 || strcmp(argv[1], "status") != 0) {
        solar_os_shell_io_writeln(io, "usage: pocsag status");
        solar_os_shell_io_writeln(
            io,
            "start: job start pocsag <radio> <frequency-hz> <baud> <ric> [alpha|numeric] [normal|inverted]");
        return;
    }

    solar_os_pocsag_job_status_t status;
    solar_os_pocsag_job_get_status(&status);
    if (!status.running) {
        solar_os_shell_io_writeln(io, "POCSAG receiver: stopped");
        return;
    }

    solar_os_shell_io_printf(io,
                             "POCSAG receiver: running on %s\n"
                             "Frequency: %" PRIu32 " Hz, baud: %" PRIu32
                             ", RIC: %" PRIu32 ", format: %s, polarity: %s\n"
                             "Batches: %" PRIu32 ", messages: %" PRIu32
                             ", duplicates: %" PRIu32 ", receive errors: %" PRIu32 "\n"
                             "Corrected codewords: %" PRIu32
                             ", uncorrectable codewords: %" PRIu32
                             ", last RSSI: %d dBm, last error: %s\n",
                             status.radio,
                             status.frequency_hz,
                             status.baud,
                             status.ric,
                             solar_os_pocsag_format_name(status.format),
                             status.inverted ? "inverted" : "normal",
                             status.batches,
                             status.messages,
                             status.duplicates,
                             status.receive_errors,
                             status.corrected_codewords,
                             status.uncorrectable_codewords,
                             status.last_rssi_dbm,
                             esp_err_to_name(status.last_error));
}

