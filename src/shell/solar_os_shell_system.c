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
#include "esp_partition.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_engines.h"
#include "solar_os_i2c.h"
#include "solar_os_memory.h"
#include "solar_os_power.h"
#include "solar_os_ramfs.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_nvs_backup.h"

static const char * const power_commands[] = {
    "status", "profile", "idle", "key", "sleep", "suspend",
};
static const char * const nvs_commands[] = {
    "status", "list", "erase", "backup", "restore", "clear",
};
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
static const char * const engine_commands[] = {"status", "list", "reset"};
#endif
static const char * const ramfs_commands[] = {"status", "mount", "unmount"};
#include "solar_os_shell_io.h"
#include "solar_os_spi.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
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
    char caps[SOLAR_OS_BOARD_CAPABILITIES_TEXT_MAX];

    (void)argv;

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "board", argv[1], "board");
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
        solar_os_shell_diag_unexpected(term, "version", argv[1], "version");
        return;
    }

    solar_os_shell_io_printf(term, "SolarOS %s\n", SOLAR_OS_VERSION);
    solar_os_shell_io_printf(term, "Flavor: %s\n", SOLAR_OS_FLAVOR_NAME);
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
        solar_os_shell_diag_unexpected(term, "pkg", argv[1], "pkg");
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
    if (argc != 1) {
        solar_os_shell_diag_unexpected(terminal(ctx), "clear", argv[1], "clear");
        return;
    }
    solar_os_shell_io_clear(terminal(ctx));
}

void solar_os_shell_cmd_sleep(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "sleep", argv[1], "sleep");
        return;
    }

    if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        solar_os_shell_io_writeln(term, "sleep is only available from the display shell");
        return;
    }

    solar_os_shell_io_writeln(term, "sleeping; press KEY to wake");
    solar_os_context_request_sleep(ctx);
}

void solar_os_shell_cmd_suspend(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "suspend", argv[1], "suspend");
        return;
    }

    if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        solar_os_shell_io_writeln(term, "suspend is only available from the display shell");
        return;
    }

    solar_os_shell_io_writeln(term, "suspending; press KEY to resume");
    solar_os_context_request_suspend(ctx);
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
    solar_os_shell_io_writeln(term, "  power profile [performance|balanced|battery|lowpower]");
    solar_os_shell_io_writeln(term, "  power idle [off|seconds]");
    solar_os_shell_io_writeln(term, "  power key [off|sleep|suspend]");
    solar_os_shell_io_writeln(term, "  power sleep");
    solar_os_shell_io_writeln(term, "  power suspend");
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
                             "Effective profile: %s\n",
                             solar_os_power_profile_name(status.effective_profile));
    solar_os_shell_io_printf(term,
                             "Suspend: %s\n",
                             status.suspend_active ? "active" : "inactive");
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
                             status.pm_last_error == ESP_OK ? "" : solar_os_shell_error_text(status.pm_last_error));
    solar_os_shell_io_printf(term,
                             "BT modem sleep: %s%s%s\n",
                             status.bt_sleep_enabled ? "on" : "off",
                             status.bt_sleep_last_error == ESP_OK ? "" : " ",
                             status.bt_sleep_last_error == ESP_OK ?
                                "" : solar_os_shell_error_text(status.bt_sleep_last_error));
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
            solar_os_shell_diag_unexpected(term, "power status", argv[2], "power status");
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
            solar_os_shell_io_writeln(term, "values: performance balanced battery lowpower");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "power profile", argv[3],
                                           "power profile [performance|balanced|battery|lowpower]");
            return;
        }

        solar_os_power_profile_t profile;
        if (!solar_os_power_parse_profile(argv[2], &profile)) {
            solar_os_shell_diag_invalid(term, "power profile", "profile", argv[2],
                                        "performance, balanced, battery, or lowpower",
                                        "power profile [performance|balanced|battery|lowpower]", false);
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
                                     solar_os_shell_error_text(err));
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
            solar_os_shell_diag_unexpected(term, "power idle", argv[3],
                                           "power idle [off|seconds]");
            return;
        }

        uint32_t idle_ms = 0;
        if (strcmp(argv[2], "off") != 0 && strcmp(argv[2], "0") != 0) {
            size_t seconds = 0;
            if (!solar_os_shell_parse_size_arg(argv[2], 1, 86400, &seconds)) {
                solar_os_shell_diag_invalid(term, "power idle", "seconds", argv[2],
                                            "off or an integer from 1 to 86400",
                                            "power idle [off|seconds]", false);
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
                                     solar_os_shell_error_text(err));
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
            solar_os_shell_io_writeln(term, "values: off sleep suspend");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "power key", argv[3],
                                           "power key [off|sleep|suspend]");
            return;
        }

        solar_os_power_key_action_t action;
        if (!solar_os_power_parse_key_action(argv[2], &action)) {
            solar_os_shell_diag_invalid(term, "power key", "action", argv[2],
                                        "off, sleep, or suspend",
                                        "power key [off|sleep|suspend]", false);
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
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "sleep") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "power sleep", argv[2], "power sleep");
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

    if (strcmp(argv[1], "suspend") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "power suspend", argv[2],
                                           "power suspend");
            return;
        }
        if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term,
                                      "suspend is only available from the display shell");
            return;
        }
        solar_os_shell_io_writeln(term, "suspending; press KEY to resume");
        solar_os_context_request_suspend(ctx);
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "power",
                                   argc,
                                   argv,
                                   "power status|profile|idle|key|sleep|suspend",
                                   power_commands,
                                   sizeof(power_commands) / sizeof(power_commands[0]));
}

void solar_os_shell_cmd_status(solar_os_context_t *ctx, int argc, char **argv)
{
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    char ble_status[64];
#endif
    char storage_status[64];
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

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "status", argv[1], "status");
        return;
    }

    solar_os_storage_get_status(storage_status, sizeof(storage_status));
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
    solar_os_shell_io_printf(term, "Storage: %s\n", storage_status);
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_shell_io_printf(term, "WiFi: %s\n", wifi_status);
#endif
    solar_os_shell_io_printf(term, "Uptime: %s\n", uptime);
    solar_os_shell_session_t *shell_session =
        solar_os_context_shell_session(ctx);
    if (shell_session != NULL) {
        solar_os_shell_io_printf(
            term,
            "Last exit: %d\n",
            solar_os_shell_session_last_exit_code(shell_session));
    }
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
        solar_os_shell_io_printf(term, "Battery: unavailable (%s)\n", solar_os_shell_error_text(battery_err));
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
                                 spi_status.name[0] != '\0' ? spi_status.name : "SPI",
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
    if (solar_os_audio_output_available()) {
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
        solar_os_shell_diag_unexpected(term, "uptime", argv[1], "uptime");
        return;
    }

    solar_os_time_format_uptime(solar_os_time_uptime_ms(), uptime, sizeof(uptime));
    solar_os_shell_io_printf(term, "up %s\n", uptime);
}

typedef struct {
    char name[NVS_NS_NAME_MAX_SIZE];
    size_t key_count;
    size_t used_entries;
    bool usage_valid;
} nvs_namespace_summary_t;

static const char *nvs_type_name(nvs_type_t type)
{
    switch (type) {
    case NVS_TYPE_U8: return "u8";
    case NVS_TYPE_I8: return "i8";
    case NVS_TYPE_U16: return "u16";
    case NVS_TYPE_I16: return "i16";
    case NVS_TYPE_U32: return "u32";
    case NVS_TYPE_I32: return "i32";
    case NVS_TYPE_U64: return "u64";
    case NVS_TYPE_I64: return "i64";
    case NVS_TYPE_STR: return "string";
    case NVS_TYPE_BLOB: return "blob";
    default: return "unknown";
    }
}

static size_t nvs_scalar_size(nvs_type_t type)
{
    switch (type) {
    case NVS_TYPE_U8:
    case NVS_TYPE_I8:
        return 1U;
    case NVS_TYPE_U16:
    case NVS_TYPE_I16:
        return 2U;
    case NVS_TYPE_U32:
    case NVS_TYPE_I32:
        return 4U;
    case NVS_TYPE_U64:
    case NVS_TYPE_I64:
        return 8U;
    default:
        return 0U;
    }
}

static esp_err_t nvs_value_size(nvs_handle_t handle,
                                const nvs_entry_info_t *info,
                                size_t *size)
{
    *size = nvs_scalar_size(info->type);
    if (*size != 0U) {
        return ESP_OK;
    }
    if (info->type == NVS_TYPE_STR) {
        return nvs_get_str(handle, info->key, NULL, size);
    }
    if (info->type == NVS_TYPE_BLOB) {
        return nvs_get_blob(handle, info->key, NULL, size);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static size_t nvs_value_entries(nvs_type_t type, size_t size)
{
    if (type == NVS_TYPE_BLOB) {
        return 2U + (size + 31U) / 32U;
    }
    if (type == NVS_TYPE_STR) {
        return 1U + (size + 31U) / 32U;
    }
    return nvs_scalar_size(type) != 0U ? 1U : 0U;
}

static int nvs_namespace_compare(const void *left, const void *right)
{
    const nvs_namespace_summary_t *a = left;
    const nvs_namespace_summary_t *b = right;
    return strcmp(a->name, b->name);
}

static void nvs_list_namespaces(solar_os_shell_io_t *term)
{
    nvs_stats_t stats;
    esp_err_t error = nvs_get_stats(NULL, &stats);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(term, "nvs list: %s\n",
                                 solar_os_shell_error_text(error));
        return;
    }
    if (stats.namespace_count == 0U) {
        solar_os_shell_io_writeln(term, "nvs: no namespaces");
        return;
    }

    nvs_namespace_summary_t *summaries = solar_os_memory_calloc(
        stats.namespace_count,
        sizeof(*summaries),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "nvs.list");
    if (summaries == NULL) {
        solar_os_shell_io_writeln(term, "nvs list: out of memory");
        return;
    }

    size_t count = 0;
    nvs_iterator_t iterator = NULL;
    error = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY,
                           &iterator);
    while (error == ESP_OK) {
        nvs_entry_info_t info;
        error = nvs_entry_info(iterator, &info);
        if (error != ESP_OK) {
            break;
        }
        size_t index = 0;
        while (index < count &&
               strcmp(summaries[index].name, info.namespace_name) != 0) {
            index++;
        }
        if (index == count && count < stats.namespace_count) {
            strlcpy(summaries[count].name,
                    info.namespace_name,
                    sizeof(summaries[count].name));
            count++;
        }
        if (index < count) {
            summaries[index].key_count++;
        }
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    if (error != ESP_ERR_NVS_NOT_FOUND) {
        solar_os_memory_free(summaries);
        solar_os_shell_io_printf(term, "nvs list: %s\n",
                                 solar_os_shell_error_text(error));
        return;
    }

    for (size_t i = 0; i < count; i++) {
        nvs_handle_t handle = 0;
        if (nvs_open(summaries[i].name, NVS_READONLY, &handle) == ESP_OK) {
            size_t used = 0;
            if (nvs_get_used_entry_count(handle, &used) == ESP_OK) {
                summaries[i].used_entries = used + 1U;
                summaries[i].usage_valid = true;
            }
            nvs_close(handle);
        }
    }
    qsort(summaries, count, sizeof(*summaries), nvs_namespace_compare);

    solar_os_shell_io_writeln(term, "Namespace        Keys Entries");
    for (size_t i = 0; i < count; i++) {
        if (summaries[i].usage_valid) {
            solar_os_shell_io_printf(term, "%-15s %4u %7u\n",
                                     summaries[i].name,
                                     (unsigned)summaries[i].key_count,
                                     (unsigned)summaries[i].used_entries);
        } else {
            solar_os_shell_io_printf(term, "%-15s %4u       ?\n",
                                     summaries[i].name,
                                     (unsigned)summaries[i].key_count);
        }
    }
    solar_os_shell_io_printf(term,
                             "%u non-empty namespaces listed; NVS reports %u total\n",
                             (unsigned)count,
                             (unsigned)stats.namespace_count);
    solar_os_memory_free(summaries);
}

static void nvs_list_namespace(solar_os_shell_io_t *term,
                               const char *namespace_name)
{
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(term, "nvs list: %s: %s\n",
                                 namespace_name,
                                 solar_os_shell_error_text(error));
        return;
    }

    size_t used = 0;
    const bool usage_valid = nvs_get_used_entry_count(handle, &used) == ESP_OK;
    solar_os_shell_io_writeln(term, "Key             Type      Bytes Entries");
    size_t key_count = 0;
    nvs_iterator_t iterator = NULL;
    error = nvs_entry_find(NVS_DEFAULT_PART_NAME,
                           namespace_name,
                           NVS_TYPE_ANY,
                           &iterator);
    while (error == ESP_OK) {
        nvs_entry_info_t info;
        error = nvs_entry_info(iterator, &info);
        if (error != ESP_OK) {
            break;
        }
        size_t size = 0;
        const esp_err_t size_error = nvs_value_size(handle, &info, &size);
        const size_t entries = size_error == ESP_OK ?
            nvs_value_entries(info.type, size) : 0U;
        if (size_error == ESP_OK) {
            solar_os_shell_io_printf(term, "%-15s %-8s %6u %7u\n",
                                     info.key,
                                     nvs_type_name(info.type),
                                     (unsigned)size,
                                     (unsigned)entries);
        } else {
            solar_os_shell_io_printf(term, "%-15s %-8s      ?       ?\n",
                                     info.key,
                                     nvs_type_name(info.type));
        }
        key_count++;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    nvs_close(handle);

    if (error != ESP_ERR_NVS_NOT_FOUND) {
        solar_os_shell_io_printf(term, "nvs list: %s: %s\n",
                                 namespace_name,
                                 solar_os_shell_error_text(error));
        return;
    }
    if (usage_valid) {
        solar_os_shell_io_printf(term,
                                 "%s: %u keys, %u entries including namespace\n",
                                 namespace_name,
                                 (unsigned)key_count,
                                 (unsigned)(used + 1U));
    } else {
        solar_os_shell_io_printf(term, "%s: %u keys\n",
                                 namespace_name, (unsigned)key_count);
    }
}

static void nvs_erase_selection(solar_os_context_t *ctx,
                                solar_os_shell_io_t *term,
                                const char *namespace_name,
                                const char *key)
{
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(term, "nvs erase: %s: %s\n",
                                 namespace_name,
                                 solar_os_shell_error_text(error));
        return;
    }
    if (key != NULL) {
        nvs_type_t type;
        error = nvs_find_key(handle, key, &type);
    }
    nvs_close(handle);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(term, "nvs erase: %s%s%s: %s\n",
                                 namespace_name,
                                 key != NULL ? "/" : "",
                                 key != NULL ? key : "",
                                 solar_os_shell_error_text(error));
        return;
    }

    error = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = key != NULL ? nvs_erase_key(handle, key) : nvs_erase_all(handle);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        solar_os_shell_io_printf(term, "nvs erase: %s%s%s: %s\n",
                                 namespace_name,
                                 key != NULL ? "/" : "",
                                 key != NULL ? key : "",
                                 solar_os_shell_error_text(error));
        return;
    }

    if (key != NULL) {
        solar_os_shell_io_printf(term,
                                 "NVS key %s/%s erased; rebooting\n",
                                 namespace_name, key);
    } else {
        solar_os_shell_io_printf(
            term,
            "NVS namespace %s cleared; its namespace record remains; rebooting\n",
            namespace_name);
    }
    solar_os_shell_io_flush(term);
    vTaskDelay(pdMS_TO_TICKS(100));
    solar_os_context_reboot(ctx, "NVS selection erased");
}

void solar_os_shell_cmd_nvs(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        solar_os_shell_diag_subcommand(
            term,
            "nvs",
            argc,
            argv,
            "nvs status|list [namespace]|erase <namespace> [key]|backup [file]|restore [file]|clear",
            nvs_commands,
            sizeof(nvs_commands) / sizeof(nvs_commands[0]));
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (argc > 3) {
            solar_os_shell_diag_unexpected(term,
                                           "nvs list",
                                           argv[3],
                                           "nvs list [namespace]");
            return;
        }
        if (argc == 3) {
            nvs_list_namespace(term, argv[2]);
        } else {
            nvs_list_namespaces(term);
        }
        return;
    }

    if (strcmp(argv[1], "erase") == 0) {
        if (argc < 3) {
            solar_os_shell_io_writeln(
                term, "usage: nvs erase <namespace> [key]");
            return;
        }
        if (argc > 4) {
            solar_os_shell_diag_unexpected(
                term, "nvs erase", argv[4],
                "nvs erase <namespace> [key]");
            return;
        }
        nvs_erase_selection(ctx, term, argv[2], argc == 4 ? argv[3] : NULL);
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term,
                                           "nvs status",
                                           argv[2],
                                           "nvs status");
            return;
        }
        nvs_stats_t stats;
        const esp_err_t error = nvs_get_stats(NULL, &stats);
        if (error != ESP_OK) {
            solar_os_shell_io_printf(
                term, "nvs status: %s\n", solar_os_shell_error_text(error));
            return;
        }

        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_NVS,
            "nvs");
        if (partition != NULL) {
            solar_os_shell_io_printf(
                term, "NVS partition: %u bytes\n", (unsigned)partition->size);
        } else {
            solar_os_shell_io_writeln(term, "NVS partition: unknown");
        }
        solar_os_shell_io_printf(
            term,
            "Entries: used %u, free %u, available %u, total %u\n",
            (unsigned)stats.used_entries,
            (unsigned)stats.free_entries,
            (unsigned)stats.available_entries,
            (unsigned)stats.total_entries);
        solar_os_shell_io_printf(
            term, "Namespaces: %u\n", (unsigned)stats.namespace_count);
        return;
    }

    if (strcmp(argv[1], "backup") == 0 ||
        strcmp(argv[1], "restore") == 0) {
        const bool restore = strcmp(argv[1], "restore") == 0;
        const char *operation = restore ? "nvs restore" : "nvs backup";
        if (argc > 3) {
            solar_os_shell_diag_unexpected(
                term,
                operation,
                argv[3],
                restore ? "nvs restore [file]" : "nvs backup [file]");
            return;
        }

        const char *path_arg = argc == 3 ?
            argv[2] : SOLAR_OS_NVS_BACKUP_DEFAULT_PATH;
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        if (!solar_os_shell_resolve_path_for_command(ctx,
                                                     term,
                                                     operation,
                                                     path_arg,
                                                     path,
                                                     sizeof(path))) {
            return;
        }
        if (!restore && argc == 2) {
            char directory[SOLAR_OS_STORAGE_PATH_MAX];
            if (!solar_os_shell_resolve_path_for_command(ctx,
                                                         term,
                                                         operation,
                                                         "/.solar",
                                                         directory,
                                                         sizeof(directory))) {
                return;
            }
            if (solar_os_storage_mkdir(directory) != ESP_OK &&
                errno != EEXIST) {
                solar_os_shell_io_printf(term,
                                         "nvs backup: %s: cannot create directory\n",
                                         directory);
                return;
            }
        }

        solar_os_nvs_backup_result_t result;
        const esp_err_t error = restore ?
            solar_os_nvs_backup_restore(path, &result) :
            solar_os_nvs_backup_create(path, &result);
        if (error == ESP_OK) {
            solar_os_shell_io_printf(
                term,
                restore ?
                    "NVS restored from %s (%u bytes, CRC32 %08" PRIx32 "); rebooting\n" :
                    "NVS backed up to %s (%u bytes, CRC32 %08" PRIx32 ")\n",
                path,
                (unsigned)result.partition_size,
                result.crc32);
            if (!restore) {
                solar_os_shell_io_writeln(
                    term,
                    "Warning: the backup contains unencrypted credentials and settings");
            }
        } else {
            solar_os_shell_io_printf(term,
                                     "%s: %s: %s\n",
                                     operation,
                                     path,
                                     solar_os_shell_error_text(error));
        }
        if (result.reboot_required) {
            solar_os_shell_io_flush(term);
            vTaskDelay(pdMS_TO_TICKS(100));
            solar_os_context_reboot(ctx,
                                    error == ESP_OK ?
                                        "NVS restored" :
                                        (restore ?
                                             "NVS restore recovery" :
                                             "NVS backup recovery"));
        }
        return;
    }

    if (strcmp(argv[1], "clear") != 0) {
        solar_os_shell_diag_subcommand(
            term,
            "nvs",
            argc,
            argv,
            "nvs status|list [namespace]|erase <namespace> [key]|backup [file]|restore [file]|clear",
            nvs_commands,
            sizeof(nvs_commands) / sizeof(nvs_commands[0]));
        return;
    }
    if (argc != 2) {
        solar_os_shell_diag_unexpected(term,
                                       "nvs clear",
                                       argv[2],
                                       "nvs clear");
        return;
    }

    const esp_err_t error = nvs_flash_erase();
    if (error != ESP_OK) {
        (void)nvs_flash_init();
        solar_os_shell_io_printf(
            term, "nvs clear: %s\n", solar_os_shell_error_text(error));
        return;
    }

    solar_os_shell_io_writeln(
        term, "NVS cleared; all saved settings removed; rebooting");
    solar_os_shell_io_flush(term);
    vTaskDelay(pdMS_TO_TICKS(100));
    solar_os_context_reboot(ctx, "NVS cleared");
}

static void mem_print_region(solar_os_shell_io_t *term,
                             const char *label,
                             const solar_os_memory_region_status_t *region)
{
    char total[16];
    char free_now[16];
    char low[16];
    char largest[16];

    format_bytes(region->total, total, sizeof(total));
    format_bytes(region->free, free_now, sizeof(free_now));
    format_bytes(region->minimum_free, low, sizeof(low));
    format_bytes(region->largest_free, largest, sizeof(largest));
    solar_os_shell_io_printf(term,
                             "%s: total %s free %s low %s max %s\n",
                             label,
                             total,
                             free_now,
                             low,
                             largest);
}

#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
static uint64_t engine_us_to_ms(uint64_t us)
{
    return (us + 500ULL) / 1000ULL;
}

static uint64_t engine_util_tenths(const solar_os_engine_stats_t *stats)
{
    if (stats == NULL || stats->since_us == 0) {
        return 0;
    }
    return (stats->busy_us * 1000ULL) / stats->since_us;
}

static void engine_print_stats(solar_os_shell_io_t *term,
                               const solar_os_engine_stats_t *stats)
{
    const uint64_t util = engine_util_tenths(stats);

    solar_os_shell_io_printf(term,
                             "%-8s %-8s %s ops=%" PRIu64
                             " work=%" PRIu64 "ms busy=%" PRIu64
                             "ms util=%" PRIu64 ".%u%% units=%" PRIu64,
                             stats->name,
                             stats->class_name[0] != '\0' ? stats->class_name : "-",
                             stats->active ? "active" : "idle",
                             stats->op_count,
                             engine_us_to_ms(stats->work_us),
                             engine_us_to_ms(stats->busy_us),
                             util / 10ULL,
                             (unsigned)(util % 10ULL),
                             stats->unit_count);
    if (stats->op_count > 0) {
        solar_os_shell_io_printf(term,
                                 " last=%" PRIu64 "us max=%" PRIu64 "us",
                                 stats->last_us,
                                 stats->max_us);
    }
    if (stats->owner[0] != '\0') {
        solar_os_shell_io_printf(term, " owner=%s", stats->owner);
    }
    if (stats->label[0] != '\0') {
        solar_os_shell_io_printf(term, " label=%s", stats->label);
    }
    solar_os_shell_io_put_char(term, '\n');
}

void solar_os_shell_cmd_engine(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc > 2) {
        solar_os_shell_diag_unexpected(term, "engine", argv[2],
                                       "engine [status|list|reset]");
        return;
    }

    const char *action = argc == 2 ? argv[1] : "status";
    if (strcmp(action, "reset") == 0) {
        solar_os_engine_reset_all();
        solar_os_shell_io_writeln(term, "engine counters reset");
        return;
    }
    if (strcmp(action, "status") != 0 && strcmp(action, "list") != 0) {
        solar_os_shell_diag_subcommand(term,
                                       "engine",
                                       argc,
                                       argv,
                                       "engine [status|list|reset]",
                                       engine_commands,
                                       sizeof(engine_commands) / sizeof(engine_commands[0]));
        return;
    }

    const size_t count = solar_os_engine_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "no engines registered");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        solar_os_engine_stats_t stats;
        if (solar_os_engine_get(i, &stats)) {
            engine_print_stats(term, &stats);
        }
    }
}
#endif

void solar_os_shell_cmd_mem(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    solar_os_memory_status_t status;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "policy") != 0)) {
        if (argc > 2) {
            solar_os_shell_diag_unexpected(term, "mem", argv[2], "mem [policy]");
        } else {
            static const char * const values[] = {"policy"};
            solar_os_shell_diag_unknown(term,
                                        "mem",
                                        "argument",
                                        argv[1],
                                        solar_os_shell_suggest(argv[1], values, 1),
                                        "mem [policy]");
        }
        return;
    }

    solar_os_memory_get_status(&status);
    mem_print_region(term, "Internal", &status.internal);
    mem_print_region(term, "PSRAM", &status.external);
    mem_print_region(term, "DMA", &status.dma);

    if (argc == 1) {
        return;
    }

    char reserve[16];
    char fallback_max[16];
    format_bytes(status.internal_reserve, reserve, sizeof(reserve));
    format_bytes(status.internal_fallback_max, fallback_max, sizeof(fallback_max));
    solar_os_shell_io_printf(term,
                             "Policy: internal reserve %s, fallback max %s\n",
                             reserve,
                             fallback_max);

    for (size_t i = 0; i < SOLAR_OS_MEMORY_CLASS_COUNT; i++) {
        const solar_os_memory_class_stats_t *stats = &status.classes[i];
        char requested[16];
        format_bytes(stats->requested_bytes, requested, sizeof(requested));
        solar_os_shell_io_printf(term,
                                 "%-18s req=%" PRIu32 " ok=%" PRIu32
                                 " fail=%" PRIu32 " fallback=%" PRIu32
                                 " bytes=%s\n",
                                 solar_os_memory_class_name((solar_os_memory_class_t)i),
                                 stats->requests,
                                 stats->successes,
                                 stats->failures,
                                 stats->fallbacks,
                                 requested);
    }

    solar_os_task_status_t task_status;
    solar_os_task_get_status(&task_status);
    for (size_t i = 0; i < SOLAR_OS_TASK_ROLE_COUNT; i++) {
        const solar_os_task_role_stats_t *stats = &task_status.roles[i];
        char requested[16];
        format_bytes(stats->requested_stack_bytes, requested, sizeof(requested));
        solar_os_shell_io_printf(term,
                                 "task %-12s req=%" PRIu32 " ok=%" PRIu32
                                 " deny=%" PRIu32 " fail=%" PRIu32 " stack=%s\n",
                                 solar_os_task_role_name((solar_os_task_role_t)i),
                                 stats->requests,
                                 stats->successes,
                                 stats->denied,
                                 stats->failures,
                                 requested);
    }
    solar_os_shell_io_printf(term,
                             "task wait         now=%" PRIu32 " ok=%" PRIu32
                             " cancel=%" PRIu32 "\n",
                             task_status.waiting,
                             task_status.wait_successes,
                             task_status.wait_cancellations);

    if (status.last_failure_valid) {
        char failed_size[16];
        format_bytes(status.last_failure_size, failed_size, sizeof(failed_size));
        solar_os_shell_io_printf(term,
                                 "Last failure: %s tag=%s size=%s\n",
                                 solar_os_memory_class_name(status.last_failure_class),
                                 status.last_failure_tag,
                                 failed_size);
    }
    if (task_status.last_failure_valid) {
        char failed_stack[16];
        format_bytes(task_status.last_failure_stack_bytes,
                     failed_stack,
                     sizeof(failed_stack));
        solar_os_shell_io_printf(term,
                                 "Last task %s: %s role=%s stack=%s\n",
                                 task_status.last_failure_denied ? "denial" : "failure",
                                 task_status.last_failure_name,
                                 solar_os_task_role_name(task_status.last_failure_role),
                                 failed_stack);
    }
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
            solar_os_shell_diag_unexpected(term, "ramfs status", argv[2], "ramfs [status]");
            return;
        }
        ramfs_print_status(term);
        return;
    }

    if (strcmp(argv[1], "mount") == 0) {
        size_t quota = 0;
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "ramfs mount", "<path>",
                                        "ramfs mount <path> <size>");
            return;
        }
        if (argc < 4) {
            solar_os_shell_diag_missing(term, "ramfs mount", "<size>",
                                        "ramfs mount <path> <size>");
            return;
        }
        if (argc > 4) {
            solar_os_shell_diag_unexpected(term, "ramfs mount", argv[4],
                                           "ramfs mount <path> <size>");
            return;
        }
        if (!ramfs_parse_size(argv[3], &quota)) {
            solar_os_shell_diag_invalid(term, "ramfs mount", "size", argv[3],
                                        "a byte count, optionally suffixed K or M",
                                        "ramfs mount <path> <size>", false);
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
            solar_os_shell_io_printf(term, "ramfs mount failed: %s\n", solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "unmount") == 0) {
        if (argc != 3) {
            if (argc < 3) {
                solar_os_shell_diag_missing(term, "ramfs unmount", "<path>",
                                            "ramfs unmount <path>");
            } else {
                solar_os_shell_diag_unexpected(term, "ramfs unmount", argv[3],
                                               "ramfs unmount <path>");
            }
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
            solar_os_shell_io_printf(term, "ramfs unmount failed: %s\n", solar_os_shell_error_text(err));
        }
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "ramfs",
                                   argc,
                                   argv,
                                   "ramfs status|mount|unmount",
                                   ramfs_commands,
                                   sizeof(ramfs_commands) / sizeof(ramfs_commands[0]));
}

void solar_os_shell_cmd_df(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "df", argv[1], "df");
        return;
    }

    bool any = false;
    solar_os_shell_io_writeln(term, "Filesystem  Total    Used     Free     Use% Mount");

    const esp_err_t scan_err = solar_os_storage_rescan();
    if (scan_err != ESP_OK && scan_err != ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_printf(term, "sd         read failed: %s\n", solar_os_shell_error_text(scan_err));
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
                                     solar_os_shell_error_text(err));
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
        solar_os_shell_diag_unexpected(term, "top", argv[1], "top");
        return;
    }

#if (configUSE_TRACE_FACILITY == 1)
    const UBaseType_t task_capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = solar_os_memory_calloc(task_capacity,
                                                 sizeof(TaskStatus_t),
                                                 SOLAR_OS_MEMORY_TRANSIENT,
                                                 "shell.top");
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
        solar_os_memory_free(tasks);
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

    solar_os_memory_free(tasks);
#else
    solar_os_shell_io_writeln(term, "top: FreeRTOS trace facility disabled");
#endif
}
