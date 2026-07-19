#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_adc.h"
#include "solar_os_adc_dpad.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_board_caps.h"
#include "solar_os_buses.h"
#include "solar_os_config.h"
#include "solar_os_gpio.h"
#include "solar_os_i2c.h"
#include "solar_os_joystick.h"
#include "solar_os_onewire.h"
#include "solar_os_pins.h"
#include "solar_os_pwm.h"
#include "solar_os_sensors.h"
#include "solar_os_status_led.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"
#include "solar_os_time.h"
#include "solar_os_uart.h"

#define SOLAR_OS_SHELL_ARG_MAX 20
#define I2C_READ_MAX_LEN 32
#define SPI_TRANSFER_MAX_LEN 64
#define UART_READ_MAX_LEN 96
#define UART_WRITE_MAX_LEN 128

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static bool shell_print_not_supported(solar_os_shell_io_t *term,
                                      const char *command,
                                      const char *feature,
                                      esp_err_t err)
{
    return solar_os_shell_print_not_supported(term, command, feature, err);
}

static bool parse_u8(const char *text, uint8_t *value)
{
    return solar_os_shell_parse_u8(text, value);
}

static bool parse_size_arg(const char *text, size_t min, size_t max, size_t *value)
{
    return solar_os_shell_parse_size_arg(text, min, max, value);
}

static bool parse_date_arg(const char *text, solar_os_datetime_t *datetime)
{
    unsigned year;
    unsigned month;
    unsigned day;
    int consumed = 0;

    if (text == NULL || datetime == NULL) {
        return false;
    }

    if (sscanf(text, "%u-%u-%u%n", &year, &month, &day, &consumed) != 3 ||
        text[consumed] != '\0' ||
        year > UINT16_MAX ||
        month > UINT8_MAX ||
        day > UINT8_MAX) {
        return false;
    }

    datetime->year = (uint16_t)year;
    datetime->month = (uint8_t)month;
    datetime->day = (uint8_t)day;
    return solar_os_time_datetime_is_valid(datetime);
}

static bool parse_time_arg(const char *text, solar_os_datetime_t *datetime)
{
    unsigned hour;
    unsigned minute;
    unsigned second = 0;
    int consumed = 0;

    if (text == NULL || datetime == NULL) {
        return false;
    }

    size_t colon_count = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ':') {
            colon_count++;
        }
    }

    if (colon_count == 1) {
        if (sscanf(text, "%u:%u%n", &hour, &minute, &consumed) != 2) {
            return false;
        }
    } else if (colon_count == 2) {
        if (sscanf(text, "%u:%u:%u%n", &hour, &minute, &second, &consumed) != 3) {
            return false;
        }
    } else {
        return false;
    }

    if (text[consumed] != '\0' ||
        hour > UINT8_MAX ||
        minute > UINT8_MAX ||
        second > UINT8_MAX) {
        return false;
    }

    datetime->hour = (uint8_t)hour;
    datetime->minute = (uint8_t)minute;
    datetime->second = (uint8_t)second;
    return solar_os_time_datetime_is_valid(datetime);
}

static void terminal_printf_fixed_1(solar_os_shell_io_t *term,
                                    const char *label,
                                    float value,
                                    const char *unit)
{
    int scaled = (int)((value * 10.0f) + (value >= 0.0f ? 0.5f : -0.5f));
    bool negative = scaled < 0;

    if (negative) {
        scaled = -scaled;
    }

    solar_os_shell_io_printf(term,
                             "%s: %s%d.%d %s\n",
                             label,
                             negative ? "-" : "",
                             scaled / 10,
                             scaled % 10,
                             unit);
}

static void format_bytes(uint64_t bytes, char *buffer, size_t buffer_len)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    size_t unit_index = 0;
    uint64_t scale = 1;

    while (unit_index + 1 < sizeof(units) / sizeof(units[0]) &&
           bytes >= scale * 1024ULL) {
        scale *= 1024ULL;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(buffer, buffer_len, "%" PRIu64 " %s", bytes, units[unit_index]);
        return;
    }

    const uint64_t tenths = ((bytes * 10ULL) + (scale / 2ULL)) / scale;
    snprintf(buffer,
             buffer_len,
             "%" PRIu64 ".%u %s",
             tenths / 10ULL,
             (unsigned)(tenths % 10ULL),
             units[unit_index]);
}

#if SOLAR_OS_PACKAGE_SERVICE_SD
static void sd_print_status(solar_os_shell_io_t *term)
{
    char status[64];
    solar_os_storage_sd_get_status(status, sizeof(status));
    solar_os_shell_io_printf(term, "SD: %s\n", status);
    solar_os_shell_io_printf(term, "Mount: %s\n", solar_os_storage_sd_mount_point());
}

static void sd_print_lsblk(solar_os_shell_io_t *term)
{
    const esp_err_t err = solar_os_storage_rescan();
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "sd", "SD storage", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "sd lsblk failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "NAME   SIZE     TYPE FS    MOUNT");
    const size_t count = solar_os_storage_block_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        char size[16];

        if (!solar_os_storage_get_block(i, &block)) {
            continue;
        }

        format_bytes(block.size_bytes, size, sizeof(size));
        solar_os_shell_io_printf(term,
                                 "%-6s %-8s %-4s %-5s %s\n",
                                 block.name,
                                 size,
                                 block.type == SOLAR_OS_STORAGE_BLOCK_DISK ? "disk" : "part",
                                 block.fs[0] != '\0' ? block.fs : "-",
                                 block.mounted ? block.mount_point : "-");
    }
}

static void sd_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  sd [status]");
    solar_os_shell_io_writeln(term, "  sd lsblk");
    solar_os_shell_io_writeln(term, "  sd mount [sd0pN] [mount]");
    solar_os_shell_io_writeln(term, "  sd umount [sd0pN|mount]");
}

void solar_os_shell_cmd_sd(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: sd [status]");
            return;
        }
        sd_print_status(term);
        return;
    }

    if (strcmp(argv[1], "lsblk") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: sd lsblk");
            return;
        }
        sd_print_lsblk(term);
        return;
    }

    if (strcmp(argv[1], "mount") == 0) {
        if (argc > 4) {
            solar_os_shell_io_writeln(term, "usage: sd mount [sd0pN] [mount]");
            return;
        }

        const char *volume = argc >= 3 ? argv[2] : NULL;
        const char *mount_point = argc >= 4 ? argv[3] : NULL;
        const esp_err_t err = volume == NULL ?
            solar_os_storage_mount() :
            solar_os_storage_mount_volume(volume, mount_point);
        if (err == ESP_OK) {
            sd_print_status(term);
        } else if (shell_print_not_supported(term, "sd", "SD storage", err)) {
            return;
        } else {
            solar_os_shell_io_printf(term, "sd mount failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "umount") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: sd umount [sd0pN|mount]");
            return;
        }

        const esp_err_t err = argc == 2 ?
            solar_os_storage_unmount() :
            solar_os_storage_unmount_volume(argv[2]);
        if (err == ESP_OK) {
            sd_print_status(term);
        } else if (shell_print_not_supported(term, "sd", "SD storage", err)) {
            return;
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "SD: not mounted");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "SD: not mounted: %s\n", argv[2]);
        } else {
            solar_os_shell_io_printf(term, "sd umount failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    sd_print_usage(term);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
static void battery_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  battery [status]");
    solar_os_shell_io_writeln(term, "  battery config");
    solar_os_shell_io_writeln(term, "  battery capacity [mAh]");
    solar_os_shell_io_writeln(term, "  battery min_voltage [V|mV]");
    solar_os_shell_io_writeln(term, "  battery max_voltage [V|mV]");
}

static void battery_format_voltage(uint16_t mv, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    snprintf(buffer, buffer_len, "%u.%03u V", (unsigned)(mv / 1000U), (unsigned)(mv % 1000U));
}

static void battery_format_minutes(uint32_t minutes, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    const uint32_t days = minutes / (24U * 60U);
    minutes %= 24U * 60U;
    const uint32_t hours = minutes / 60U;
    const uint32_t mins = minutes % 60U;

    if (days > 0) {
        snprintf(buffer, buffer_len, "%" PRIu32 "d %" PRIu32 "h", days, hours);
    } else if (hours > 0) {
        snprintf(buffer, buffer_len, "%" PRIu32 "h %" PRIu32 "m", hours, mins);
    } else {
        snprintf(buffer, buffer_len, "%" PRIu32 "m", mins);
    }
}

static bool battery_parse_voltage_mv(const char *text, uint16_t *voltage_mv)
{
    char buffer[24];

    if (text == NULL || text[0] == '\0' || voltage_mv == NULL) {
        return false;
    }
    if (strlcpy(buffer, text, sizeof(buffer)) >= sizeof(buffer)) {
        return false;
    }

    size_t len = strlen(buffer);
    bool explicit_mv = false;
    if (len >= 2 &&
        tolower((unsigned char)buffer[len - 2]) == 'm' &&
        tolower((unsigned char)buffer[len - 1]) == 'v') {
        explicit_mv = true;
        buffer[len - 2] = '\0';
        len -= 2;
    } else if (len >= 1 && tolower((unsigned char)buffer[len - 1]) == 'v') {
        buffer[len - 1] = '\0';
        len--;
    }
    if (len == 0) {
        return false;
    }

    uint32_t parsed_mv = 0;
    char *dot = strchr(buffer, '.');
    if (dot != NULL) {
        *dot = '\0';
        const char *frac = dot + 1;
        if (buffer[0] == '\0' || frac[0] == '\0') {
            return false;
        }

        char *end = NULL;
        errno = 0;
        const unsigned long whole = strtoul(buffer, &end, 10);
        if (errno != 0 || end == buffer || *end != '\0' || whole > 20UL) {
            return false;
        }

        uint32_t frac_mv = 0;
        size_t frac_digits = 0;
        while (frac[frac_digits] != '\0') {
            if (!isdigit((unsigned char)frac[frac_digits]) || frac_digits >= 3) {
                return false;
            }
            frac_mv = (frac_mv * 10U) + (uint32_t)(frac[frac_digits] - '0');
            frac_digits++;
        }
        while (frac_digits < 3) {
            frac_mv *= 10U;
            frac_digits++;
        }
        parsed_mv = (uint32_t)whole * 1000U + frac_mv;
    } else {
        char *end = NULL;
        errno = 0;
        const unsigned long parsed = strtoul(buffer, &end, 10);
        if (errno != 0 || end == buffer || *end != '\0') {
            return false;
        }

        parsed_mv = (uint32_t)parsed;
        if (!explicit_mv && parsed_mv <= 20U) {
            parsed_mv *= 1000U;
        }
    }

    if (parsed_mv > UINT16_MAX) {
        return false;
    }

    *voltage_mv = (uint16_t)parsed_mv;
    return true;
}

static void battery_print_config(solar_os_shell_io_t *term)
{
    solar_os_battery_config_t config;
    char min_voltage[16];
    char max_voltage[16];

    solar_os_battery_get_config(&config);
    battery_format_voltage(config.min_voltage_mv, min_voltage, sizeof(min_voltage));
    battery_format_voltage(config.max_voltage_mv, max_voltage, sizeof(max_voltage));

    if (config.capacity_mah == 0) {
        solar_os_shell_io_writeln(term, "Capacity: unset");
    } else {
        solar_os_shell_io_printf(term, "Capacity: %" PRIu32 " mAh\n", config.capacity_mah);
    }
    solar_os_shell_io_printf(term, "Min voltage: %s\n", min_voltage);
    solar_os_shell_io_printf(term, "Max voltage: %s\n", max_voltage);
}

static void battery_print_monitor_status(solar_os_shell_io_t *term)
{
    solar_os_battery_monitor_status_t monitor;
    solar_os_battery_monitor_get_status(&monitor);

    if (!monitor.running) {
        solar_os_shell_io_writeln(term, "Monitor: stopped");
        return;
    }

    solar_os_shell_io_printf(term,
                             "Monitor: running, interval %" PRIu32 " s, samples %" PRIu32 "\n",
                             monitor.interval_ms / 1000U,
                             monitor.sample_count);
    if (monitor.last_error != ESP_OK) {
        solar_os_shell_io_printf(term, "Monitor error: %s\n", esp_err_to_name(monitor.last_error));
        return;
    }
    if (monitor.sample_count == 0) {
        solar_os_shell_io_writeln(term, "Trend: waiting for first sample");
        return;
    }

    const uint32_t age_s =
        (uint32_t)((solar_os_time_uptime_ms() - monitor.last_sample_ms) / 1000ULL);
    solar_os_shell_io_printf(term,
                             "Last sample: %u.%03u V, %u%%, %" PRIu32 " s ago\n",
                             (unsigned)(monitor.last_voltage_mv / 1000U),
                             (unsigned)(monitor.last_voltage_mv % 1000U),
                             (unsigned)monitor.last_percent,
                             age_s);
    solar_os_shell_io_printf(term,
                             "Trend: %s, power %s, %" PRId32 " mV/hour\n",
                             solar_os_battery_trend_name(monitor.trend),
                             monitor.external_power ? "external" : "battery",
                             monitor.slope_mvh);
    if (monitor.time_left_valid) {
        char eta[24];
        battery_format_minutes(monitor.time_left_min, eta, sizeof(eta));
        solar_os_shell_io_printf(term, "Time left: %s estimated\n", eta);
    } else {
        solar_os_shell_io_writeln(term, "Time left: unknown");
    }
}

static void battery_print_status(solar_os_shell_io_t *term)
{
    solar_os_battery_status_t status;
    const esp_err_t err = solar_os_battery_get_status(&status);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "battery", "battery monitor", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "battery: read failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "Battery: %u.%03u V\n",
                             (unsigned)(status.voltage_mv / 1000U),
                             (unsigned)(status.voltage_mv % 1000U));
    solar_os_shell_io_printf(term,
                             "Charge: %u%% estimated%s\n",
                             (unsigned)status.percent,
                             status.adc_calibrated ? "" : " (uncalibrated ADC)");
    solar_os_shell_io_printf(term,
                             "Power: %s\n",
                             status.external_power ? "external" : "battery");
    battery_print_config(term);
    battery_print_monitor_status(term);
}

static void battery_print_config_result(solar_os_shell_io_t *term,
                                        const char *name,
                                        esp_err_t err)
{
    if (err == ESP_OK) {
        battery_print_config(term);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_printf(term, "%s: invalid value\n", name);
    } else {
        solar_os_shell_io_printf(term,
                                 "%s: applied but save failed: %s\n",
                                 name,
                                 esp_err_to_name(err));
    }
}

static void battery_cmd_capacity(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2) {
        solar_os_battery_config_t config;
        solar_os_battery_get_config(&config);
        if (config.capacity_mah == 0) {
            solar_os_shell_io_writeln(term, "Capacity: unset");
        } else {
            solar_os_shell_io_printf(term, "Capacity: %" PRIu32 " mAh\n", config.capacity_mah);
        }
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: battery capacity [mAh]");
        return;
    }

    size_t capacity = 0;
    if (!parse_size_arg(argv[2], 0, 100000, &capacity)) {
        solar_os_shell_io_writeln(term, "capacity: invalid value");
        return;
    }

    const esp_err_t err = solar_os_battery_set_capacity_mah((uint32_t)capacity);
    battery_print_config_result(term, "capacity", err);
}

static void battery_cmd_min_voltage(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2) {
        solar_os_battery_config_t config;
        char voltage[16];
        solar_os_battery_get_config(&config);
        battery_format_voltage(config.min_voltage_mv, voltage, sizeof(voltage));
        solar_os_shell_io_printf(term, "Min voltage: %s\n", voltage);
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: battery min_voltage [V|mV]");
        return;
    }

    uint16_t voltage_mv = 0;
    if (!battery_parse_voltage_mv(argv[2], &voltage_mv)) {
        solar_os_shell_io_writeln(term, "min_voltage: invalid value");
        return;
    }

    const esp_err_t err = solar_os_battery_set_min_voltage_mv(voltage_mv);
    battery_print_config_result(term, "min_voltage", err);
}

static void battery_cmd_max_voltage(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2) {
        solar_os_battery_config_t config;
        char voltage[16];
        solar_os_battery_get_config(&config);
        battery_format_voltage(config.max_voltage_mv, voltage, sizeof(voltage));
        solar_os_shell_io_printf(term, "Max voltage: %s\n", voltage);
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: battery max_voltage [V|mV]");
        return;
    }

    uint16_t voltage_mv = 0;
    if (!battery_parse_voltage_mv(argv[2], &voltage_mv)) {
        solar_os_shell_io_writeln(term, "max_voltage: invalid value");
        return;
    }

    const esp_err_t err = solar_os_battery_set_max_voltage_mv(voltage_mv);
    battery_print_config_result(term, "max_voltage", err);
}

void solar_os_shell_cmd_battery(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        battery_print_status(term);
        return;
    }

    if (strcmp(argv[1], "config") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: battery config");
            return;
        }
        battery_print_config(term);
        return;
    }

    if (strcmp(argv[1], "capacity") == 0) {
        battery_cmd_capacity(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "min_voltage") == 0) {
        battery_cmd_min_voltage(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "max_voltage") == 0) {
        battery_cmd_max_voltage(term, argc, argv);
        return;
    }

    battery_print_usage(term);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE
static void ble_format_bda(const uint8_t *bda, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (bda == NULL || buffer_len < 18) {
        buffer[0] = '\0';
        return;
    }

    snprintf(buffer,
             buffer_len,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0],
             bda[1],
             bda[2],
             bda[3],
             bda[4],
             bda[5]);
}

static int ble_hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool ble_parse_bda(const char *text, uint8_t bda[6])
{
    if (text == NULL || bda == NULL || strlen(text) != 17) {
        return false;
    }

    for (size_t i = 0; i < 6; i++) {
        const size_t pos = i * 3;
        const int high = ble_hex_nibble(text[pos]);
        const int low = ble_hex_nibble(text[pos + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        if (i < 5 && text[pos + 2] != ':') {
            return false;
        }
        bda[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool ble_parse_u16(const char *text, uint16_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool ble_parse_hex_token(const char *text,
                                uint8_t *buffer,
                                size_t buffer_len,
                                size_t *offset)
{
    if (text == NULL || buffer == NULL || offset == NULL) {
        return false;
    }

    size_t start = 0;
    size_t len = strlen(text);
    if (len >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        start = 2;
        len -= 2;
    }
    if (len == 0 || (len % 2) != 0) {
        return false;
    }

    for (size_t i = start; text[i] != '\0'; i += 2) {
        if (*offset >= buffer_len) {
            return false;
        }
        const int high = ble_hex_nibble(text[i]);
        const int low = ble_hex_nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        buffer[*offset] = (uint8_t)((high << 4) | low);
        (*offset)++;
    }
    return true;
}

static bool ble_parse_hex_args(int argc,
                               char **argv,
                               int first,
                               uint8_t *buffer,
                               size_t buffer_len,
                               size_t *value_len)
{
    if (buffer == NULL || value_len == NULL || first >= argc) {
        return false;
    }

    size_t offset = 0;
    for (int i = first; i < argc; i++) {
        if (!ble_parse_hex_token(argv[i], buffer, buffer_len, &offset)) {
            return false;
        }
    }
    *value_len = offset;
    return offset > 0;
}

static void ble_format_props(uint8_t properties, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    size_t len = 0;
    if ((properties & (1U << 1)) != 0 && len + 1 < buffer_len) {
        buffer[len++] = 'r';
    }
    if ((properties & (1U << 2)) != 0 && len + 1 < buffer_len) {
        buffer[len++] = 'w';
    }
    if ((properties & (1U << 3)) != 0 && len + 1 < buffer_len) {
        buffer[len++] = 'W';
    }
    if ((properties & (1U << 4)) != 0 && len + 1 < buffer_len) {
        buffer[len++] = 'n';
    }
    if ((properties & (1U << 5)) != 0 && len + 1 < buffer_len) {
        buffer[len++] = 'i';
    }
    if (len == 0 && len + 1 < buffer_len) {
        buffer[len++] = '-';
    }
    buffer[len] = '\0';
}

static void ble_print_hex_value(solar_os_shell_io_t *term, const uint8_t *value, size_t value_len)
{
    solar_os_shell_io_printf(term, "len: %u\n", (unsigned)value_len);
    solar_os_shell_io_write(term, "hex:");
    for (size_t i = 0; i < value_len; i++) {
        solar_os_shell_io_printf(term, " %02x", value[i]);
    }
    solar_os_shell_io_write(term, "\n");

    bool printable = value_len > 0;
    for (size_t i = 0; i < value_len; i++) {
        if (!isprint(value[i]) && value[i] != '\r' && value[i] != '\n' && value[i] != '\t') {
            printable = false;
            break;
        }
    }
    if (printable) {
        solar_os_shell_io_write(term, "text: ");
        for (size_t i = 0; i < value_len; i++) {
            solar_os_shell_io_put_char(term, (char)value[i]);
        }
        solar_os_shell_io_write(term, "\n");
    }
}

static void ble_set_scan_indicator(solar_os_shell_io_t *term, bool scanning)
{
    solar_os_terminal_t *display = solar_os_shell_io_terminal(term);
    if (display == NULL) {
        return;
    }

    solar_os_status_bar_t status;
    solar_os_terminal_get_status_bar(display, &status);
    status.ble_scanning = scanning;
    status.ble_connected = solar_os_ble_keyboard_is_connected();
    solar_os_terminal_set_status_bar(display, &status);
}

static void ble_cmd_scan(solar_os_shell_io_t *term)
{
    solar_os_ble_keyboard_scan_result_t results[SOLAR_OS_BLE_KEYBOARD_SCAN_MAX_RESULTS];
    size_t found = 0;

    solar_os_shell_io_writeln(term, "BLE scanning...");
    ble_set_scan_indicator(term, true);
    solar_os_shell_io_flush(term);

    const esp_err_t err = solar_os_ble_keyboard_scan(results,
                                                     sizeof(results) / sizeof(results[0]),
                                                     &found);
    ble_set_scan_indicator(term, false);
    if (err == ESP_ERR_NOT_FOUND || found == 0) {
        solar_os_shell_io_writeln(term, "no BLE devices found");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "BLE scan failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "RSSI Type       Address           Appr   Flags Name");
    for (size_t i = 0; i < found; i++) {
        char bda[18];
        char rssi[8];
        ble_format_bda(results[i].bda, bda, sizeof(bda));
        if (results[i].connected) {
            strlcpy(rssi, "conn", sizeof(rssi));
        } else {
            snprintf(rssi, sizeof(rssi), "%d", (int)results[i].rssi);
        }
        solar_os_shell_io_printf(term,
                                 "%4s %-10s %-17s 0x%04x %c%c%c%c  %s\n",
                                 rssi,
                                 solar_os_ble_keyboard_addr_type_name(results[i].addr_type),
                                 bda,
                                 results[i].appearance,
                                 results[i].connected ? 'c' : '-',
                                 results[i].hid_service ? 'h' : '-',
                                 results[i].keyboard_like ? 'k' : '-',
                                 results[i].remembered ? '*' : '-',
                                 results[i].name[0] ? results[i].name : "(unnamed)");
    }
    solar_os_shell_io_writeln(term, "flags: c=connected h=HID k=keyboard-like *=remembered");
}

static void ble_gatt_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  ble gatt status");
    solar_os_shell_io_writeln(term, "  ble gatt connect <aa:bb:cc:dd:ee:ff> <public|random|rpa_public|rpa_random>");
    solar_os_shell_io_writeln(term, "  ble gatt disconnect");
    solar_os_shell_io_writeln(term, "  ble gatt services");
    solar_os_shell_io_writeln(term, "  ble gatt chars <service-index>");
    solar_os_shell_io_writeln(term, "  ble gatt read <handle>");
    solar_os_shell_io_writeln(term, "  ble gatt write <handle> <hex...>");
    solar_os_shell_io_writeln(term, "  ble gatt write-nr <handle> <hex...>");
}

static void ble_gatt_print_status(solar_os_shell_io_t *term)
{
    solar_os_ble_gatt_status_t status;
    solar_os_ble_gatt_get_status(&status);

    if (!status.connected) {
        solar_os_shell_io_printf(term, "GATT: %s\n", status.status);
        return;
    }

    char bda[18];
    ble_format_bda(status.bda, bda, sizeof(bda));
    solar_os_shell_io_printf(term,
                             "GATT: %s %s %s conn=%u mtu=%u services=%u\n",
                             status.status,
                             bda,
                             solar_os_ble_keyboard_addr_type_name(status.addr_type),
                             (unsigned)status.conn_id,
                             (unsigned)status.mtu,
                             (unsigned)status.service_count);
}

static void ble_gatt_cmd_connect(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 5) {
        solar_os_shell_io_writeln(term,
                                  "usage: ble gatt connect <aa:bb:cc:dd:ee:ff> <public|random|rpa_public|rpa_random>");
        return;
    }

    uint8_t bda[6];
    uint8_t addr_type = 0;
    if (!ble_parse_bda(argv[3], bda)) {
        solar_os_shell_io_writeln(term, "ble gatt: invalid address");
        return;
    }
    if (!solar_os_ble_keyboard_parse_addr_type(argv[4], &addr_type)) {
        solar_os_shell_io_writeln(term, "ble gatt: invalid address type");
        return;
    }

    solar_os_shell_io_writeln(term, "BLE GATT connecting...");
    solar_os_shell_io_flush(term);
    const esp_err_t err = solar_os_ble_gatt_connect(bda, addr_type, 0);
    if (err == ESP_OK) {
        ble_gatt_print_status(term);
    } else if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ble gatt: already connected or unavailable");
    } else if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_writeln(term, "ble gatt: connect timeout");
    } else {
        solar_os_shell_io_printf(term, "ble gatt connect failed: %s\n", esp_err_to_name(err));
    }
}

static void ble_gatt_cmd_services(solar_os_shell_io_t *term)
{
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t count = 0;
    const esp_err_t err = solar_os_ble_gatt_services(services,
                                                     sizeof(services) / sizeof(services[0]),
                                                     &count);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ble gatt: not connected");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ble gatt services failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "#  Start End   P UUID");
    const size_t shown = count < SOLAR_OS_BLE_GATT_MAX_SERVICES ?
        count :
        SOLAR_OS_BLE_GATT_MAX_SERVICES;
    for (size_t i = 0; i < shown; i++) {
        solar_os_shell_io_printf(term,
                                 "%2u 0x%04x 0x%04x %c %s\n",
                                 (unsigned)i,
                                 (unsigned)services[i].start_handle,
                                 (unsigned)services[i].end_handle,
                                 services[i].primary ? 'p' : '-',
                                 services[i].uuid);
    }
    if (count > shown) {
        solar_os_shell_io_printf(term, "%u more services not shown\n", (unsigned)(count - shown));
    }
}

static void ble_gatt_cmd_chars(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 4) {
        solar_os_shell_io_writeln(term, "usage: ble gatt chars <service-index>");
        return;
    }

    char *end = NULL;
    const unsigned long service_index = strtoul(argv[3], &end, 0);
    if (end == argv[3] || *end != '\0') {
        solar_os_shell_io_writeln(term, "ble gatt: invalid service index");
        return;
    }

    solar_os_ble_gatt_characteristic_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t count = 0;
    const esp_err_t err = solar_os_ble_gatt_characteristics((size_t)service_index,
                                                           chars,
                                                           sizeof(chars) / sizeof(chars[0]),
                                                           &count);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ble gatt: not connected");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_writeln(term, "ble gatt: service index not found");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ble gatt chars failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "Handle Props UUID");
    const size_t shown = count < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ?
        count :
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS;
    for (size_t i = 0; i < shown; i++) {
        char props[8];
        ble_format_props(chars[i].properties, props, sizeof(props));
        solar_os_shell_io_printf(term,
                                 "0x%04x %-5s %s\n",
                                 (unsigned)chars[i].handle,
                                 props,
                                 chars[i].uuid);
    }
    if (count > shown) {
        solar_os_shell_io_printf(term, "%u more characteristics not shown\n", (unsigned)(count - shown));
    }
    solar_os_shell_io_writeln(term, "props: r=read w=write-nr W=write n=notify i=indicate");
}

static void ble_gatt_cmd_read(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 4) {
        solar_os_shell_io_writeln(term, "usage: ble gatt read <handle>");
        return;
    }

    uint16_t handle = 0;
    if (!ble_parse_u16(argv[3], &handle) || handle == 0) {
        solar_os_shell_io_writeln(term, "ble gatt: invalid handle");
        return;
    }

    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len = 0;
    const esp_err_t err = solar_os_ble_gatt_read(handle,
                                                 value,
                                                 sizeof(value),
                                                 &value_len,
                                                 0);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ble gatt: not connected");
        return;
    }
    if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_writeln(term, "ble gatt: read timeout");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ble gatt read failed: %s\n", esp_err_to_name(err));
        return;
    }

    ble_print_hex_value(term, value, value_len);
    if (value_len > sizeof(value)) {
        solar_os_shell_io_writeln(term, "value truncated");
    }
}

static void ble_gatt_cmd_write(solar_os_shell_io_t *term,
                               int argc,
                               char **argv,
                               bool with_response)
{
    if (argc < 5) {
        solar_os_shell_io_writeln(term,
                                  with_response ?
                                  "usage: ble gatt write <handle> <hex...>" :
                                  "usage: ble gatt write-nr <handle> <hex...>");
        return;
    }

    uint16_t handle = 0;
    if (!ble_parse_u16(argv[3], &handle) || handle == 0) {
        solar_os_shell_io_writeln(term, "ble gatt: invalid handle");
        return;
    }

    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len = 0;
    if (!ble_parse_hex_args(argc, argv, 4, value, sizeof(value), &value_len)) {
        solar_os_shell_io_writeln(term, "ble gatt: invalid hex payload");
        return;
    }

    const esp_err_t err = solar_os_ble_gatt_write(handle, value, value_len, with_response, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ble gatt: not connected");
        return;
    }
    if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_writeln(term, "ble gatt: write timeout");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ble gatt write failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term, "wrote %u byte%s\n", (unsigned)value_len, value_len == 1 ? "" : "s");
}

static void ble_cmd_gatt(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: ble gatt status");
            return;
        }
        ble_gatt_print_status(term);
        return;
    }

    if (strcmp(argv[2], "connect") == 0) {
        ble_gatt_cmd_connect(term, argc, argv);
        return;
    }

    if (strcmp(argv[2], "disconnect") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: ble gatt disconnect");
            return;
        }
        const esp_err_t err = solar_os_ble_gatt_disconnect();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "BLE GATT disconnected");
        } else {
            solar_os_shell_io_printf(term, "ble gatt disconnect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[2], "services") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: ble gatt services");
            return;
        }
        ble_gatt_cmd_services(term);
        return;
    }

    if (strcmp(argv[2], "chars") == 0) {
        ble_gatt_cmd_chars(term, argc, argv);
        return;
    }

    if (strcmp(argv[2], "read") == 0) {
        ble_gatt_cmd_read(term, argc, argv);
        return;
    }

    if (strcmp(argv[2], "write") == 0) {
        ble_gatt_cmd_write(term, argc, argv, true);
        return;
    }

    if (strcmp(argv[2], "write-nr") == 0) {
        ble_gatt_cmd_write(term, argc, argv, false);
        return;
    }

    ble_gatt_print_usage(term);
}

void solar_os_shell_cmd_ble(solar_os_context_t *ctx, int argc, char **argv)
{
    char ble_status[64];
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc <= 1 || strcmp(argv[1], "status") == 0) {
        solar_os_ble_keyboard_get_status(ble_status, sizeof(ble_status));
        solar_os_shell_io_printf(term,
                                 "BLE: %s, remembered %u/%u\n",
                                 ble_status,
                                 (unsigned)solar_os_ble_keyboard_remembered_count(),
                                 (unsigned)SOLAR_OS_BLE_KEYBOARD_MAX_REMEMBERED);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: ble scan");
            return;
        }
        ble_cmd_scan(term);
        return;
    }

    if (strcmp(argv[1], "pair") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: ble pair");
            return;
        }
        const esp_err_t err = solar_os_ble_keyboard_start_pairing();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "BLE pairing scan started");
        } else {
            solar_os_shell_io_printf(term, "BLE pairing failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "cancel") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: ble cancel");
            return;
        }
        const esp_err_t err = solar_os_ble_keyboard_cancel_pairing();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "BLE pairing cancelled");
        } else {
            solar_os_shell_io_printf(term, "BLE pairing cancel failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "forget") == 0) {
        const esp_err_t err = solar_os_ble_keyboard_forget();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "BLE keyboard forgotten");
        } else {
            solar_os_shell_io_printf(term, "BLE forget failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "gatt") == 0) {
        ble_cmd_gatt(term, argc, argv);
        return;
    }

    solar_os_shell_io_writeln(term, "usage: ble [status|scan|pair|cancel|forget|gatt]");
}
#endif


#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
static void audio_print_gain(solar_os_shell_io_t *term, float gain_db)
{
    int tenths = (int)((gain_db * 10.0f) + (gain_db >= 0.0f ? 0.5f : -0.5f));
    const char *sign = "";

    if (tenths < 0) {
        sign = "-";
        tenths = -tenths;
    }

    solar_os_shell_io_printf(term, "%s%d.%u dB", sign, tenths / 10, (unsigned)(tenths % 10));
}

static bool audio_parse_frequency(const char *text, uint32_t *frequency_hz)
{
    size_t value = 0;

    if (!parse_size_arg(text,
                        SOLAR_OS_AUDIO_TONE_MIN_HZ,
                        SOLAR_OS_AUDIO_TONE_MAX_HZ,
                        &value)) {
        return false;
    }

    *frequency_hz = (uint32_t)value;
    return true;
}

static bool audio_parse_duration(const char *text, uint32_t *duration_ms)
{
    size_t value = 0;

    if (!parse_size_arg(text, 1, SOLAR_OS_AUDIO_TEST_MAX_MS, &value)) {
        return false;
    }

    *duration_ms = (uint32_t)value;
    return true;
}

static void audio_print_status(solar_os_shell_io_t *term)
{
    solar_os_audio_status_t status;
    solar_os_audio_get_status(&status);

    solar_os_shell_io_printf(term, "Audio: %s\n", status.initialized ? "on" : "off");
    solar_os_shell_io_printf(term,
                             "Codec: out %s, in %s\n",
                             status.output_codec,
                             status.input_codec);
    solar_os_shell_io_printf(term,
                             "Format: %" PRIu32 " Hz, %u ch, %u bit\n",
                             status.sample_rate,
                             (unsigned)status.channels,
                             (unsigned)status.bits_per_sample);
    solar_os_shell_io_printf(term, "Volume: %u\n", (unsigned)status.volume);
    solar_os_shell_io_write(term, "Mic gain: ");
    audio_print_gain(term, status.mic_gain_db);
    solar_os_shell_io_put_char(term, '\n');
    if (status.i2s_port >= 0) {
        solar_os_shell_io_printf(term,
                                 "I2S: port %d mclk %d bclk %d ws %d din %d dout %d\n",
                                 status.i2s_port,
                                 status.mclk_pin,
                                 status.bclk_pin,
                                 status.ws_pin,
                                 status.din_pin,
                                 status.dout_pin);
        solar_os_shell_io_printf(term, "PA pin: %d\n", status.pa_pin);
    } else if (status.dout_pin >= 0 || status.din_pin >= 0) {
        solar_os_shell_io_printf(term,
                                 "DAC: pos %d neg %d\n",
                                 status.dout_pin,
                                 status.din_pin);
    }
}

static void audio_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  audio status");
    solar_os_shell_io_writeln(term, "  audio tone [hz] [ms] [volume]");
    solar_os_shell_io_writeln(term, "  audio level [volume]");
    solar_os_shell_io_writeln(term, "  audio mic [ms]");
    solar_os_shell_io_writeln(term, "  audio loopback [ms] [volume]");
    solar_os_shell_io_writeln(term, "  audio off");
}

static void audio_cmd_tone(solar_os_shell_io_t *term, int argc, char **argv)
{
    uint32_t frequency_hz = 880;
    uint32_t duration_ms = 500;
    uint8_t volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL;

    if (argc > 5 ||
        (argc >= 3 && !audio_parse_frequency(argv[2], &frequency_hz)) ||
        (argc >= 4 && !audio_parse_duration(argv[3], &duration_ms)) ||
        (argc >= 5 && !parse_u8(argv[4], &volume)) ||
        (argc >= 5 && volume > 100)) {
        solar_os_shell_io_writeln(term, "usage: audio tone [hz] [ms] [volume]");
        solar_os_shell_io_printf(term,
                                 "hz: %u..%u, ms: 1..%u, volume: 0..100\n",
                                 (unsigned)SOLAR_OS_AUDIO_TONE_MIN_HZ,
                                 (unsigned)SOLAR_OS_AUDIO_TONE_MAX_HZ,
                                 (unsigned)SOLAR_OS_AUDIO_TEST_MAX_MS);
        return;
    }

    if (volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL) {
        solar_os_shell_io_printf(term,
                                 "tone: %" PRIu32 " Hz %" PRIu32 " ms volume global\n",
                                 frequency_hz,
                                 duration_ms);
    } else {
        solar_os_shell_io_printf(term,
                                 "tone: %" PRIu32 " Hz %" PRIu32 " ms volume %u\n",
                                 frequency_hz,
                                 duration_ms,
                                 (unsigned)volume);
    }
    solar_os_shell_io_flush(term);

    const esp_err_t err = solar_os_audio_play_tone(frequency_hz, duration_ms, volume);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "audio", "audio hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "audio tone failed: %s\n", esp_err_to_name(err));
        return;
    }
    solar_os_shell_io_writeln(term, "audio tone: done");
}

static void audio_cmd_level(solar_os_shell_io_t *term, int argc, char **argv)
{
    solar_os_audio_status_t status;

    if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: audio level [volume]");
        solar_os_shell_io_writeln(term, "volume: 0..100");
        return;
    }

    if (argc == 2) {
        solar_os_audio_get_status(&status);
        solar_os_shell_io_printf(term, "speaker level: %u\n", (unsigned)status.volume);
        return;
    }

    uint8_t volume = 0;
    if (!parse_u8(argv[2], &volume) || volume > 100) {
        solar_os_shell_io_writeln(term, "usage: audio level [volume]");
        solar_os_shell_io_writeln(term, "volume: 0..100");
        return;
    }

    const esp_err_t err = solar_os_audio_set_volume(volume);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "audio", "audio hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "audio level failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term, "speaker level: %u\n", (unsigned)volume);
}

static void audio_cmd_mic(solar_os_shell_io_t *term, int argc, char **argv)
{
    uint32_t duration_ms = 1000;

    if (argc > 3 || (argc == 3 && !audio_parse_duration(argv[2], &duration_ms))) {
        solar_os_shell_io_writeln(term, "usage: audio mic [ms]");
        solar_os_shell_io_printf(term,
                                 "ms: 1..%u\n",
                                 (unsigned)SOLAR_OS_AUDIO_TEST_MAX_MS);
        return;
    }

    solar_os_shell_io_printf(term, "listening: %" PRIu32 " ms\n", duration_ms);
    solar_os_shell_io_flush(term);

    solar_os_audio_level_t level;
    const esp_err_t err = solar_os_audio_measure_level(duration_ms, &level);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "audio", "audio hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "audio mic failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "samples: %" PRIu32 ", peak: %u%%, avg: %u%%\n",
                             level.samples,
                             (unsigned)level.peak_percent,
                             (unsigned)level.average_percent);
}

static void audio_cmd_loopback(solar_os_shell_io_t *term, int argc, char **argv)
{
    uint32_t duration_ms = 3000;
    uint8_t volume = 40;

    if (argc > 4 ||
        (argc >= 3 && !audio_parse_duration(argv[2], &duration_ms)) ||
        (argc >= 4 && !parse_u8(argv[3], &volume)) ||
        volume > 100) {
        solar_os_shell_io_writeln(term, "usage: audio loopback [ms] [volume]");
        solar_os_shell_io_printf(term,
                                 "ms: 1..%u, volume: 0..100\n",
                                 (unsigned)SOLAR_OS_AUDIO_TEST_MAX_MS);
        return;
    }

    solar_os_shell_io_printf(term,
                             "loopback: %" PRIu32 " ms volume %u\n",
                             duration_ms,
                             (unsigned)volume);
    solar_os_shell_io_flush(term);

    const esp_err_t err = solar_os_audio_loopback(duration_ms, volume);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "audio", "audio hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "audio loopback failed: %s\n", esp_err_to_name(err));
        return;
    }
    solar_os_shell_io_writeln(term, "audio loopback: done");
}

void solar_os_shell_cmd_audio(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: audio status");
            return;
        }
        audio_print_status(term);
        return;
    }

    if (strcmp(argv[1], "tone") == 0) {
        audio_cmd_tone(term, argc, argv);
    } else if (strcmp(argv[1], "level") == 0) {
        audio_cmd_level(term, argc, argv);
    } else if (strcmp(argv[1], "mic") == 0) {
        audio_cmd_mic(term, argc, argv);
    } else if (strcmp(argv[1], "loopback") == 0) {
        audio_cmd_loopback(term, argc, argv);
    } else if (strcmp(argv[1], "off") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: audio off");
            return;
        }
        const esp_err_t err = solar_os_audio_set_volume(0);
        if (err != ESP_OK) {
            if (shell_print_not_supported(term, "audio", "audio hardware", err)) {
                return;
            }
            solar_os_shell_io_printf(term, "audio off failed: %s\n", esp_err_to_name(err));
            return;
        }
        solar_os_audio_deinit();
        solar_os_shell_io_writeln(term, "audio: off");
    } else {
        audio_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
static void uart_print_status(solar_os_shell_io_t *term)
{
    solar_os_uart_status_t status;
    solar_os_uart_get_status(&status);

    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_UART)) {
        solar_os_shell_io_writeln(term, "UART: not available on this board");
        return;
    }

    solar_os_shell_io_printf(term,
                             "UART: %s\n",
                             status.initialized ? "ready" : "unavailable");
    solar_os_shell_io_printf(term, "Port: UART%d\n", status.port_num);
    solar_os_shell_io_printf(term, "Pins: TX %d, RX %d\n", status.tx_pin, status.rx_pin);
    solar_os_shell_io_printf(term, "Baud: %" PRIu32 "\n", status.baud_rate);
    solar_os_shell_io_printf(term, "Mode: %s\n", solar_os_uart_mode_name(status.mode));
    if (status.rx_buffered_valid) {
        solar_os_shell_io_printf(term, "RX buffered: %u bytes\n", (unsigned)status.rx_buffered);
    } else {
        solar_os_shell_io_printf(term,
                                 "RX buffered: %s\n",
                                 status.initialized ? "busy" : "unavailable");
    }
    solar_os_shell_io_printf(term,
                             "Owner: %s\n",
                             status.port_claimed ? status.port_owner : "-");
}

static void uart_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  uart status");
    solar_os_shell_io_writeln(term, "  uart baud [rate]");
    solar_os_shell_io_writeln(term, "  uart mode [raw|line]");
    solar_os_shell_io_writeln(term, "  uart write <text>");
    solar_os_shell_io_writeln(term, "  uart read [ms]");
}

static void uart_print_apply_result(solar_os_shell_io_t *term,
                                    const char *setting,
                                    const char *value,
                                    esp_err_t err,
                                    bool applied)
{
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "%s: %s\n", setting, value);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_printf(term, "%s: invalid value: %s\n", setting, value);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_printf(term, "%s failed: UART not available on this board\n", setting);
    } else if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(term, "%s failed: port is busy\n", setting);
    } else if (applied) {
        solar_os_shell_io_printf(term,
                                 "%s: applied but save failed: %s\n",
                                 setting,
                                 esp_err_to_name(err));
    } else {
        solar_os_shell_io_printf(term,
                                 "%s failed: %s\n",
                                 setting,
                                 esp_err_to_name(err));
    }
}

static void uart_cmd_baud(solar_os_shell_io_t *term, int argc, char **argv)
{
    solar_os_uart_status_t status;

    if (argc == 2) {
        solar_os_uart_get_status(&status);
        solar_os_shell_io_printf(term, "baud: %" PRIu32 "\n", status.baud_rate);
        solar_os_shell_io_printf(term,
                                 "values: %u..%u\n",
                                 (unsigned)SOLAR_OS_UART_MIN_BAUD_RATE,
                                 (unsigned)SOLAR_OS_UART_MAX_BAUD_RATE);
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: uart baud [rate]");
        return;
    }

    size_t baud_rate = 0;
    if (!parse_size_arg(argv[2],
                        SOLAR_OS_UART_MIN_BAUD_RATE,
                        SOLAR_OS_UART_MAX_BAUD_RATE,
                        &baud_rate)) {
        solar_os_shell_io_printf(term,
                                 "baud values: %u..%u\n",
                                 (unsigned)SOLAR_OS_UART_MIN_BAUD_RATE,
                                 (unsigned)SOLAR_OS_UART_MAX_BAUD_RATE);
        return;
    }

    const esp_err_t err = solar_os_uart_set_baud_rate((uint32_t)baud_rate);
    solar_os_uart_get_status(&status);
    const bool applied = status.initialized && status.baud_rate == (uint32_t)baud_rate;
    uart_print_apply_result(term, "baud", argv[2], err, applied);
}

static void uart_cmd_mode(solar_os_shell_io_t *term, int argc, char **argv)
{
    solar_os_uart_status_t status;

    if (argc == 2) {
        solar_os_uart_get_status(&status);
        solar_os_shell_io_printf(term, "mode: %s\n", solar_os_uart_mode_name(status.mode));
        solar_os_shell_io_writeln(term, "values: raw line");
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: uart mode [raw|line]");
        return;
    }

    solar_os_uart_mode_t mode;
    if (!solar_os_uart_parse_mode(argv[2], &mode)) {
        solar_os_shell_io_writeln(term, "mode values: raw line");
        return;
    }

    const esp_err_t err = solar_os_uart_set_mode(mode);
    solar_os_uart_get_status(&status);
    const bool applied = status.initialized && status.mode == mode;
    uart_print_apply_result(term, "mode", argv[2], err, applied);
}

static bool uart_build_write_payload(int argc,
                                     char **argv,
                                     uint8_t *buffer,
                                     size_t buffer_len,
                                     size_t *payload_len)
{
    size_t len = 0;

    for (int i = 2; i < argc; i++) {
        const size_t arg_len = strlen(argv[i]);
        const size_t extra_space = i > 2 ? 1 : 0;
        if (len + extra_space + arg_len > buffer_len) {
            return false;
        }
        if (extra_space != 0) {
            buffer[len++] = ' ';
        }
        memcpy(&buffer[len], argv[i], arg_len);
        len += arg_len;
    }

    *payload_len = len;
    return true;
}

static void uart_cmd_write(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 3) {
        solar_os_shell_io_writeln(term, "usage: uart write <text>");
        return;
    }

    uint8_t buffer[UART_WRITE_MAX_LEN];
    size_t len = 0;
    if (!uart_build_write_payload(argc, argv, buffer, sizeof(buffer), &len)) {
        solar_os_shell_io_writeln(term, "uart write: text too long");
        return;
    }

    size_t written = 0;
    const esp_err_t err = solar_os_uart_write(buffer, len, &written);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "uart", "UART hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "uart write failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term, "uart write: %u bytes\n", (unsigned)written);
}

static void uart_print_read_data(solar_os_shell_io_t *term, const uint8_t *data, size_t len)
{
    for (size_t offset = 0; offset < len; offset += 16) {
        const size_t line_len = len - offset > 16 ? 16 : len - offset;
        solar_os_shell_io_printf(term, "%04x:", (unsigned)offset);

        for (size_t i = 0; i < 16; i++) {
            if (i < line_len) {
                solar_os_shell_io_printf(term, " %02x", data[offset + i]);
            } else {
                solar_os_shell_io_write(term, "   ");
            }
        }

        solar_os_shell_io_write(term, "  ");
        for (size_t i = 0; i < line_len; i++) {
            const unsigned char ch = data[offset + i];
            solar_os_shell_io_put_char(term, isprint(ch) ? (char)ch : '.');
        }
        solar_os_shell_io_put_char(term, '\n');
    }
}

static void uart_cmd_read(solar_os_shell_io_t *term, int argc, char **argv)
{
    size_t timeout_ms = 100;

    if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: uart read [ms]");
        return;
    }
    if (argc == 3 && !parse_size_arg(argv[2], 0, 10000, &timeout_ms)) {
        solar_os_shell_io_writeln(term, "read timeout: 0..10000 ms");
        return;
    }

    uint8_t buffer[UART_READ_MAX_LEN];
    size_t read_len = 0;
    const esp_err_t err = solar_os_uart_read(buffer, sizeof(buffer), (uint32_t)timeout_ms, &read_len);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "uart", "UART hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "uart read failed: %s\n", esp_err_to_name(err));
        return;
    }
    if (read_len == 0) {
        solar_os_shell_io_writeln(term, "uart read: no data");
        return;
    }

    solar_os_shell_io_printf(term, "uart read: %u bytes\n", (unsigned)read_len);
    uart_print_read_data(term, buffer, read_len);
}

void solar_os_shell_cmd_uart(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: uart status");
            return;
        }
        uart_print_status(term);
        return;
    }

    if (strcmp(argv[1], "baud") == 0) {
        uart_cmd_baud(term, argc, argv);
    } else if (strcmp(argv[1], "mode") == 0) {
        uart_cmd_mode(term, argc, argv);
    } else if (strcmp(argv[1], "write") == 0) {
        uart_cmd_write(term, argc, argv);
    } else if (strcmp(argv[1], "read") == 0) {
        uart_cmd_read(term, argc, argv);
    } else {
        uart_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO && SOLAR_OS_BOARD_HAS_STATUS_LED
static void led_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  led status");
    solar_os_shell_io_writeln(term, "  led on");
    solar_os_shell_io_writeln(term, "  led off");
    solar_os_shell_io_writeln(term, "  led toggle");
}

static void led_print_error(solar_os_shell_io_t *term, const char *action, esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "led: status LED not available on this board");
        return;
    }
    solar_os_shell_io_printf(term, "led %s failed: %s\n", action, esp_err_to_name(err));
}

static void led_print_status(solar_os_shell_io_t *term)
{
    bool on = false;
    const esp_err_t err = solar_os_status_led_get(&on);
    if (err != ESP_OK) {
        led_print_error(term, "status", err);
        return;
    }
    solar_os_shell_io_printf(term, "led: %s\n", on ? "on" : "off");
}

void solar_os_shell_cmd_led(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: led status");
            return;
        }
        led_print_status(term);
        return;
    }

    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        if (argc != 2) {
            solar_os_shell_io_printf(term, "usage: led %s\n", argv[1]);
            return;
        }
        const bool on = strcmp(argv[1], "on") == 0;
        const esp_err_t err = solar_os_status_led_set(on);
        if (err != ESP_OK) {
            led_print_error(term, argv[1], err);
            return;
        }
        solar_os_shell_io_printf(term, "led: %s\n", on ? "on" : "off");
        return;
    }

    if (strcmp(argv[1], "toggle") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: led toggle");
            return;
        }
        bool on = false;
        const esp_err_t err = solar_os_status_led_toggle(&on);
        if (err != ESP_OK) {
            led_print_error(term, "toggle", err);
            return;
        }
        solar_os_shell_io_printf(term, "led: %s\n", on ? "on" : "off");
        return;
    }

    led_print_usage(term);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GPIO
static void gpio_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  gpio list");
    solar_os_shell_io_writeln(term, "  gpio mode <pin> <in|out> [none|up|down]");
    solar_os_shell_io_writeln(term, "  gpio read <pin>");
    solar_os_shell_io_writeln(term, "  gpio write <pin> <0|1>");
}

static bool gpio_parse_pin(const char *text, int *pin)
{
    size_t parsed = 0;
    if (pin == NULL || !parse_size_arg(text, 0, 48, &parsed)) {
        return false;
    }

    *pin = (int)parsed;
    return true;
}

static void gpio_print_error(solar_os_shell_io_t *term, const char *action, int pin, esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "gpio: GPIO hardware not available on this board");
        return;
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        solar_os_shell_io_printf(term, "gpio %s: GPIO%d is reserved\n", action, pin);
        return;
    }

    solar_os_shell_io_printf(term,
                             "gpio %s GPIO%d failed: %s\n",
                             action,
                             pin,
                             esp_err_to_name(err));
}

static void gpio_print_pin_info(solar_os_shell_io_t *term, const solar_os_gpio_pin_info_t *info)
{
    solar_os_shell_io_printf(term,
                             "GPIO%-2d %-10s %-6s %-6s pull %-4s",
                             info->pin,
                             solar_os_pin_policy_name(info->policy),
                             info->configured ? solar_os_gpio_mode_name(info->mode) : "-",
                             info->level_valid ? (info->level ? "high" : "low") : "?",
                             info->configured ? solar_os_gpio_pull_name(info->pull) : "-");
    if (info->role != NULL && info->role[0] != '\0') {
        solar_os_shell_io_printf(term, " %s", info->role);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void gpio_cmd_list(solar_os_shell_io_t *term)
{
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_GPIO)) {
        solar_os_shell_io_writeln(term, "gpio: GPIO hardware not available on this board");
        return;
    }
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (solar_os_gpio_get_pin_info(i, &info)) {
            gpio_print_pin_info(term, &info);
        }
    }
}

static void gpio_cmd_mode(solar_os_shell_io_t *term, int argc, char **argv)
{
    int pin = -1;
    solar_os_gpio_mode_t mode;
    solar_os_gpio_pull_t pull = SOLAR_OS_GPIO_PULL_NONE;

    if (argc < 4 ||
        argc > 5 ||
        !gpio_parse_pin(argv[2], &pin) ||
        !solar_os_gpio_parse_mode(argv[3], &mode) ||
        (argc == 5 && !solar_os_gpio_parse_pull(argv[4], &pull))) {
        solar_os_shell_io_writeln(term, "usage: gpio mode <pin> <in|out> [none|up|down]");
        return;
    }

    const esp_err_t err = solar_os_gpio_configure(pin, mode, pull);
    if (err != ESP_OK) {
        gpio_print_error(term, "mode", pin, err);
        return;
    }

    solar_os_shell_io_printf(term,
                             "GPIO%d: %s pull %s\n",
                             pin,
                             solar_os_gpio_mode_name(mode),
                             solar_os_gpio_pull_name(pull));
}

static void gpio_cmd_read(solar_os_shell_io_t *term, int argc, char **argv)
{
    int pin = -1;
    bool level = false;

    if (argc != 3 || !gpio_parse_pin(argv[2], &pin)) {
        solar_os_shell_io_writeln(term, "usage: gpio read <pin>");
        return;
    }

    const esp_err_t err = solar_os_gpio_read(pin, &level);
    if (err != ESP_OK) {
        gpio_print_error(term, "read", pin, err);
        return;
    }

    solar_os_shell_io_printf(term, "GPIO%d: %u\n", pin, level ? 1U : 0U);
}

static void gpio_cmd_write(solar_os_shell_io_t *term, int argc, char **argv)
{
    int pin = -1;
    uint8_t level = 0;

    if (argc != 4 ||
        !gpio_parse_pin(argv[2], &pin) ||
        !parse_u8(argv[3], &level) ||
        level > 1) {
        solar_os_shell_io_writeln(term, "usage: gpio write <pin> <0|1>");
        return;
    }

    const esp_err_t err = solar_os_gpio_write(pin, level != 0);
    if (err != ESP_OK) {
        gpio_print_error(term, "write", pin, err);
        return;
    }

    solar_os_shell_io_printf(term, "GPIO%d <- %u\n", pin, (unsigned)level);
}

void solar_os_shell_cmd_gpio(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "list") == 0) {
        gpio_cmd_list(term);
        return;
    }

    if (strcmp(argv[1], "mode") == 0) {
        gpio_cmd_mode(term, argc, argv);
    } else if (strcmp(argv[1], "read") == 0) {
        gpio_cmd_read(term, argc, argv);
    } else if (strcmp(argv[1], "write") == 0) {
        gpio_cmd_write(term, argc, argv);
    } else {
        gpio_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
typedef struct {
    bool named;
    int pin;
    const char *name;
} onewire_target_t;

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
static void onewire_print_bus_status(solar_os_shell_io_t *term,
                                     const solar_os_bus_info_t *info)
{
    solar_os_shell_io_printf(term,
                             "%s: %s %s GPIO%d ready=%s leases=%u\n",
                             info->name,
                             solar_os_bus_origin_name(info->origin),
                             solar_os_bus_sharing_name(info->sharing),
                             info->config.onewire.pin,
                             info->ready ? "yes" : "no",
                             (unsigned)info->lease_count);
}
#endif

static void onewire_print_status(solar_os_shell_io_t *term, const char *name)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    if (name != NULL) {
        solar_os_bus_info_t info;
        if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, &info)) {
            solar_os_shell_io_printf(term, "onewire status: bus '%s' not found\n", name);
            return;
        }
        onewire_print_bus_status(term, &info);
        return;
    }

    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE);
    if (count == 0) {
        solar_os_shell_io_writeln(term,
                                  "1-Wire: no registered buses; direct GPIO targets are available");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE, i, &info)) {
            onewire_print_bus_status(term, &info);
        }
    }
#else
    (void)name;
    solar_os_shell_io_writeln(term,
                              "1-Wire: named buses unavailable; direct GPIO targets are available");
#endif
}

static void onewire_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  onewire [status [bus]]");
    solar_os_shell_io_writeln(term, "  onewire reset <bus|pin>");
    solar_os_shell_io_writeln(term, "  onewire scan <bus|pin>");
    solar_os_shell_io_writeln(term, "  onewire xfer <bus|pin> <read-len> [byte ...]");
}

static bool onewire_parse_pin(const char *text, int *pin)
{
    size_t parsed = 0;
    if (pin == NULL || !parse_size_arg(text, 0, 48, &parsed)) {
        return false;
    }
    *pin = (int)parsed;
    return true;
}

static bool onewire_parse_target(solar_os_shell_io_t *term,
                                 const char *text,
                                 onewire_target_t *target)
{
    if (text == NULL || target == NULL) {
        return false;
    }
    int pin = -1;
    if (onewire_parse_pin(text, &pin)) {
        *target = (onewire_target_t) {
            .pin = pin,
        };
        return true;
    }
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_bus_info_t info;
    if (solar_os_bus_find(text, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, &info)) {
        *target = (onewire_target_t) {
            .named = true,
            .pin = info.config.onewire.pin,
            .name = text,
        };
        return true;
    }
#endif
    solar_os_shell_io_printf(term, "onewire: bus '%s' not found\n", text);
    return false;
}

static void onewire_target_label(const onewire_target_t *target,
                                 char *label,
                                 size_t label_size)
{
    if (target->named) {
        strlcpy(label, target->name, label_size);
    } else {
        (void)snprintf(label, label_size, "GPIO%d", target->pin);
    }
}

static void onewire_print_error(solar_os_shell_io_t *term,
                                const char *action,
                                const onewire_target_t *target,
                                esp_err_t err)
{
    char label[SOLAR_OS_BUS_NAME_MAX + 8];
    onewire_target_label(target, label, sizeof(label));
    if (err == ESP_ERR_NOT_ALLOWED) {
        solar_os_shell_io_printf(term, "onewire %s: %s is reserved\n", action, label);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "onewire %s: no device on %s\n", action, label);
    } else if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(term, "onewire %s: bus '%s' is busy\n", action, label);
    } else {
        solar_os_shell_io_printf(term,
                                 "onewire %s on %s failed: %s\n",
                                 action,
                                 label,
                                 esp_err_to_name(err));
    }
}

static void onewire_cmd_reset(solar_os_shell_io_t *term, int argc, char **argv)
{
    onewire_target_t target;
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: onewire reset <bus|pin>");
        return;
    }
    if (!onewire_parse_target(term, argv[2], &target)) {
        return;
    }

    bool present = false;
    const esp_err_t err = target.named
        ? solar_os_onewire_bus_reset(target.name, &present)
        : solar_os_onewire_reset(target.pin, &present);
    if (err != ESP_OK) {
        onewire_print_error(term, "reset", &target, err);
        return;
    }
    char label[SOLAR_OS_BUS_NAME_MAX + 8];
    onewire_target_label(&target, label, sizeof(label));
    solar_os_shell_io_printf(term, "%s: %s\n", label, present ? "presence detected" : "no presence");
}

static void onewire_cmd_scan(solar_os_shell_io_t *term, int argc, char **argv)
{
    onewire_target_t target;
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: onewire scan <bus|pin>");
        return;
    }
    if (!onewire_parse_target(term, argv[2], &target)) {
        return;
    }

    uint64_t addresses[SOLAR_OS_ONEWIRE_MAX_DEVICES];
    size_t count = 0;
    const esp_err_t err = target.named
        ? solar_os_onewire_bus_scan(target.name,
                                    addresses,
                                    SOLAR_OS_ONEWIRE_MAX_DEVICES,
                                    &count)
        : solar_os_onewire_scan(target.pin,
                                addresses,
                                SOLAR_OS_ONEWIRE_MAX_DEVICES,
                                &count);
    if (err != ESP_OK) {
        onewire_print_error(term, "scan", &target, err);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        solar_os_shell_io_printf(term,
                                 "%016" PRIx64 " family 0x%02x\n",
                                 addresses[i],
                                 (unsigned)(addresses[i] & 0xffU));
    }
    char label[SOLAR_OS_BUS_NAME_MAX + 8];
    onewire_target_label(&target, label, sizeof(label));
    solar_os_shell_io_printf(term,
                             "%u device%s found on %s\n",
                             (unsigned)count,
                             count == 1 ? "" : "s",
                             label);
}

static void onewire_cmd_xfer(solar_os_shell_io_t *term, int argc, char **argv)
{
    onewire_target_t target;
    size_t read_len = 0;
    const size_t write_len = argc >= 4 ? (size_t)(argc - 4) : 0;
    uint8_t tx_data[SOLAR_OS_ONEWIRE_MAX_TRANSFER];
    uint8_t rx_data[SOLAR_OS_ONEWIRE_MAX_TRANSFER];

    if (argc < 4 ||
        !parse_size_arg(argv[3], 0, SOLAR_OS_ONEWIRE_MAX_TRANSFER, &read_len) ||
        write_len > SOLAR_OS_ONEWIRE_MAX_TRANSFER ||
        (read_len == 0 && write_len == 0)) {
        solar_os_shell_io_writeln(term,
                                  "usage: onewire xfer <bus|pin> <read-len> [byte ...]");
        return;
    }
    if (!onewire_parse_target(term, argv[2], &target)) {
        return;
    }
    for (size_t i = 0; i < write_len; i++) {
        if (!parse_u8(argv[i + 4], &tx_data[i])) {
            solar_os_shell_io_printf(term, "onewire xfer: invalid byte '%s'\n", argv[i + 4]);
            return;
        }
    }

    const esp_err_t err = target.named
        ? solar_os_onewire_bus_transfer(target.name,
                                        tx_data,
                                        write_len,
                                        rx_data,
                                        read_len)
        : solar_os_onewire_transfer(target.pin,
                                    tx_data,
                                    write_len,
                                    rx_data,
                                    read_len);
    if (err != ESP_OK) {
        onewire_print_error(term, "xfer", &target, err);
        return;
    }

    char label[SOLAR_OS_BUS_NAME_MAX + 8];
    onewire_target_label(&target, label, sizeof(label));
    if (read_len == 0) {
        solar_os_shell_io_printf(term, "wrote %u byte%s on %s\n",
                                 (unsigned)write_len,
                                 write_len == 1 ? "" : "s",
                                 label);
        return;
    }
    solar_os_shell_io_write(term, "rx:");
    for (size_t i = 0; i < read_len; i++) {
        solar_os_shell_io_printf(term, " %02x", rx_data[i]);
    }
    solar_os_shell_io_put_char(term, '\n');
}

void solar_os_shell_cmd_onewire(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 3) {
            onewire_print_usage(term);
            return;
        }
        onewire_print_status(term, argc == 3 ? argv[2] : NULL);
    } else if (strcmp(argv[1], "reset") == 0) {
        onewire_cmd_reset(term, argc, argv);
    } else if (strcmp(argv[1], "scan") == 0) {
        onewire_cmd_scan(term, argc, argv);
    } else if (strcmp(argv[1], "xfer") == 0) {
        onewire_cmd_xfer(term, argc, argv);
    } else {
        onewire_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
static const char *dpad_zone_name(solar_os_adc_dpad_zone_t zone)
{
    switch (zone) {
    case SOLAR_OS_ADC_DPAD_ZONE_IDLE:
        return "idle";
    case SOLAR_OS_ADC_DPAD_ZONE_MID:
        return "mid";
    case SOLAR_OS_ADC_DPAD_ZONE_HIGH:
        return "high";
    default:
        return "?";
    }
}

static void dpad_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  dpad [status]");
    solar_os_shell_io_writeln(term, "  dpad calibrate [idle]");
    solar_os_shell_io_writeln(term, "  dpad calibrate reset");
}

static void dpad_print_status(solar_os_shell_io_t *term)
{
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_ADC_DPAD)) {
        solar_os_shell_io_writeln(term, "dpad: not available on this board");
        return;
    }

    const size_t count = solar_os_adc_dpad_axis_count();
    solar_os_shell_io_printf(term, "dpad: %u ADC %s\n", (unsigned)count, count == 1 ? "axis" : "axes");
    solar_os_shell_io_writeln(term, "AXIS PIN RAW  ZONE  IDLE<= MID       HIGH>=");
    for (size_t i = 0; i < count; i++) {
        solar_os_adc_dpad_axis_status_t status;
        if (!solar_os_adc_dpad_get_axis_status(i, &status)) {
            continue;
        }

        if (!status.initialized) {
            solar_os_shell_io_printf(term,
                                     "%-4s %-3d -    -     -      -         -\n",
                                     status.name != NULL ? status.name : "?",
                                     (int)status.pin);
            continue;
        }

        if (status.read_error != ESP_OK && !status.raw_valid) {
            solar_os_shell_io_printf(term,
                                     "%-4s %-3d err  %-5s %-6u %4u-%-4u %-6u %s\n",
                                     status.name != NULL ? status.name : "?",
                                     (int)status.pin,
                                     dpad_zone_name(status.zone),
                                     (unsigned)status.idle_max,
                                     (unsigned)status.mid_min,
                                     (unsigned)status.mid_max,
                                     (unsigned)status.high_min,
                                     esp_err_to_name(status.read_error));
            continue;
        }

        solar_os_shell_io_printf(term,
                                 "%-4s %-3d %-4d %-5s %-6u %4u-%-4u %-6u\n",
                                 status.name != NULL ? status.name : "?",
                                 (int)status.pin,
                                 status.raw,
                                 dpad_zone_name(status.zone),
                                 (unsigned)status.idle_max,
                                 (unsigned)status.mid_min,
                                 (unsigned)status.mid_max,
                                 (unsigned)status.high_min);
    }
}

void solar_os_shell_cmd_dpad(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: dpad status");
            return;
        }
        dpad_print_status(term);
        return;
    }

    if (strcmp(argv[1], "calibrate") == 0) {
        if (argc > 3) {
            dpad_print_usage(term);
            return;
        }

        esp_err_t err;
        if (argc == 3 && strcmp(argv[2], "reset") == 0) {
            err = solar_os_adc_dpad_calibrate_reset();
        } else if (argc == 2 || (argc == 3 && strcmp(argv[2], "idle") == 0)) {
            err = solar_os_adc_dpad_calibrate_idle();
        } else {
            dpad_print_usage(term);
            return;
        }

        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term,
                                      argc == 3 && strcmp(argv[2], "reset") == 0 ?
                                          "dpad calibration reset" :
                                          "dpad idle calibrated");
            dpad_print_status(term);
        } else if (shell_print_not_supported(term, "dpad", "ADC D-pad", err)) {
            return;
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "dpad: not initialized");
        } else {
            solar_os_shell_io_printf(term, "dpad calibrate failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    dpad_print_usage(term);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_JOYSTICK
static const char *joystick_direction_name(int direction)
{
    if (direction < 0) {
        return "low";
    }
    if (direction > 0) {
        return "high";
    }
    return "center";
}

static void joystick_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  joystick [status]");
    solar_os_shell_io_writeln(term, "  joystick calibrate");
    solar_os_shell_io_writeln(term, "  joystick calibrate reset");
}

static void joystick_print_status(solar_os_shell_io_t *term)
{
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_JOYSTICK)) {
        solar_os_shell_io_writeln(term, "joystick: not available on this board");
        return;
    }

    const size_t count = solar_os_joystick_axis_count();
    solar_os_shell_io_printf(term, "joystick: %u %s\n", (unsigned)count, count == 1 ? "axis" : "axes");
    solar_os_shell_io_writeln(term, "AXIS PIN RAW  DIR    LOW-P LOW-R HIGH-R HIGH-P");
    for (size_t i = 0; i < count; i++) {
        solar_os_joystick_axis_status_t status;
        if (!solar_os_joystick_get_axis_status(i, &status)) {
            continue;
        }

        if (!status.initialized) {
            solar_os_shell_io_printf(term,
                                     "%-4s %-3d -    -      -     -     -      -\n",
                                     status.name != NULL ? status.name : "?",
                                     (int)status.pin);
            continue;
        }

        if (status.read_error != ESP_OK && !status.raw_valid) {
            solar_os_shell_io_printf(term,
                                     "%-4s %-3d err  %-6s %-5u %-5u %-6u %-6u %s\n",
                                     status.name != NULL ? status.name : "?",
                                     (int)status.pin,
                                     joystick_direction_name(status.direction),
                                     (unsigned)status.low_press,
                                     (unsigned)status.low_release,
                                     (unsigned)status.high_release,
                                     (unsigned)status.high_press,
                                     esp_err_to_name(status.read_error));
            continue;
        }

        solar_os_shell_io_printf(term,
                                 "%-4s %-3d %-4d %-6s %-5u %-5u %-6u %-6u\n",
                                 status.name != NULL ? status.name : "?",
                                 (int)status.pin,
                                 status.raw,
                                 joystick_direction_name(status.direction),
                                 (unsigned)status.low_press,
                                 (unsigned)status.low_release,
                                 (unsigned)status.high_release,
                                 (unsigned)status.high_press);
    }
}

void solar_os_shell_cmd_joystick(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: joystick status");
            return;
        }
        joystick_print_status(term);
        return;
    }

    if (strcmp(argv[1], "calibrate") == 0) {
        if (argc > 3) {
            joystick_print_usage(term);
            return;
        }

        esp_err_t err;
        if (argc == 3 && strcmp(argv[2], "reset") == 0) {
            err = solar_os_joystick_calibrate_reset();
        } else if (argc == 2) {
            err = solar_os_joystick_calibrate_center();
        } else {
            joystick_print_usage(term);
            return;
        }

        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, argc == 3 ? "joystick calibration reset" : "joystick center calibrated");
            joystick_print_status(term);
        } else if (shell_print_not_supported(term, "joystick", "joystick", err)) {
            return;
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "joystick: not initialized");
        } else {
            solar_os_shell_io_printf(term, "joystick calibrate failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    joystick_print_usage(term);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
static void adc_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  adc status");
    solar_os_shell_io_writeln(term, "  adc read <pin>");
}

static void adc_print_error(solar_os_shell_io_t *term, const char *action, int pin, esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "adc: ADC hardware not available on this board");
        return;
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        solar_os_shell_io_printf(term, "adc %s: GPIO%d is reserved\n", action, pin);
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "adc %s: GPIO%d is not ADC capable\n", action, pin);
        return;
    }

    solar_os_shell_io_printf(term,
                             "adc %s GPIO%d failed: %s\n",
                             action,
                             pin,
                             esp_err_to_name(err));
}

static void adc_print_pin_info(solar_os_shell_io_t *term, const solar_os_adc_pin_info_t *info)
{
    if (info->adc_capable) {
        solar_os_shell_io_printf(term,
                                 "GPIO%-2d ADC%d ch%d\n",
                                 info->pin,
                                 info->unit,
                                 info->channel);
    } else {
        solar_os_shell_io_printf(term, "GPIO%-2d digital-only\n", info->pin);
    }
}

static void adc_cmd_status(solar_os_shell_io_t *term)
{
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_ADC)) {
        solar_os_shell_io_writeln(term, "adc: ADC hardware not available on this board");
        return;
    }
    for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
        solar_os_adc_pin_info_t info;
        if (solar_os_adc_get_pin_info(i, &info)) {
            adc_print_pin_info(term, &info);
        }
    }
}

static void adc_cmd_read(solar_os_shell_io_t *term, int argc, char **argv)
{
    int pin = -1;
    solar_os_adc_sample_t sample;

    if (argc != 3 || !gpio_parse_pin(argv[2], &pin)) {
        solar_os_shell_io_writeln(term, "usage: adc read <pin>");
        return;
    }

    const esp_err_t err = solar_os_adc_read(pin, &sample);
    if (err != ESP_OK) {
        adc_print_error(term, "read", pin, err);
        return;
    }

    if (sample.calibrated) {
        solar_os_shell_io_printf(term,
                                 "GPIO%d: raw %d, %u mV (ADC%d ch%d)\n",
                                 sample.pin,
                                 sample.raw,
                                 (unsigned)sample.voltage_mv,
                                 sample.unit,
                                 sample.channel);
    } else {
        solar_os_shell_io_printf(term,
                                 "GPIO%d: raw %d, uncalibrated (ADC%d ch%d)\n",
                                 sample.pin,
                                 sample.raw,
                                 sample.unit,
                                 sample.channel);
    }
}

void solar_os_shell_cmd_adc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: adc status");
            return;
        }
        adc_cmd_status(term);
        return;
    }

    if (strcmp(argv[1], "read") == 0) {
        adc_cmd_read(term, argc, argv);
    } else {
        adc_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PWM
static void pwm_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  pwm status");
    solar_os_shell_io_writeln(term, "  pwm set <pin> <freq-hz> <duty-percent>");
    solar_os_shell_io_writeln(term, "  pwm off <pin>");
}

static void pwm_print_error(solar_os_shell_io_t *term, const char *action, int pin, esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "pwm: PWM hardware not available on this board");
        return;
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        solar_os_shell_io_printf(term, "pwm %s: GPIO%d is reserved\n", action, pin);
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "pwm %s: GPIO%d is not active\n", action, pin);
        return;
    }

    solar_os_shell_io_printf(term,
                             "pwm %s GPIO%d failed: %s\n",
                             action,
                             pin,
                             esp_err_to_name(err));
}

static void pwm_print_pin_info(solar_os_shell_io_t *term, const solar_os_pwm_pin_info_t *info)
{
    if (info->active) {
        solar_os_shell_io_printf(term,
                                 "GPIO%-2d ch%d %" PRIu32 " Hz duty %u%%\n",
                                 info->pin,
                                 info->channel,
                                 info->freq_hz,
                                 (unsigned)info->duty_percent);
    } else {
        solar_os_shell_io_printf(term, "GPIO%-2d off\n", info->pin);
    }
}

static void pwm_cmd_status(solar_os_shell_io_t *term)
{
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_PWM)) {
        solar_os_shell_io_writeln(term, "pwm: PWM hardware not available on this board");
        return;
    }
    for (size_t i = 0; i < solar_os_pwm_pin_count(); i++) {
        solar_os_pwm_pin_info_t info;
        if (solar_os_pwm_get_pin_info(i, &info)) {
            pwm_print_pin_info(term, &info);
        }
    }
}

static void pwm_cmd_set(solar_os_shell_io_t *term, int argc, char **argv)
{
    int pin = -1;
    size_t freq_hz = 0;
    size_t duty_percent = 0;

    if (argc != 5 ||
        !gpio_parse_pin(argv[2], &pin) ||
        !parse_size_arg(argv[3], SOLAR_OS_PWM_FREQ_MIN_HZ, SOLAR_OS_PWM_FREQ_MAX_HZ, &freq_hz) ||
        !parse_size_arg(argv[4], 0, SOLAR_OS_PWM_DUTY_MAX_PERCENT, &duty_percent)) {
        solar_os_shell_io_writeln(term, "usage: pwm set <pin> <freq-hz> <duty-percent>");
        solar_os_shell_io_printf(term,
                                 "freq: %u..%u Hz, duty: 0..100\n",
                                 (unsigned)SOLAR_OS_PWM_FREQ_MIN_HZ,
                                 (unsigned)SOLAR_OS_PWM_FREQ_MAX_HZ);
        return;
    }

    const esp_err_t err = solar_os_pwm_set(pin, (uint32_t)freq_hz, (uint8_t)duty_percent);
    if (err != ESP_OK) {
        pwm_print_error(term, "set", pin, err);
        return;
    }

    solar_os_shell_io_printf(term,
                             "GPIO%d PWM %" PRIu32 " Hz duty %u%%\n",
                             pin,
                             (uint32_t)freq_hz,
                             (unsigned)duty_percent);
}

static void pwm_cmd_off(solar_os_shell_io_t *term, int argc, char **argv)
{
    int pin = -1;

    if (argc != 3 || !gpio_parse_pin(argv[2], &pin)) {
        solar_os_shell_io_writeln(term, "usage: pwm off <pin>");
        return;
    }

    const esp_err_t err = solar_os_pwm_stop(pin);
    if (err != ESP_OK) {
        pwm_print_error(term, "off", pin, err);
        return;
    }

    solar_os_shell_io_printf(term, "GPIO%d PWM off\n", pin);
}

void solar_os_shell_cmd_pwm(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: pwm status");
            return;
        }
        pwm_cmd_status(term);
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        pwm_cmd_set(term, argc, argv);
    } else if (strcmp(argv[1], "off") == 0) {
        pwm_cmd_off(term, argc, argv);
    } else {
        pwm_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
static void i2c_print_bus_status(solar_os_shell_io_t *term,
                                 const solar_os_bus_info_t *info)
{
    solar_os_shell_io_printf(term,
                             "%s: %s %s port=%d SDA=%d SCL=%d speed=%" PRIu32
                             " ready=%s leases=%u\n",
                             info->name,
                             solar_os_bus_origin_name(info->origin),
                             solar_os_bus_sharing_name(info->sharing),
                             info->config.i2c.port,
                             info->config.i2c.sda_pin,
                             info->config.i2c.scl_pin,
                             info->config.i2c.speed_hz,
                             info->ready ? "yes" : "no",
                             (unsigned)info->lease_count);
}
#endif

static void i2c_print_status(solar_os_shell_io_t *term, const char *name)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    if (name != NULL) {
        solar_os_bus_info_t info;
        if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, &info)) {
            solar_os_shell_io_printf(term, "i2c status: bus '%s' not found\n", name);
            return;
        }
        i2c_print_bus_status(term, &info);
        return;
    }

    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C);
    if (count == 0) {
        solar_os_shell_io_writeln(term, "I2C: no registered buses");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_I2C, i, &info)) {
            i2c_print_bus_status(term, &info);
        }
    }
#else
    if (name != NULL && strcmp(name, SOLAR_OS_I2C_DEFAULT_BUS) != 0) {
        solar_os_shell_io_printf(term, "i2c status: bus '%s' not found\n", name);
        return;
    }
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        solar_os_shell_io_writeln(term, "I2C: not available on this board");
        return;
    }
    solar_os_shell_io_printf(term,
                             "I2C: SDA %d, SCL %d, %" PRIu32 " Hz\n",
                             solar_os_i2c_get_sda_pin(),
                             solar_os_i2c_get_scl_pin(),
                             solar_os_i2c_get_speed_hz());
#endif
}

static bool i2c_bus_exists(solar_os_shell_io_t *term, const char *name)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, NULL)) {
        solar_os_shell_io_printf(term, "i2c: bus '%s' not found\n", name);
        return false;
    }
    return true;
#else
    if (strcmp(name, SOLAR_OS_I2C_DEFAULT_BUS) == 0) {
        return true;
    }
    solar_os_shell_io_printf(term, "i2c: bus '%s' not found\n", name);
    return false;
#endif
}

static void i2c_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  i2c [status [bus]]");
    solar_os_shell_io_writeln(term, "  i2c scan [bus]");
    solar_os_shell_io_writeln(term, "  i2c probe [bus] <addr>");
    solar_os_shell_io_writeln(term, "  i2c read [bus] <addr> <reg> [len]");
    solar_os_shell_io_writeln(term, "  i2c write [bus] <addr> <reg> <byte...>");
}

static void i2c_cmd_scan(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: i2c scan [bus]");
        return;
    }
    const char *bus = argc == 3 ? argv[2] : SOLAR_OS_I2C_DEFAULT_BUS;
    if (!i2c_bus_exists(term, bus)) {
        return;
    }
    size_t found = 0;

    solar_os_shell_io_writeln(term, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
    for (uint8_t row = 0; row < 0x80; row += 0x10) {
        solar_os_shell_io_printf(term, "%02x: ", row);
        for (uint8_t col = 0; col < 0x10; col++) {
            const uint8_t address = row + col;
            if (address < SOLAR_OS_I2C_SCAN_MIN_ADDR || address > SOLAR_OS_I2C_SCAN_MAX_ADDR) {
                solar_os_shell_io_write(term, "   ");
                continue;
            }

            const esp_err_t err = solar_os_i2c_bus_probe(bus, address);
            if (err == ESP_OK) {
                solar_os_shell_io_printf(term, "%02x ", address);
                found++;
            } else if (err == ESP_ERR_TIMEOUT) {
                solar_os_shell_io_write(term, "UU ");
            } else {
                solar_os_shell_io_write(term, "-- ");
            }
        }
        solar_os_shell_io_put_char(term, '\n');
    }

    solar_os_shell_io_printf(term, "%u device%s found\n", (unsigned)found, found == 1 ? "" : "s");
}

static void i2c_cmd_probe(solar_os_shell_io_t *term, int argc, char **argv)
{
    const char *bus = SOLAR_OS_I2C_DEFAULT_BUS;
    int address_arg = 2;
    if (argc == 4) {
        bus = argv[2];
        address_arg = 3;
    }
    uint8_t address;
    if ((argc != 3 && argc != 4) || !parse_u8(argv[address_arg], &address) ||
        address < SOLAR_OS_I2C_SCAN_MIN_ADDR || address > SOLAR_OS_I2C_SCAN_MAX_ADDR) {
        solar_os_shell_io_writeln(term, "usage: i2c probe [bus] <addr>");
        return;
    }
    if (!i2c_bus_exists(term, bus)) {
        return;
    }

    const esp_err_t err = solar_os_i2c_bus_probe(bus, address);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "%s 0x%02x: ACK\n", bus, address);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "i2c: I2C hardware not available on this board");
    } else if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_printf(term, "%s 0x%02x: bus busy\n", bus, address);
    } else {
        solar_os_shell_io_printf(term,
                                 "%s 0x%02x: no response (%s)\n",
                                 bus,
                                 address,
                                 esp_err_to_name(err));
    }
}

static void i2c_cmd_read(solar_os_shell_io_t *term, int argc, char **argv)
{
    const char *bus = SOLAR_OS_I2C_DEFAULT_BUS;
    int value_arg = 2;
    uint8_t legacy_address;
    if (argc >= 5 && !parse_u8(argv[2], &legacy_address)) {
        bus = argv[2];
        value_arg = 3;
    }
    uint8_t address;
    uint8_t reg;
    size_t len = 1;

    if (argc < value_arg + 2 ||
        argc > value_arg + 3 ||
        !parse_u8(argv[value_arg], &address) ||
        !parse_u8(argv[value_arg + 1], &reg) ||
        (argc == value_arg + 3 &&
         !parse_size_arg(argv[value_arg + 2], 1, I2C_READ_MAX_LEN, &len))) {
        solar_os_shell_io_writeln(term, "usage: i2c read [bus] <addr> <reg> [len]");
        return;
    }
    if (!i2c_bus_exists(term, bus)) {
        return;
    }

    uint8_t data[I2C_READ_MAX_LEN];
    const esp_err_t err = solar_os_i2c_bus_read_reg(bus, address, reg, data, len);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "i2c", "I2C hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term,
                                 "i2c read failed on %s: %s\n",
                                 bus,
                                 esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term, "0x%02x[0x%02x]:", address, reg);
    for (size_t i = 0; i < len; i++) {
        solar_os_shell_io_printf(term, " %02x", data[i]);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void i2c_cmd_write(solar_os_shell_io_t *term, int argc, char **argv)
{
    const char *bus = SOLAR_OS_I2C_DEFAULT_BUS;
    int value_arg = 2;
    uint8_t legacy_address;
    if (argc >= 6 && !parse_u8(argv[2], &legacy_address)) {
        bus = argv[2];
        value_arg = 3;
    }
    uint8_t address;
    uint8_t reg;
    uint8_t data[SOLAR_OS_SHELL_ARG_MAX];

    if (argc < value_arg + 3 ||
        !parse_u8(argv[value_arg], &address) ||
        !parse_u8(argv[value_arg + 1], &reg)) {
        solar_os_shell_io_writeln(term,
                                  "usage: i2c write [bus] <addr> <reg> <byte...>");
        return;
    }
    if (!i2c_bus_exists(term, bus)) {
        return;
    }

    const size_t len = (size_t)(argc - value_arg - 2);
    for (size_t i = 0; i < len; i++) {
        if (!parse_u8(argv[i + value_arg + 2], &data[i])) {
            solar_os_shell_io_writeln(term, "i2c write: byte values must be 0..255");
            return;
        }
    }

    const esp_err_t err = solar_os_i2c_bus_write_reg(bus, address, reg, data, len);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "i2c", "I2C hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term,
                                 "i2c write failed on %s: %s\n",
                                 bus,
                                 esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term, "0x%02x[0x%02x] <-", address, reg);
    for (size_t i = 0; i < len; i++) {
        solar_os_shell_io_printf(term, " %02x", data[i]);
    }
    solar_os_shell_io_put_char(term, '\n');
}

void solar_os_shell_cmd_i2c(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0 || strcmp(argv[1], "speed") == 0) {
        if (argc > 3) {
            i2c_print_usage(term);
            return;
        }
        i2c_print_status(term, argc == 3 ? argv[2] : NULL);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        i2c_cmd_scan(term, argc, argv);
    } else if (strcmp(argv[1], "probe") == 0) {
        i2c_cmd_probe(term, argc, argv);
    } else if (strcmp(argv[1], "read") == 0) {
        i2c_cmd_read(term, argc, argv);
    } else if (strcmp(argv[1], "write") == 0) {
        i2c_cmd_write(term, argc, argv);
    } else {
        i2c_print_usage(term);
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
static bool parse_u32_arg(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text) {
        return false;
    }

    uint32_t multiplier = 1U;
    if (*end == 'k' || *end == 'K') {
        multiplier = 1000U;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1000000U;
        end++;
    }

    if (*end != '\0' || parsed > UINT32_MAX / multiplier) {
        return false;
    }

    const uint32_t scaled = (uint32_t)parsed * multiplier;
    if (scaled < min || scaled > max) {
        return false;
    }

    *value = scaled;
    return true;
}

static const char *spi_bus_host_name(int host)
{
    if (host == SPI2_HOST) {
        return "spi2";
    }
    if (host == SPI3_HOST) {
        return "spi3";
    }
    return "unknown";
}

static void spi_print_bus_status(solar_os_shell_io_t *term,
                                 const solar_os_bus_info_t *info)
{
    solar_os_shell_io_printf(term,
                             "%s: %s %s host=%s SCLK=%d MISO=%d MOSI=%d ready=%s leases=%u max=%" PRIu32 "\n",
                             info->name,
                             solar_os_bus_origin_name(info->origin),
                             solar_os_bus_sharing_name(info->sharing),
                             spi_bus_host_name(info->config.spi.host),
                             info->config.spi.sclk_pin,
                             info->config.spi.miso_pin,
                             info->config.spi.mosi_pin,
                             info->ready ? "yes" : "no",
                             (unsigned)info->lease_count,
                             info->config.spi.max_transfer_size);
    solar_os_shell_io_write(term, "  CS:");
    for (size_t i = 0; i < info->config.spi.cs_count; i++) {
        solar_os_shell_io_printf(term,
                                 " %s(GPIO%d)",
                                 info->config.spi.cs[i].name,
                                 info->config.spi.cs[i].pin);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void spi_print_status(solar_os_shell_io_t *term, const char *name)
{
    if (name != NULL) {
        solar_os_bus_info_t info;
        if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_SPI, &info)) {
            solar_os_shell_io_printf(term, "spi status: bus '%s' not found\n", name);
            return;
        }
        spi_print_bus_status(term, &info);
        return;
    }

    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI);
    if (count == 0) {
        solar_os_shell_io_writeln(term, "SPI: no registered buses");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, i, &info)) {
            spi_print_bus_status(term, &info);
        }
    }
}

static void spi_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  spi [status [bus]]");
    solar_os_shell_io_writeln(term, "  spi xfer <bus> <cs> <mode> <hz> <byte...>");
    solar_os_shell_io_writeln(term, "  spi read <bus> <cs> <mode> <hz> <len> [fill]");
    solar_os_shell_io_writeln(term, "  spi write <bus> <cs> <mode> <hz> <byte...>");
}

static bool spi_resolve_cs(const solar_os_bus_info_t *info,
                           const char *text,
                           int *cs_pin)
{
    if (info == NULL || text == NULL || cs_pin == NULL) {
        return false;
    }
    for (size_t i = 0; i < info->config.spi.cs_count; i++) {
        if (strcmp(text, info->config.spi.cs[i].name) == 0) {
            *cs_pin = info->config.spi.cs[i].pin;
            return true;
        }
    }

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > 63) {
        return false;
    }
    for (size_t i = 0; i < info->config.spi.cs_count; i++) {
        if (parsed == info->config.spi.cs[i].pin) {
            *cs_pin = (int)parsed;
            return true;
        }
    }
    return false;
}

static bool spi_parse_common(solar_os_shell_io_t *term,
                             int argc,
                             char **argv,
                             int min_argc,
                             solar_os_bus_info_t *info,
                             int *cs_pin,
                             uint8_t *mode,
                             uint32_t *speed_hz)
{
    size_t parsed_mode;

    if (argc < min_argc || info == NULL || cs_pin == NULL ||
        mode == NULL || speed_hz == NULL) {
        return false;
    }
    if (!solar_os_bus_find(argv[2], SOLAR_OS_BUS_PROTOCOL_SPI, info)) {
        solar_os_shell_io_printf(term, "spi: bus '%s' not found\n", argv[2]);
        return false;
    }
    if (!spi_resolve_cs(info, argv[3], cs_pin)) {
        solar_os_shell_io_printf(term,
                                 "spi: chip-select '%s' is not declared on %s\n",
                                 argv[3],
                                 info->name);
        return false;
    }
    if (!parse_size_arg(argv[4], 0, 3, &parsed_mode) ||
        !parse_u32_arg(argv[5], 1, SOLAR_OS_BUS_SPI_MAX_SPEED_HZ, speed_hz)) {
        return false;
    }

    *mode = (uint8_t)parsed_mode;
    return true;
}

static void spi_print_rx(solar_os_shell_io_t *term, const uint8_t *rx, size_t len)
{
    solar_os_shell_io_write(term, "rx:");
    for (size_t i = 0; i < len; i++) {
        solar_os_shell_io_printf(term, " %02x", rx[i]);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void spi_print_transfer_error(solar_os_shell_io_t *term,
                                     const char *operation,
                                     const char *bus,
                                     esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(term,
                                 "spi %s: %s or its chip-select is busy\n",
                                 operation,
                                 bus);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "spi %s: bus '%s' not found\n", operation, bus);
    } else {
        solar_os_shell_io_printf(term,
                                 "spi %s failed on %s: %s\n",
                                 operation,
                                 bus,
                                 esp_err_to_name(err));
    }
}

static void spi_cmd_xfer(solar_os_shell_io_t *term,
                         int argc,
                         char **argv,
                         const char *owner)
{
    solar_os_bus_info_t info;
    int cs_pin;
    uint8_t mode;
    uint32_t speed_hz;
    uint8_t tx[SPI_TRANSFER_MAX_LEN];
    uint8_t rx[SPI_TRANSFER_MAX_LEN];

    if (!spi_parse_common(term, argc, argv, 7, &info, &cs_pin, &mode, &speed_hz) ||
        argc > 6 + SPI_TRANSFER_MAX_LEN) {
        solar_os_shell_io_writeln(term,
                                  "usage: spi xfer <bus> <cs> <mode> <hz> <byte...>");
        return;
    }

    const size_t len = (size_t)(argc - 6);
    for (size_t i = 0; i < len; i++) {
        if (!parse_u8(argv[i + 6], &tx[i])) {
            solar_os_shell_io_writeln(term, "spi xfer: byte values must be 0..255");
            return;
        }
    }

    const esp_err_t err = solar_os_bus_spi_transfer_once(info.name,
                                                         cs_pin,
                                                         mode,
                                                         speed_hz,
                                                         tx,
                                                         rx,
                                                         len,
                                                         owner);
    if (err != ESP_OK) {
        spi_print_transfer_error(term, "xfer", info.name, err);
        return;
    }

    spi_print_rx(term, rx, len);
}

static void spi_cmd_read(solar_os_shell_io_t *term,
                         int argc,
                         char **argv,
                         const char *owner)
{
    solar_os_bus_info_t info;
    int cs_pin;
    uint8_t mode;
    uint32_t speed_hz;
    size_t len;
    uint8_t fill = 0xff;
    uint8_t tx[SPI_TRANSFER_MAX_LEN];
    uint8_t rx[SPI_TRANSFER_MAX_LEN];

    if (!spi_parse_common(term, argc, argv, 7, &info, &cs_pin, &mode, &speed_hz) ||
        !parse_size_arg(argv[6], 1, SPI_TRANSFER_MAX_LEN, &len) ||
        (argc == 8 && !parse_u8(argv[7], &fill)) ||
        argc > 8) {
        solar_os_shell_io_writeln(term,
                                  "usage: spi read <bus> <cs> <mode> <hz> <len> [fill]");
        return;
    }

    memset(tx, fill, len);
    const esp_err_t err = solar_os_bus_spi_transfer_once(info.name,
                                                         cs_pin,
                                                         mode,
                                                         speed_hz,
                                                         tx,
                                                         rx,
                                                         len,
                                                         owner);
    if (err != ESP_OK) {
        spi_print_transfer_error(term, "read", info.name, err);
        return;
    }

    spi_print_rx(term, rx, len);
}

static void spi_cmd_write(solar_os_shell_io_t *term,
                          int argc,
                          char **argv,
                          const char *owner)
{
    solar_os_bus_info_t info;
    int cs_pin;
    uint8_t mode;
    uint32_t speed_hz;
    uint8_t tx[SPI_TRANSFER_MAX_LEN];

    if (!spi_parse_common(term, argc, argv, 7, &info, &cs_pin, &mode, &speed_hz) ||
        argc > 6 + SPI_TRANSFER_MAX_LEN) {
        solar_os_shell_io_writeln(term,
                                  "usage: spi write <bus> <cs> <mode> <hz> <byte...>");
        return;
    }

    const size_t len = (size_t)(argc - 6);
    for (size_t i = 0; i < len; i++) {
        if (!parse_u8(argv[i + 6], &tx[i])) {
            solar_os_shell_io_writeln(term, "spi write: byte values must be 0..255");
            return;
        }
    }

    const esp_err_t err = solar_os_bus_spi_transfer_once(info.name,
                                                         cs_pin,
                                                         mode,
                                                         speed_hz,
                                                         tx,
                                                         NULL,
                                                         len,
                                                         owner);
    if (err != ESP_OK) {
        spi_print_transfer_error(term, "write", info.name, err);
        return;
    }

    solar_os_shell_io_printf(term, "wrote %u byte%s\n", (unsigned)len, len == 1 ? "" : "s");
}

void solar_os_shell_cmd_spi(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char owner[SOLAR_OS_BUS_OWNER_MAX];
    (void)snprintf(owner, sizeof(owner), "shell-spi:%" PRIxPTR, (uintptr_t)ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 3) {
            spi_print_usage(term);
            return;
        }
        spi_print_status(term, argc == 3 ? argv[2] : NULL);
        return;
    }

    if (strcmp(argv[1], "xfer") == 0) {
        spi_cmd_xfer(term, argc, argv, owner);
    } else if (strcmp(argv[1], "read") == 0) {
        spi_cmd_read(term, argc, argv, owner);
    } else if (strcmp(argv[1], "write") == 0) {
        spi_cmd_write(term, argc, argv, owner);
    } else {
        spi_print_usage(term);
    }
}
#endif

static void print_rtc_warning(solar_os_shell_io_t *term, const solar_os_datetime_t *datetime)
{
    if (datetime != NULL && !datetime->clock_integrity) {
        solar_os_shell_io_writeln(term, "warning: RTC clock integrity flag is set");
    }
}

void solar_os_shell_cmd_date(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_datetime_t datetime;
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1) {
        const esp_err_t err = solar_os_time_get_datetime(&datetime);
        if (err != ESP_OK) {
            if (shell_print_not_supported(term, "date", "RTC", err)) {
                return;
            }
            solar_os_shell_io_printf(term, "date: RTC read failed: %s\n", esp_err_to_name(err));
            return;
        }

        solar_os_shell_io_printf(term,
                                 "%04u-%02u-%02u\n",
                                 (unsigned)datetime.year,
                                 (unsigned)datetime.month,
                                 (unsigned)datetime.day);
        print_rtc_warning(term, &datetime);
        return;
    }

    if (argc != 2) {
        solar_os_shell_io_writeln(term, "usage: date [YYYY-MM-DD]");
        return;
    }

    const esp_err_t read_err = solar_os_time_get_datetime(&datetime);
    if (read_err != ESP_OK) {
        datetime = (solar_os_datetime_t){
            .year = 2026,
            .month = 1,
            .day = 1,
            .hour = 0,
            .minute = 0,
            .second = 0,
            .weekday = 0,
            .clock_integrity = true,
        };
    }

    if (!parse_date_arg(argv[1], &datetime)) {
        solar_os_shell_io_writeln(term, "usage: date [YYYY-MM-DD]");
        return;
    }

    const esp_err_t err = solar_os_time_set_datetime(&datetime);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "date", "RTC", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "date: RTC write failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "%04u-%02u-%02u\n",
                             (unsigned)datetime.year,
                             (unsigned)datetime.month,
                             (unsigned)datetime.day);
}

void solar_os_shell_cmd_time(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_datetime_t datetime;
    const esp_err_t read_err = solar_os_time_get_datetime(&datetime);
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1) {
        if (read_err != ESP_OK) {
            if (shell_print_not_supported(term, "time", "RTC", read_err)) {
                return;
            }
            solar_os_shell_io_printf(term, "time: RTC read failed: %s\n", esp_err_to_name(read_err));
            return;
        }

        solar_os_shell_io_printf(term,
                                 "%02u:%02u:%02u\n",
                                 (unsigned)datetime.hour,
                                 (unsigned)datetime.minute,
                                 (unsigned)datetime.second);
        print_rtc_warning(term, &datetime);
        return;
    }

    if (argc != 2) {
        solar_os_shell_io_writeln(term, "usage: time [HH:MM[:SS]]");
        return;
    }

    if (read_err != ESP_OK) {
        if (read_err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "time: RTC not available on this board");
            return;
        }
        solar_os_shell_io_writeln(term, "time: set date first with date YYYY-MM-DD");
        return;
    }

    if (!parse_time_arg(argv[1], &datetime)) {
        solar_os_shell_io_writeln(term, "usage: time [HH:MM[:SS]]");
        return;
    }

    const esp_err_t err = solar_os_time_set_datetime(&datetime);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "time", "RTC", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "time: RTC write failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "%02u:%02u:%02u\n",
                             (unsigned)datetime.hour,
                             (unsigned)datetime.minute,
                             (unsigned)datetime.second);
}

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
static bool read_environment_for_shell(solar_os_shell_io_t *term, solar_os_environment_t *environment)
{
    const esp_err_t err = solar_os_sensors_read_environment(environment);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "sensor", "environment sensors", err)) {
            return false;
        }
        solar_os_shell_io_printf(term, "sensor read failed: %s\n", esp_err_to_name(err));
        return false;
    }

    return true;
}

void solar_os_shell_cmd_temperature(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: temperature");
        return;
    }

    solar_os_environment_t environment;
    if (!read_environment_for_shell(term, &environment)) {
        return;
    }

    terminal_printf_fixed_1(term, "Temperature", environment.temperature_c, "C");
}

void solar_os_shell_cmd_humidity(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: humidity");
        return;
    }

    solar_os_environment_t environment;
    if (!read_environment_for_shell(term, &environment)) {
        return;
    }

    terminal_printf_fixed_1(term, "Humidity", environment.humidity_percent, "%RH");
}
#endif
