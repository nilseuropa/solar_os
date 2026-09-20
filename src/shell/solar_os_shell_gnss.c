#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_gnss.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static EXT_RAM_BSS_ATTR solar_os_gnss_info_t info;
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

static bool parse_timeout(const char *text, uint32_t *timeout_ms)
{
    if (text == NULL || text[0] == '\0' || timeout_ms == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > UINT32_MAX) {
        return false;
    }
    *timeout_ms = (uint32_t)parsed;
    return true;
}

static void print_satellite_count(solar_os_shell_io_t *term,
                                  const solar_os_gnss_fix_t *fix)
{
    if (fix->satellites_valid) {
        solar_os_shell_io_printf(term, "%u\r\n", fix->satellites);
    } else {
        solar_os_shell_io_writeln(term, "unknown");
    }
}

static void show_status(solar_os_shell_io_t *term,
                        int argc,
                        char **argv)
{
    if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: gnss status [name]");
        return;
    }
    const char *name = argc == 3 ? argv[2] : default_name();
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "gnss: no receiver");
        return;
    }
    solar_os_gnss_status_t status;
    const esp_err_t ret = solar_os_gnss_get_status(name, 1000U, &status);
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "gnss status: %s\r\n",
                                 esp_err_to_name(ret));
        return;
    }
    const char *power = status.power_control
        ? (status.powered ? "on" : "off")
        : "always-on";
    if (!status.fix_available) {
        solar_os_shell_io_printf(term,
                                 "%s power=%s fix=unavailable\r\n",
                                 name,
                                 power);
        return;
    }
    solar_os_shell_io_printf(term,
                             "%s power=%s fix=%s type=%u satellites=",
                             name,
                             power,
                             status.fix.valid ? "valid" : "invalid",
                             status.fix.fix_type);
    print_satellite_count(term, &status.fix);
}

void solar_os_shell_cmd_gnss(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        if (solar_os_gnss_count() == 0U) {
            solar_os_shell_io_writeln(term, "no GNSS receivers registered");
            return;
        }
        solar_os_gnss_info_t info;
        for (size_t i = 0; solar_os_gnss_get(i, &info); i++) {
            solar_os_shell_io_printf(term,
                                     "%s  %s  power=%s\r\n",
                                     info.name,
                                     info.driver,
                                     info.power_control
                                         ? (info.powered ? "on" : "off")
                                         : "always-on");
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        show_status(term, argc, argv);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "power") == 0) {
        if (argc < 3 || argc > 4 ||
            (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
            solar_os_shell_io_writeln(term, "usage: gnss power <on|off> [name]");
            return;
        }
        const char *name = argc == 4 ? argv[3] : default_name();
        if (name == NULL) {
            solar_os_shell_io_writeln(term, "gnss: no receiver");
            return;
        }
        const bool enabled = strcmp(argv[2], "on") == 0;
        const esp_err_t ret = solar_os_gnss_set_power(name, enabled);
        if (ret != ESP_OK) {
            solar_os_shell_io_printf(term, "gnss: %s\r\n", esp_err_to_name(ret));
            return;
        }
        solar_os_shell_io_printf(term, "%s power=%s\r\n", name, enabled ? "on" : "off");
        return;
    }
    if (argc < 2 || argc > 4 || strcmp(argv[1], "fix") != 0) {
        solar_os_shell_io_writeln(term,
                                  "usage: gnss [list] | gnss status [name] | "
                                  "gnss power <on|off> [name] | gnss fix [name] "
                                  "[timeout-ms]");
        return;
    }
    const char *name = default_name();
    uint32_t timeout_ms = 1000U;
    if (argc == 3 && !parse_timeout(argv[2], &timeout_ms)) {
        name = argv[2];
    } else if (argc == 4) {
        name = argv[2];
        if (!parse_timeout(argv[3], &timeout_ms)) {
            solar_os_shell_io_writeln(term, "gnss: invalid timeout");
            return;
        }
    }
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "gnss: no receiver");
        return;
    }
    solar_os_gnss_fix_t fix;
    const esp_err_t ret = solar_os_gnss_read_fix(name, timeout_ms, &fix);
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "gnss: %s\r\n", esp_err_to_name(ret));
        return;
    }
    solar_os_shell_io_printf(term,
                             "%s fix=%s type=%u satellites=",
                             name,
                             fix.valid ? "valid" : "invalid",
                             fix.fix_type);
    print_satellite_count(term, &fix);
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
