#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_gnss.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static solar_os_gnss_info_t info;
    return solar_os_gnss_get(0U, &info) ? info.name : NULL;
}

static void print_coordinate(solar_os_shell_io_t *term,
                             const char *label,
                             int32_t degrees_e7)
{
    const int64_t value = degrees_e7;
    const uint64_t magnitude = (uint64_t)(value < 0 ? -value : value);
    solar_os_shell_io_printf(term,
                             "%s=%s%" PRIu64 ".%07" PRIu64,
                             label,
                             value < 0 ? "-" : "",
                             magnitude / 10000000ULL,
                             magnitude % 10000000ULL);
}

void solar_os_shell_cmd_gnss(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_gnss_info_t info;
        for (size_t i = 0; solar_os_gnss_get(i, &info); i++) {
            solar_os_shell_io_printf(term, "%s  %s\r\n", info.name, info.driver);
        }
        return;
    }
    if (argc < 2 || argc > 4 || strcmp(argv[1], "fix") != 0) {
        solar_os_shell_io_writeln(term, "usage: gnss [list] | gnss fix [name] [timeout-ms]");
        return;
    }
    const char *name = argc >= 3 ? argv[2] : default_name();
    const uint32_t timeout_ms = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 1000U;
    if (name == NULL || timeout_ms == 0U) {
        solar_os_shell_io_writeln(term, "gnss: no receiver or invalid timeout");
        return;
    }
    solar_os_gnss_fix_t fix;
    const esp_err_t ret = solar_os_gnss_read_fix(name, timeout_ms, &fix);
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "gnss: %s\r\n", esp_err_to_name(ret));
        return;
    }
    solar_os_shell_io_printf(term,
                             "%s fix=%s type=%u satellites=%u\r\n",
                             name,
                             fix.valid ? "valid" : "invalid",
                             fix.fix_type,
                             fix.satellites);
    print_coordinate(term, "lat", fix.latitude_deg_e7);
    solar_os_shell_io_write(term, " ");
    print_coordinate(term, "lon", fix.longitude_deg_e7);
    solar_os_shell_io_printf(term,
                             " height-mm=%" PRId32 " hacc-mm=%" PRIu32 "\r\n",
                             fix.height_msl_mm,
                             fix.horizontal_accuracy_mm);
    if (fix.time_valid) {
        solar_os_shell_io_printf(term,
                                 "utc=%04u-%02u-%02uT%02u:%02u:%02uZ\r\n",
                                 fix.year, fix.month, fix.day,
                                 fix.hour, fix.minute, fix.second);
    }
}
