#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_battery.h"

typedef int solar_os_context_t;
typedef int solar_os_shell_io_t;
static char output[4096];
static unsigned unexpected, reads;
static esp_err_t read_error;
static solar_os_battery_status_t battery;
static solar_os_battery_monitor_status_t monitor;
static const char *const battery_subcommands[] = {"status", "config"};
static solar_os_shell_io_t *terminal(solar_os_context_t *ctx) { return ctx; }
static void solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *format, ...)
{
    (void)io;
    va_list args; va_start(args, format);
    vsnprintf(output + strlen(output), sizeof(output) - strlen(output), format, args);
    va_end(args);
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{ solar_os_shell_io_printf(io, "%s\n", text); }
esp_err_t solar_os_battery_get_status(solar_os_battery_status_t *out)
{ reads++; *out = battery; return read_error; }
void solar_os_battery_monitor_get_status(solar_os_battery_monitor_status_t *out)
{ *out = monitor; }
void solar_os_battery_get_config(solar_os_battery_config_t *out)
{ *out = (solar_os_battery_config_t){2500U, 3000U, 4120U, 6000U}; }
const char *solar_os_battery_trend_name(solar_os_battery_trend_t trend)
{ return trend == SOLAR_OS_BATTERY_TREND_DISCHARGING ? "discharging" : "unknown"; }
static uint64_t solar_os_time_uptime_ms(void) { return 10000U; }
static const char *solar_os_shell_error_text(esp_err_t err) { (void)err; return "error"; }
static bool shell_print_not_supported(solar_os_shell_io_t *io,
    const char *command, const char *feature, esp_err_t err)
{ (void)io; (void)command; (void)feature; (void)err; return false; }
static void solar_os_shell_diag_unexpected(solar_os_shell_io_t *io,
    const char *command, const char *arg, const char *usage)
{ (void)io; (void)command; (void)arg; (void)usage; unexpected++; }
static void solar_os_shell_diag_subcommand(solar_os_shell_io_t *io, const char *command,
    int argc, char **argv, const char *usage, const char *const *names, size_t count)
{ (void)io; (void)command; (void)argc; (void)argv; (void)usage; (void)names; (void)count; assert(false); }
#define CONFIG_STUB(name) \
    static void battery_cmd_##name(solar_os_shell_io_t *io, int argc, char **argv) \
    { (void)io; (void)argc; (void)argv; assert(false); }
CONFIG_STUB(capacity)
CONFIG_STUB(runtime)
CONFIG_STUB(min_voltage)
CONFIG_STUB(max_voltage)

#include "battery_command.inc"

static void run(const char *subcommand)
{
    solar_os_context_t ctx = 0;
    char *argv[] = {"battery", (char *)subcommand};
    output[0] = '\0';
    solar_os_shell_cmd_battery(&ctx, subcommand ? 2 : 1, argv);
}

static void assert_short_report(void)
{
    unsigned lines = 0;
    for (const char *p = output; *p; p++) if (*p == '\n') lines++;
    assert(lines == 5U);
    assert(strstr(output, "Battery:") && strstr(output, "Charge:") &&
           strstr(output, "Power:") && strstr(output, "Charging:") && strstr(output, "Time left:"));
    assert(!strstr(output, "Capacity:") && !strstr(output, "Monitor:") && !strstr(output, "window"));
}

int main(void)
{
    battery = (solar_os_battery_status_t){.voltage_mv = 3800U, .percent = 40U,
        .adc_calibrated = true, .external_power_known = true, .charging_known = true};
    monitor = (solar_os_battery_monitor_status_t){.running = true, .sample_count = 42U,
        .interval_ms = 10000U, .time_left_valid = true, .time_left_min = 125U,
        .full_runtime_min = 360U, .trend = SOLAR_OS_BATTERY_TREND_DISCHARGING,
        .short_window = {.valid = true, .span_ms = 300000U},
        .long_window = {.valid = true, .span_ms = 3600000U}};
    run(NULL);
    assert_short_report();
    assert(strcmp(output, "Battery: 3.800 V\nCharge: 40%\nPower: battery\nCharging: no\n"
                          "Time left: 2h 5m estimated\n") == 0);
    run("status");
    assert(strstr(output, "Capacity: 2500 mAh"));
    assert(strstr(output, "Monitor: running") && strstr(output, "Last sample:"));
    assert(strstr(output, "Short window estimated current:") && strstr(output, "Long window estimated voltage:"));
    assert(strstr(output, "Estimated full charge runtime: 6h 0m\nTime left: 2h 5m estimated\n"));

    monitor.time_left_seeded = true;
    run(NULL); assert_short_report();
    assert(strstr(output, "Time left: 2h 5m estimated (configured runtime)"));
    monitor.running = false;
    run(NULL); assert_short_report(); assert(strstr(output, "Time left: unknown"));
    monitor.running = true; monitor.last_error = ESP_FAIL;
    run(NULL); assert_short_report(); assert(strstr(output, "Time left: unknown"));
    monitor.last_error = ESP_OK; monitor.sample_count = 0;
    run(NULL); assert_short_report(); assert(strstr(output, "Time left: unknown"));

    battery.charging_known = false; battery.external_power_known = false;
    run(NULL); assert(strstr(output, "Charging: no (estimated)"));
    battery.charging = true; battery.external_power = true;
    run(NULL); assert_short_report(); assert(strstr(output, "Charging: yes (estimated)"));
    battery.charging = false; battery.external_power = false;
    monitor.trend = SOLAR_OS_BATTERY_TREND_UNKNOWN;
    run(NULL); assert(strstr(output, "Power: unknown") && strstr(output, "Charging: unknown"));
    run("config");
    assert(strstr(output, "Full-charge runtime: 100 h 0 min") && !strstr(output, "Monitor:"));
    read_error = ESP_FAIL;
    run(NULL); assert(strcmp(output, "battery: read failed: error\n") == 0);
    solar_os_context_t ctx = 0;
    char *argv[] = {"battery", "status", "extra"};
    const unsigned before = reads;
    solar_os_shell_cmd_battery(&ctx, 3, argv);
    assert(unexpected == 1U && reads == before);
    puts("battery command: short/full reports, estimates, errors and argument validation passed");
    return 0;
}
