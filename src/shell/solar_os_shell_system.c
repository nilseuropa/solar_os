#include "solar_os_shell_commands.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_i2c.h"
#include "solar_os_power.h"
#include "solar_os_ramfs.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_spi.h"
#include "solar_os_storage.h"
#include "solar_os_time.h"
#include "solar_os_uart.h"
#include "solar_os_wifi.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif
#ifndef SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES
#define SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES ""
#endif

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
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
#endif

void solar_os_shell_cmd_board(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char caps[192];

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: board");
        return;
    }

    solar_os_board_capabilities_format(caps, sizeof(caps));
    solar_os_shell_io_printf(term, "Board: %s\n", SOLAR_OS_BOARD_NAME);
    solar_os_shell_io_printf(term, "ID: %s\n", SOLAR_OS_BOARD_ID);
#ifdef SOLAR_OS_BOARD_MODULE_NAME
    solar_os_shell_io_printf(term, "Module: %s\n", SOLAR_OS_BOARD_MODULE_NAME);
#endif
    solar_os_shell_io_printf(term, "Capabilities: %s\n", caps);
#if SOLAR_OS_BOARD_HAS_PSRAM
    solar_os_shell_io_printf(term,
                             "PSRAM: declared %u bytes, heap %u bytes\n",
                             (unsigned)SOLAR_OS_BOARD_PSRAM_BYTES,
                             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
    solar_os_shell_io_printf(term,
                             "PSRAM: not declared, heap %u bytes\n",
                             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#endif
}

void solar_os_shell_cmd_version(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: version");
        return;
    }

    solar_os_shell_io_printf(term, "SolarOS %s\n", SOLAR_OS_VERSION);
    solar_os_shell_io_printf(term, "Flavor: %s\n", SOLAR_OS_FLAVOR_NAME);
    solar_os_shell_io_printf(term,
                             "Required capabilities: %s\n",
                             SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES[0] != '\0' ?
                                SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES :
                                "none");
    solar_os_shell_io_write(term, "Packages: ");
    solar_os_shell_io_writeln(term, SOLAR_OS_PACKAGE_LIST);
}

static void pkg_print_wrapped_list(solar_os_shell_io_t *term,
                                   const char *title,
                                   const char *text)
{
    enum { pkg_width = 76, pkg_indent = 2 };
    size_t col = pkg_indent;

    solar_os_shell_io_printf(term, "%s:\n", title);
    solar_os_shell_io_write(term, "  ");
    if (text == NULL || text[0] == '\0') {
        solar_os_shell_io_writeln(term, "none");
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        const char *word = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            cursor++;
        }

        const size_t len = (size_t)(cursor - word);
        const bool needs_space = col > pkg_indent;
        if (needs_space && col + 1U + len > pkg_width) {
            solar_os_shell_io_put_char(term, '\n');
            solar_os_shell_io_write(term, "  ");
            col = pkg_indent;
        } else if (needs_space) {
            solar_os_shell_io_put_char(term, ' ');
            col++;
        }

        solar_os_shell_io_write_len(term, word, len);
        col += len;
    }
    solar_os_shell_io_put_char(term, '\n');
}

static bool pkg_token_next(const char **cursor, const char **token, size_t *token_len)
{
    if (cursor == NULL || *cursor == NULL || token == NULL || token_len == NULL) {
        return false;
    }

    while (**cursor == ' ') {
        (*cursor)++;
    }
    if (**cursor == '\0') {
        return false;
    }

    *token = *cursor;
    while (**cursor != '\0' && **cursor != ' ') {
        (*cursor)++;
    }
    *token_len = (size_t)(*cursor - *token);
    return true;
}

static bool pkg_token_split(const char *token,
                            size_t token_len,
                            const char **prefix,
                            size_t *prefix_len,
                            const char **name,
                            size_t *name_len)
{
    if (token == NULL || token_len == 0 || prefix == NULL || prefix_len == NULL ||
        name == NULL || name_len == NULL) {
        return false;
    }

    for (size_t i = 0; i < token_len; i++) {
        if (token[i] != '.') {
            continue;
        }
        if (i == 0 || i + 1 >= token_len) {
            return false;
        }
        *prefix = token;
        *prefix_len = i;
        *name = token + i + 1;
        *name_len = token_len - i - 1;
        return true;
    }
    return false;
}

static bool pkg_token_prefix_is(const char *prefix,
                                size_t prefix_len,
                                const char *expected)
{
    const size_t expected_len = strlen(expected);
    return prefix_len == expected_len && strncmp(prefix, expected, expected_len) == 0;
}

static bool pkg_print_build_unit_group(solar_os_shell_io_t *term,
                                       const char *text,
                                       const char *group)
{
    const char *cursor = text;
    const char *token = NULL;
    size_t token_len = 0;
    bool printed = false;

    while (pkg_token_next(&cursor, &token, &token_len)) {
        const char *prefix = NULL;
        const char *name = NULL;
        size_t prefix_len = 0;
        size_t name_len = 0;

        if (!pkg_token_split(token, token_len, &prefix, &prefix_len, &name, &name_len) ||
            !pkg_token_prefix_is(prefix, prefix_len, group)) {
            continue;
        }

        if (!printed) {
            solar_os_shell_io_printf(term, "  %s\n", group);
            printed = true;
        }
        solar_os_shell_io_write(term, "    ");
        solar_os_shell_io_write_len(term, name, name_len);
        solar_os_shell_io_put_char(term, '\n');
    }
    return printed;
}

static bool pkg_prefix_is_known(const char *prefix, size_t prefix_len)
{
    static const char * const groups[] = {"core", "service", "app", "job"};

    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        if (pkg_token_prefix_is(prefix, prefix_len, groups[i])) {
            return true;
        }
    }
    return false;
}

static bool pkg_print_other_build_units(solar_os_shell_io_t *term, const char *text)
{
    const char *cursor = text;
    const char *token = NULL;
    size_t token_len = 0;
    bool printed = false;

    while (pkg_token_next(&cursor, &token, &token_len)) {
        const char *prefix = NULL;
        const char *name = NULL;
        size_t prefix_len = 0;
        size_t name_len = 0;

        if (pkg_token_split(token, token_len, &prefix, &prefix_len, &name, &name_len) &&
            pkg_prefix_is_known(prefix, prefix_len)) {
            continue;
        }

        if (!printed) {
            solar_os_shell_io_writeln(term, "  other");
            printed = true;
        }
        solar_os_shell_io_write(term, "    ");
        solar_os_shell_io_write_len(term, token, token_len);
        solar_os_shell_io_put_char(term, '\n');
    }
    return printed;
}

static void pkg_print_build_unit_tree(solar_os_shell_io_t *term,
                                      const char *title,
                                      const char *text)
{
    bool printed = false;

    solar_os_shell_io_printf(term, "%s:\n", title);
    if (text == NULL || text[0] == '\0') {
        solar_os_shell_io_writeln(term, "  none");
        return;
    }

    printed |= pkg_print_build_unit_group(term, text, "core");
    printed |= pkg_print_build_unit_group(term, text, "service");
    printed |= pkg_print_build_unit_group(term, text, "app");
    printed |= pkg_print_build_unit_group(term, text, "job");
    printed |= pkg_print_other_build_units(term, text);

    if (!printed) {
        solar_os_shell_io_writeln(term, "  none");
    }
}

void solar_os_shell_cmd_pkg(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: pkg");
        return;
    }

    solar_os_shell_io_printf(term, "Flavor: %s\n", SOLAR_OS_FLAVOR_NAME);
    if (SOLAR_OS_FLAVOR_DESCRIPTION[0] != '\0') {
        solar_os_shell_io_printf(term, "%s\n", SOLAR_OS_FLAVOR_DESCRIPTION);
    }
    pkg_print_wrapped_list(term, "Groups", SOLAR_OS_PACKAGE_GROUP_LIST);
    pkg_print_wrapped_list(term, "Required capabilities", SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES);
    pkg_print_build_unit_tree(term, "Build units", SOLAR_OS_PACKAGE_LIST);
}

void solar_os_shell_cmd_clear(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    solar_os_shell_io_clear(terminal(ctx));
}

void solar_os_shell_cmd_sleep(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: sleep");
        return;
    }

    if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        solar_os_shell_io_writeln(term, "sleep is only available from the display shell");
        return;
    }

    solar_os_shell_io_writeln(term, "sleeping; press KEY to wake");
    solar_os_context_request_sleep(ctx);
}

static const char *power_wakeup_cause_name(int cause)
{
    switch ((esp_sleep_wakeup_cause_t)cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
        return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
        return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
        return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        return "touch";
    case ESP_SLEEP_WAKEUP_ULP:
        return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:
        return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
        return "uart";
    case ESP_SLEEP_WAKEUP_WIFI:
        return "wifi";
    case ESP_SLEEP_WAKEUP_COCPU:
        return "coproc";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
        return "coproc-trap";
    case ESP_SLEEP_WAKEUP_BT:
        return "bt";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
        return "undefined";
    }
}

static void power_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  power status");
    solar_os_shell_io_writeln(term, "  power profile [performance|balanced|solar|offline]");
    solar_os_shell_io_writeln(term, "  power idle [off|seconds]");
    solar_os_shell_io_writeln(term, "  power key [off|light]");
    solar_os_shell_io_writeln(term, "  power sleep");
}

static void power_print_status(solar_os_shell_io_t *term)
{
    solar_os_power_status_t status;
    char last_sleep[32];
    char idle_text[32];

    solar_os_power_get_status(&status);
    solar_os_time_format_uptime(status.last_sleep_duration_ms, last_sleep, sizeof(last_sleep));
    if (status.idle_sleep_ms == 0) {
        strlcpy(idle_text, "off", sizeof(idle_text));
    } else {
        solar_os_time_format_uptime(status.idle_sleep_ms, idle_text, sizeof(idle_text));
    }

    solar_os_shell_io_printf(term,
                             "Profile: %s\n",
                             solar_os_power_profile_name(status.profile));
    solar_os_shell_io_printf(term,
                             "CPU: %" PRIu32 "-%" PRIu32 " MHz\n",
                             status.cpu_min_mhz,
                             status.cpu_max_mhz);
    solar_os_shell_io_printf(term,
                             "Automatic light sleep: %s\n",
                             status.automatic_light_sleep ? "on" : "off");
    solar_os_shell_io_printf(term,
                             "Explicit sleep transition: %s\n",
                             status.explicit_sleep_active ? "active" : "idle");
    if (status.automatic_light_sleep_holdoff_ms > 0) {
        char holdoff_text[32];
        solar_os_time_format_uptime(status.automatic_light_sleep_holdoff_ms,
                                    holdoff_text,
                                    sizeof(holdoff_text));
        solar_os_shell_io_printf(term, "Automatic sleep holdoff: %s\n", holdoff_text);
    }
    solar_os_shell_io_printf(term,
                             "PM: %s%s%s\n",
                             status.pm_configured ? "configured" : "not configured",
                             status.pm_last_error == ESP_OK ? "" : " ",
                             status.pm_last_error == ESP_OK ? "" : esp_err_to_name(status.pm_last_error));
    solar_os_shell_io_printf(term,
                             "BT modem sleep: %s%s%s\n",
                             status.bt_sleep_enabled ? "on" : "off",
                             status.bt_sleep_last_error == ESP_OK ? "" : " ",
                             status.bt_sleep_last_error == ESP_OK ?
                                "" : esp_err_to_name(status.bt_sleep_last_error));
    solar_os_shell_io_printf(term,
                             "KEY short press: %s\n",
                             solar_os_power_key_action_name(status.key_action));
    solar_os_shell_io_printf(term, "Idle sleep: %s\n", idle_text);
    solar_os_shell_io_printf(term,
                             "Light sleep count: %" PRIu32 "\n",
                             status.light_sleep_count);
    solar_os_shell_io_printf(term, "Last sleep: %s\n", last_sleep);
    solar_os_shell_io_printf(term,
                             "Last wake: %s (%d) ext1=0x%016" PRIx64 "\n",
                             power_wakeup_cause_name(status.last_wakeup_cause),
                             status.last_wakeup_cause,
                             status.last_wakeup_ext1);
}

void solar_os_shell_cmd_power(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: power status");
            return;
        }
        power_print_status(term);
        return;
    }

    if (strcmp(argv[1], "profile") == 0) {
        if (argc == 2) {
            solar_os_power_status_t status;
            solar_os_power_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "profile: %s\n",
                                     solar_os_power_profile_name(status.profile));
            solar_os_shell_io_writeln(term, "values: performance balanced solar offline");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term,
                                      "usage: power profile [performance|balanced|solar|offline]");
            return;
        }

        solar_os_power_profile_t profile;
        if (!solar_os_power_parse_profile(argv[2], &profile)) {
            solar_os_shell_io_printf(term, "power profile: invalid value: %s\n", argv[2]);
            solar_os_shell_io_writeln(term, "values: performance balanced solar offline");
            return;
        }

        const esp_err_t err = solar_os_power_set_profile(profile);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "power profile: %s\n",
                                     solar_os_power_profile_name(profile));
        } else {
            solar_os_shell_io_printf(term,
                                     "power profile: save failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "idle") == 0) {
        if (argc == 2) {
            solar_os_power_status_t status;
            solar_os_power_get_status(&status);
            if (status.idle_sleep_ms == 0) {
                solar_os_shell_io_writeln(term, "idle: off");
            } else {
                solar_os_shell_io_printf(term,
                                         "idle: %" PRIu32 " seconds\n",
                                         status.idle_sleep_ms / 1000U);
            }
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: power idle [off|seconds]");
            return;
        }

        uint32_t idle_ms = 0;
        if (strcmp(argv[2], "off") != 0 && strcmp(argv[2], "0") != 0) {
            size_t seconds = 0;
            if (!solar_os_shell_parse_size_arg(argv[2], 1, 86400, &seconds)) {
                solar_os_shell_io_printf(term, "power idle: invalid seconds: %s\n", argv[2]);
                return;
            }
            idle_ms = (uint32_t)seconds * 1000U;
        }

        const esp_err_t err = solar_os_power_set_idle_sleep_ms(idle_ms);
        if (err == ESP_OK) {
            if (idle_ms == 0) {
                solar_os_shell_io_writeln(term, "power idle: off");
            } else {
                solar_os_shell_io_printf(term,
                                         "power idle: %" PRIu32 " seconds\n",
                                         idle_ms / 1000U);
            }
        } else {
            solar_os_shell_io_printf(term,
                                     "power idle: save failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "key") == 0) {
        if (argc == 2) {
            solar_os_power_status_t status;
            solar_os_power_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "key: %s\n",
                                     solar_os_power_key_action_name(status.key_action));
            solar_os_shell_io_writeln(term, "values: off light");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: power key [off|light]");
            return;
        }

        solar_os_power_key_action_t action;
        if (!solar_os_power_parse_key_action(argv[2], &action)) {
            solar_os_shell_io_printf(term, "power key: invalid value: %s\n", argv[2]);
            solar_os_shell_io_writeln(term, "values: off light");
            return;
        }

        const esp_err_t err = solar_os_power_set_key_action(action);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "power key: %s\n",
                                     solar_os_power_key_action_name(action));
        } else {
            solar_os_shell_io_printf(term,
                                     "power key: save failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "sleep") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: power sleep");
            return;
        }
        if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term, "sleep is only available from the display shell");
            return;
        }
        solar_os_shell_io_writeln(term, "sleeping; press KEY to wake");
        solar_os_context_request_sleep(ctx);
        return;
    }

    power_print_usage(term);
}

void solar_os_shell_cmd_status(solar_os_context_t *ctx, int argc, char **argv)
{
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    char ble_status[64];
#endif
    char sd_status[64];
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    char wifi_status[64];
#endif
    char uptime[32];
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    solar_os_battery_status_t battery_status;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_status_t audio_status;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART
    solar_os_uart_status_t uart_status;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    solar_os_spi_status_t spi_status;
#endif
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argc;
    (void)argv;

    solar_os_storage_get_status(sd_status, sizeof(sd_status));
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    solar_os_ble_keyboard_get_status(ble_status, sizeof(ble_status));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_wifi_get_status_text(wifi_status, sizeof(wifi_status));
#endif
    solar_os_time_format_uptime(solar_os_time_uptime_ms(), uptime, sizeof(uptime));

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    solar_os_shell_io_printf(term, "BLE: %s\n", ble_status);
#endif
    solar_os_shell_io_printf(term, "SD: %s\n", sd_status);
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_shell_io_printf(term, "WiFi: %s\n", wifi_status);
#endif
    solar_os_shell_io_printf(term, "Uptime: %s\n", uptime);
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    const esp_err_t battery_err = solar_os_battery_get_status(&battery_status);
    if (battery_err == ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "Battery: %u.%03u V, %u%% est.\n",
                                 (unsigned)(battery_status.voltage_mv / 1000U),
                                 (unsigned)(battery_status.voltage_mv % 1000U),
                                 (unsigned)battery_status.percent);
    } else if (battery_err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "Battery: not available on this board");
    } else {
        solar_os_shell_io_printf(term, "Battery: unavailable (%s)\n", esp_err_to_name(battery_err));
    }
#endif
    solar_os_shell_io_printf(term,
                             "Heap: internal %u, PSRAM %u\n",
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if SOLAR_OS_PACKAGE_SERVICE_I2C
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        solar_os_shell_io_printf(term,
                                 "I2C: SDA %d, SCL %d, %" PRIu32 " Hz\n",
                                 solar_os_i2c_get_sda_pin(),
                                 solar_os_i2c_get_scl_pin(),
                                 solar_os_i2c_get_speed_hz());
    } else {
        solar_os_shell_io_writeln(term, "I2C: not available on this board");
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    if (solar_os_spi_get_status(&spi_status) == ESP_OK && spi_status.available) {
        solar_os_shell_io_printf(term,
                                 "%s: SCK %d, MISO %d, MOSI %d, CS",
                                 spi_status.name != NULL ? spi_status.name : "SPI",
                                 spi_status.sclk_pin,
                                 spi_status.miso_pin,
                                 spi_status.mosi_pin);
        for (size_t i = 0; i < spi_status.cs_count; i++) {
            solar_os_shell_io_printf(term,
                                     " %s(GPIO%d)",
                                     spi_status.cs[i].name,
                                     spi_status.cs[i].pin);
        }
        solar_os_shell_io_put_char(term, '\n');
    } else {
        solar_os_shell_io_writeln(term, "SPI: not available on this board");
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_get_status(&audio_status);
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_AUDIO)) {
        solar_os_shell_io_printf(term,
                                 "Audio: %s, %" PRIu32 " Hz %uch %ubit, vol %u, mic ",
                                 audio_status.initialized ? "on" : "off",
                                 audio_status.sample_rate,
                                 (unsigned)audio_status.channels,
                                 (unsigned)audio_status.bits_per_sample,
                                 (unsigned)audio_status.volume);
        audio_print_gain(term, audio_status.mic_gain_db);
        solar_os_shell_io_put_char(term, '\n');
    } else {
        solar_os_shell_io_writeln(term, "Audio: not available on this board");
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART
    solar_os_uart_get_status(&uart_status);
    if (uart_status.initialized) {
        if (uart_status.rx_buffered_valid) {
            solar_os_shell_io_printf(term,
                                     "UART: UART%d TX %d RX %d, %" PRIu32 " baud, %s mode, %u buffered\n",
                                     uart_status.port_num,
                                     uart_status.tx_pin,
                                     uart_status.rx_pin,
                                     uart_status.baud_rate,
                                     solar_os_uart_mode_name(uart_status.mode),
                                     (unsigned)uart_status.rx_buffered);
        } else {
            solar_os_shell_io_printf(term,
                                     "UART: UART%d TX %d RX %d, %" PRIu32 " baud, %s mode, buffered busy\n",
                                     uart_status.port_num,
                                     uart_status.tx_pin,
                                     uart_status.rx_pin,
                                     uart_status.baud_rate,
                                     solar_os_uart_mode_name(uart_status.mode));
        }
    } else {
        if (uart_status.port_num >= 0) {
            solar_os_shell_io_printf(term,
                                     "UART: unavailable (UART%d TX %d RX %d)\n",
                                     uart_status.port_num,
                                     uart_status.tx_pin,
                                     uart_status.rx_pin);
        } else {
            solar_os_shell_io_writeln(term, "UART: not available on this board");
        }
    }
#endif
}

void solar_os_shell_cmd_uptime(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char uptime[32];

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: uptime");
        return;
    }

    solar_os_time_format_uptime(solar_os_time_uptime_ms(), uptime, sizeof(uptime));
    solar_os_shell_io_printf(term, "up %s\n", uptime);
}

static void mem_print_region(solar_os_shell_io_t *term, const char *label, uint32_t caps)
{
    char total[16];
    char free_now[16];
    char low[16];
    char largest[16];

    format_bytes(heap_caps_get_total_size(caps), total, sizeof(total));
    format_bytes(heap_caps_get_free_size(caps), free_now, sizeof(free_now));
    format_bytes(heap_caps_get_minimum_free_size(caps), low, sizeof(low));
    format_bytes(heap_caps_get_largest_free_block(caps), largest, sizeof(largest));
    solar_os_shell_io_printf(term,
                             "%s: total %s free %s low %s max %s\n",
                             label,
                             total,
                             free_now,
                             low,
                             largest);
}

void solar_os_shell_cmd_mem(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: mem");
        return;
    }

    mem_print_region(term, "Internal", MALLOC_CAP_INTERNAL);
    mem_print_region(term, "PSRAM", MALLOC_CAP_SPIRAM);
    mem_print_region(term, "DMA", MALLOC_CAP_DMA);
}

static bool ramfs_parse_size(const char *text, size_t *bytes)
{
    if (text == NULL || text[0] == '\0' || bytes == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || value == 0) {
        return false;
    }

    uint64_t scale = 1;
    char suffix[4] = {0};
    size_t suffix_len = 0;
    while (*end != '\0' && suffix_len + 1 < sizeof(suffix)) {
        suffix[suffix_len++] = (char)tolower((unsigned char)*end++);
    }
    if (*end != '\0') {
        return false;
    }

    if (suffix[0] == '\0' || strcmp(suffix, "b") == 0) {
        scale = 1;
    } else if (strcmp(suffix, "k") == 0 ||
               strcmp(suffix, "kb") == 0 ||
               strcmp(suffix, "ki") == 0 ||
               strcmp(suffix, "kib") == 0) {
        scale = 1024ULL;
    } else if (strcmp(suffix, "m") == 0 ||
               strcmp(suffix, "mb") == 0 ||
               strcmp(suffix, "mi") == 0 ||
               strcmp(suffix, "mib") == 0) {
        scale = 1024ULL * 1024ULL;
    } else if (strcmp(suffix, "g") == 0 ||
               strcmp(suffix, "gb") == 0 ||
               strcmp(suffix, "gi") == 0 ||
               strcmp(suffix, "gib") == 0) {
        scale = 1024ULL * 1024ULL * 1024ULL;
    } else {
        return false;
    }

    if (value > UINT64_MAX / scale || value * scale > SIZE_MAX) {
        return false;
    }
    *bytes = (size_t)(value * scale);
    return true;
}

static void ramfs_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  ramfs [status]");
    solar_os_shell_io_writeln(term, "  ramfs mount /path size");
    solar_os_shell_io_writeln(term, "  ramfs unmount /path");
}

static void ramfs_print_status(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_ramfs_mount_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "ramfs: no mounts");
        return;
    }

    solar_os_shell_io_writeln(term, "PATH       TOTAL    USED     FREE     FILES DIRS OPEN");
    for (size_t i = 0; i < count; i++) {
        solar_os_ramfs_info_t info;
        char total[12];
        char used[12];
        char free_space[12];

        if (!solar_os_ramfs_get_info(i, &info)) {
            continue;
        }
        format_bytes(info.total_bytes, total, sizeof(total));
        format_bytes(info.used_bytes, used, sizeof(used));
        format_bytes(info.free_bytes, free_space, sizeof(free_space));
        solar_os_shell_io_printf(term,
                                 "%-10s %-8s %-8s %-8s %5u %4u %4u\n",
                                 info.mount_point,
                                 total,
                                 used,
                                 free_space,
                                 (unsigned)info.file_count,
                                 (unsigned)info.dir_count,
                                 (unsigned)info.open_count);
    }
}

void solar_os_shell_cmd_ramfs(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: ramfs [status]");
            return;
        }
        ramfs_print_status(term);
        return;
    }

    if (strcmp(argv[1], "mount") == 0) {
        size_t quota = 0;
        if (argc != 4 || !ramfs_parse_size(argv[3], &quota)) {
            solar_os_shell_io_writeln(term, "usage: ramfs mount /path size");
            return;
        }

        const esp_err_t err = solar_os_ramfs_mount(argv[2], quota);
        if (err == ESP_OK) {
            ramfs_print_status(term);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "ramfs: PSRAM not available on this board");
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_printf(term, "ramfs: mount point already in use: %s\n", argv[2]);
        } else if (err == ESP_ERR_INVALID_SIZE) {
            solar_os_shell_io_writeln(term, "ramfs: invalid size or mount path too long");
        } else {
            solar_os_shell_io_printf(term, "ramfs mount failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "unmount") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: ramfs unmount /path");
            return;
        }

        const esp_err_t err = solar_os_ramfs_unmount(argv[2]);
        if (err == ESP_OK) {
            ramfs_print_status(term);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "ramfs: PSRAM not available on this board");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "ramfs: not mounted: %s\n", argv[2]);
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_printf(term, "ramfs: mount is busy: %s\n", argv[2]);
        } else {
            solar_os_shell_io_printf(term, "ramfs unmount failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    ramfs_print_usage(term);
}

void solar_os_shell_cmd_df(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: df");
        return;
    }

    bool any = false;
    solar_os_shell_io_writeln(term, "Filesystem  Total    Used     Free     Use% Mount");

    const esp_err_t scan_err = solar_os_storage_rescan();
    if (scan_err != ESP_OK && scan_err != ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_printf(term, "sd         read failed: %s\n", esp_err_to_name(scan_err));
        any = true;
    }

    const size_t count = solar_os_storage_mount_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_mount_info_t mount;
        solar_os_storage_usage_t usage;
        char total[12];
        char used[12];
        char free_space[12];

        if (!solar_os_storage_get_mount(i, &mount)) {
            continue;
        }

        const esp_err_t err = solar_os_storage_get_usage_for_path(mount.mount_point, &usage);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "%-10s read failed: %s\n",
                                     mount.name,
                                     esp_err_to_name(err));
            any = true;
            continue;
        }

        format_bytes(usage.total_bytes, total, sizeof(total));
        format_bytes(usage.used_bytes, used, sizeof(used));
        format_bytes(usage.free_bytes, free_space, sizeof(free_space));
        const uint32_t used_percent = usage.total_bytes > 0 ?
            (uint32_t)((usage.used_bytes * 100ULL) / usage.total_bytes) :
            0U;

        solar_os_shell_io_printf(term,
                                 "%-10s %-8s %-8s %-8s %3u%% %s\n",
                                 mount.name,
                                 total,
                                 used,
                                 free_space,
                                 (unsigned)used_percent,
                                 mount.mount_point);
        any = true;
    }

    if (!any) {
        solar_os_shell_io_writeln(term, "df: no mounted filesystems");
    }
}

static char task_state_char(eTaskState state)
{
    switch (state) {
    case eRunning:
        return 'R';
    case eReady:
        return 'r';
    case eBlocked:
        return 'B';
    case eSuspended:
        return 'S';
    case eDeleted:
        return 'D';
    case eInvalid:
    default:
        return '?';
    }
}

void solar_os_shell_cmd_top(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: top");
        return;
    }

#if (configUSE_TRACE_FACILITY == 1)
    const UBaseType_t task_capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks =
        heap_caps_calloc(task_capacity, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tasks == NULL) {
        tasks = heap_caps_calloc(task_capacity, sizeof(TaskStatus_t), MALLOC_CAP_8BIT);
    }
    if (tasks == NULL) {
        solar_os_shell_io_writeln(term, "top: out of memory");
        return;
    }

#if (configGENERATE_RUN_TIME_STATS == 1)
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t task_count = uxTaskGetSystemState(tasks, task_capacity, &total_runtime);
#else
    UBaseType_t task_count = uxTaskGetSystemState(tasks, task_capacity, NULL);
#endif
    if (task_count == 0) {
        heap_caps_free(tasks);
        solar_os_shell_io_writeln(term, "top: task snapshot failed");
        return;
    }

    for (UBaseType_t i = 0; i < task_count; i++) {
        for (UBaseType_t j = i + 1; j < task_count; j++) {
            if (tasks[j].ulRunTimeCounter > tasks[i].ulRunTimeCounter) {
                const TaskStatus_t temp = tasks[i];
                tasks[i] = tasks[j];
                tasks[j] = temp;
            }
        }
    }

    solar_os_shell_io_writeln(term, "TASK         S PRI CPU% STACK");
    for (UBaseType_t i = 0; i < task_count; i++) {
        char stack_free[16];
        const char *name = tasks[i].pcTaskName != NULL ? tasks[i].pcTaskName : "?";
        const uint64_t stack_bytes = (uint64_t)tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        format_bytes(stack_bytes, stack_free, sizeof(stack_free));

#if (configGENERATE_RUN_TIME_STATS == 1)
        const uint64_t cpu_tenths = total_runtime > 0 ?
            (((uint64_t)tasks[i].ulRunTimeCounter * 1000ULL) + ((uint64_t)total_runtime / 2ULL)) /
                (uint64_t)total_runtime :
            0ULL;
#else
        const uint64_t cpu_tenths = 0ULL;
#endif
        solar_os_shell_io_printf(term,
                                 "%-12.12s %c %3u %3" PRIu64 ".%u %s\n",
                                 name,
                                 task_state_char(tasks[i].eCurrentState),
                                 (unsigned)tasks[i].uxCurrentPriority,
                                 cpu_tenths / 10ULL,
                                 (unsigned)(cpu_tenths % 10ULL),
                                 stack_free);
    }

    heap_caps_free(tasks);
#else
    solar_os_shell_io_writeln(term, "top: FreeRTOS trace facility disabled");
#endif
}
