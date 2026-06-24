#include "solar_os_shell.h"

#include "solar_os_shell_commands.h"
#include "solar_os_shell_io.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_app_registry.h"
#include "solar_os_identity.h"
#include "solar_os_job_registry.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_terminal.h"

#define SHELL_INPUT_MAX 192
#define SHELL_ARG_MAX 20
#define SHELL_PATH_MAX SOLAR_OS_STORAGE_PATH_MAX
#define SHELL_CAT_MAX_BYTES 4096
#define SHELL_HISTORY_LEN 12
#define SHELL_STATE_DIR ".shell"
#define SHELL_HISTORY_FILE "history"
#define SHELL_STARTUP_FILE "startup"
#define SHELL_ALIAS_FILE "alias"
#define SHELL_SCRIPT_MAX_DEPTH 3
#define SHELL_ALIAS_MAX_DEPTH 4
#define SHELL_WATCH_DEFAULT_INTERVAL_MS 2000U
#define SHELL_WATCH_MIN_INTERVAL_MS 1000U
#define SHELL_WATCH_MAX_INTERVAL_MS 86400000U
#define SHELL_LOG_FOLLOW_POLL_MS 250U
#define SHELL_LOG_FOLLOW_BATCH 8
#define SHELL_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define SHELL_COMPLETION_ANY "*"

typedef void (*shell_command_handler_t)(solar_os_context_t *ctx, int argc, char **argv);

typedef struct {
    const char *name;
    const char *summary;
    shell_command_handler_t handler;
} shell_command_t;

typedef struct {
    const char * const *path;
    size_t path_count;
    const char * const *values;
    size_t value_count;
    const char *required_prefix;
    bool complete_commands;
    bool complete_jobs;
    bool complete_ports;
    bool complete_streams;
    bool scalar_streams_only;
    bool complete_path;
    bool dirs_only;
} shell_completion_rule_t;

typedef struct {
    bool show_all;
    bool human;
} shell_ls_options_t;

struct solar_os_shell_session {
    char input[SHELL_INPUT_MAX];
    size_t input_len;
    size_t input_cursor;
    size_t input_row;
    size_t input_col;
    size_t input_view_offset;
    char history[SHELL_HISTORY_LEN][SHELL_INPUT_MAX];
    char history_draft[SHELL_INPUT_MAX];
    char cwd[SHELL_PATH_MAX];
    size_t history_count;
    int history_index;
    bool history_browsing;
    bool previous_key_was_tab;
    bool builtin_suppressed_prompt;
    bool startup_attempted;
    bool watch_active;
    bool watch_executing;
    bool log_follow_active;
    uint8_t script_depth;
    uint8_t alias_depth;
    uint32_t watch_interval_ms;
    uint32_t watch_next_ms;
    uint32_t log_follow_next_ms;
    uint32_t log_follow_last_sequence;
    solar_os_log_level_t log_follow_level;
    const solar_os_app_t *foreground_app;
    char watch_command[SHELL_INPUT_MAX];
    solar_os_shell_io_t io;
};

static EXT_RAM_BSS_ATTR solar_os_shell_session_t shell_display_session;

static void cmd_help(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_cd(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_ls(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_cat(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_sh(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_watch(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_mkdir(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_rm(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_mv(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_cp(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_reboot(solar_os_context_t *ctx, int argc, char **argv);
static bool shell_execute_line(solar_os_context_t *ctx,
                               const char *line,
                               bool add_history,
                               const char *source,
                               size_t line_number);

static const shell_command_t shell_builtin_commands[] = {
    {"help", "show commands", cmd_help},
    {"apps", "list applications", solar_os_shell_cmd_apps},
    {"jobs", "list background jobs", solar_os_shell_cmd_jobs},
    {"job", "control background jobs", solar_os_shell_cmd_job},
    {"version", "show SolarOS version", solar_os_shell_cmd_version},
    {"pkg", "show compiled packages", solar_os_shell_cmd_pkg},
    {"clear", "clear the screen", solar_os_shell_cmd_clear},
    {"sleep", "enter light sleep", solar_os_shell_cmd_sleep},
    {"power", "power profile and sleep policy", solar_os_shell_cmd_power},
    {"watch", "repeat a command", cmd_watch},
    {"setterm", "configure terminal settings", solar_os_shell_cmd_setterm},
    {"status", "show system status", solar_os_shell_cmd_status},
    {"uptime", "show time since boot", solar_os_shell_cmd_uptime},
    {"mem", "show free memory", solar_os_shell_cmd_mem},
    {"stream", "list data streams", solar_os_shell_cmd_stream},
    {"daq", "capture data streams", solar_os_shell_cmd_daq},
    {"log", "show SolarOS logs", solar_os_shell_cmd_log},
    {"port", "show byte-stream ports", solar_os_shell_cmd_port},
    {"df", "show SD card free space", solar_os_shell_cmd_df},
    {"sd", "SD card control", solar_os_shell_cmd_sd},
    {"top", "show task resource usage", solar_os_shell_cmd_top},
    {"battery", "battery status and config", solar_os_shell_cmd_battery},
    {"adc", "read expansion analog inputs", solar_os_shell_cmd_adc},
    {"ble", "BLE keyboard control", solar_os_shell_cmd_ble},
    {"wifi", "Wi-Fi station control", solar_os_shell_cmd_wifi},
#if SOLAR_OS_PACKAGE_NET
    {"mqtt", "MQTT client", solar_os_shell_cmd_mqtt},
    {"ping", "send ICMP echo requests", solar_os_shell_cmd_ping},
    {"netscan", "scan TCP ports", solar_os_shell_cmd_netscan},
#endif
    {"audio", "audio codec tools", solar_os_shell_cmd_audio},
    {"uart", "UART port tools", solar_os_shell_cmd_uart},
    {"i2c", "I2C bus tools", solar_os_shell_cmd_i2c},
    {"gpio", "expansion GPIO tools", solar_os_shell_cmd_gpio},
    {"pwm", "expansion PWM output", solar_os_shell_cmd_pwm},
    {"date", "read or set local date", solar_os_shell_cmd_date},
    {"time", "read or set local time", solar_os_shell_cmd_time},
    {"ntp", "sync RTC from network time", solar_os_shell_cmd_ntp},
    {"ota", "OTA update control", solar_os_shell_cmd_ota},
#if SOLAR_OS_PACKAGE_NET
    {"sshkey", "manage SSH keys", solar_os_shell_cmd_sshkey},
#endif
    {"temperature", "read SHTC3 temperature", solar_os_shell_cmd_temperature},
    {"humidity", "read SHTC3 humidity", solar_os_shell_cmd_humidity},
    {"cd", "change directory", cmd_cd},
    {"ls", "list SD card files", cmd_ls},
    {"cat", "print a small text file", cmd_cat},
    {"sh", "run a shell script", cmd_sh},
    {"mkdir", "create directories", cmd_mkdir},
    {"rm", "remove files or directories", cmd_rm},
    {"mv", "rename or move a file", cmd_mv},
    {"cp", "copy a file", cmd_cp},
    {"reboot", "restart the board", cmd_reboot},
};

static const size_t shell_builtin_command_count =
    sizeof(shell_builtin_commands) / sizeof(shell_builtin_commands[0]);

static const char * const setterm_subcommands[] = {
    "orientation",
    "font",
    "textsize",
    "keyboard",
    "keymap",
    "keyrate",
    "typerate",
    "repeat",
    "timezone",
    "otaurl",
    "ota",
};

static const char * const setterm_orientation_values[] = {"0", "90", "180", "270"};
static const char * const setterm_font_values[] = {"mono", "compact"};
static const char * const setterm_textsize_values[] = {"12", "14", "16", "18", "20"};
static const char * const setterm_keyboard_values[] = {"us", "de"};
static const char * const setterm_keyrate_values[] = {"off"};
static const char * const setterm_timezone_values[] = {"UTC", "Europe/Berlin"};

static const char * const ble_subcommands[] = {
    "status",
    "scan",
    "pair",
    "cancel",
    "forget",
    "gatt",
};

static const char * const ble_gatt_subcommands[] = {
    "status",
    "connect",
    "disconnect",
    "services",
    "chars",
    "read",
    "write",
    "write-nr",
};

static const char * const ble_addr_type_values[] = {
    "public",
    "random",
    "rpa_public",
    "rpa_random",
};

static const char * const wifi_subcommands[] = {
    "status",
    "on",
    "off",
    "ap",
    "scan",
    "connect",
    "disconnect",
    "known",
    "forget",
    "nat",
};

static const char * const wifi_ap_subcommands[] = {"status", "on", "off"};
static const char * const wifi_nat_subcommands[] = {"status", "on", "off"};
static const char * const wifi_ap_auth_values[] = {"open", "wpa", "wpa2", "wpa/wpa2"};
static const char * const wifi_forget_values[] = {"all"};

#if SOLAR_OS_PACKAGE_NET
static const char * const mqtt_subcommands[] = {
    "status",
    "connect",
    "disconnect",
    "publish",
    "subscribe",
};

static const char * const mqtt_qos_values[] = {"0", "1", "2"};
static const char * const mqtt_retain_values[] = {"0", "1", "on", "off", "retain"};
#endif

static const char * const job_subcommands[] = {
    "status",
    "start",
    "stop",
};

static const char * const job_log_values[] = {"file"};
static const char * const ntp_sync_values[] = {"once"};

static const char * const sd_subcommands[] = {
    "status",
    "lsblk",
    "mount",
    "unmount",
};

static const char * const i2c_subcommands[] = {
    "status",
    "speed",
    "scan",
    "probe",
    "read",
    "write",
};

static const char * const uart_subcommands[] = {
    "status",
    "baud",
    "mode",
    "write",
    "read",
};

static const char * const uart_mode_values[] = {"raw", "line"};

static const char * const port_subcommands[] = {
    "list",
    "status",
};

static const char * const log_subcommands[] = {
    "status",
    "show",
    "follow",
    "clear",
    "level",
    "sink",
};

static const char * const log_level_values[] = {"error", "warn", "info", "debug"};
static const char * const log_sink_values[] = {"cdc"};
static const char * const on_off_values[] = {"on", "off"};

static const char * const gpio_subcommands[] = {
    "status",
    "list",
    "mode",
    "read",
    "write",
};

static const char * const expansion_gpio_values[] = {"1", "2", "3", "17"};
static const char * const gpio_mode_values[] = {"in", "out"};
static const char * const gpio_pull_values[] = {"none", "up", "down"};
static const char * const bit_values[] = {"0", "1"};

static const char * const adc_subcommands[] = {
    "status",
    "read",
};

static const char * const pwm_subcommands[] = {
    "status",
    "set",
    "off",
};

static const char * const power_subcommands[] = {
    "status",
    "profile",
    "idle",
    "sleep",
};

static const char * const power_profile_values[] = {
    "performance",
    "balanced",
    "solar",
    "offline",
};
static const char * const power_idle_values[] = {"off"};

static const char * const battery_subcommands[] = {
    "status",
    "config",
    "capacity",
    "min_voltage",
    "max_voltage",
};

static const char * const audio_subcommands[] = {
    "status",
    "tone",
    "level",
    "mic",
    "loopback",
    "off",
};

#if SOLAR_OS_PACKAGE_NET
static const char * const sshkey_subcommands[] = {
    "status",
    "gen",
    "pub",
    "rm",
};

static const char * const sshkey_gen_values[] = {"-f", "2048", "3072", "4096"};
static const char * const sshkey_bits_values[] = {"2048", "3072", "4096"};
#endif

static const char * const ota_subcommands[] = {
    "status",
    "check",
    "upgrade",
    "url",
    "flavor",
    "boot",
};

static const char * const ota_boot_values[] = {"0", "1"};
static const char * const ota_flavor_values[] = {"core", "full"};
static const char * const stream_subcommands[] = {"list", "status"};
static const char * const daq_subcommands[] = {"status", "start", "stop"};
static const char * const daq_options[] = {
    "--rate",
    "--rate-ms",
    "--changes",
    "--append",
    "--replace",
    "--raw",
};
static const char * const watch_subcommands[] = {"-n"};
static const char * const ls_options[] = {"-a", "-h", "--"};
static const char * const rm_options[] = {"-f", "-rf"};
#if SOLAR_OS_PACKAGE_MEDIA
static const char * const view_options[] = {"-fit", "-actual"};
#endif
#if SOLAR_OS_PACKAGE_UTILS
static const char * const plot_options[] = {"-f", "--file", "--rate"};
static const char * const plot_live_options[] = {"--rate"};
#endif

static const char * const path_ls[] = {"ls"};
static const char * const path_rm[] = {"rm"};
#if SOLAR_OS_PACKAGE_MEDIA
static const char * const path_view[] = {"view"};
#endif
#if SOLAR_OS_PACKAGE_UTILS
static const char * const path_plot[] = {"plot"};
static const char * const path_plot_file[] = {"plot", "-f"};
static const char * const path_plot_long_file[] = {"plot", "--file"};
static const char * const path_plot_stream[] = {"plot", SHELL_COMPLETION_ANY};
#endif
static const char * const path_watch[] = {"watch"};
static const char * const path_watch_n_interval[] = {"watch", "-n", SHELL_COMPLETION_ANY};
static const char * const path_setterm[] = {"setterm"};
static const char * const path_setterm_orientation[] = {"setterm", "orientation"};
static const char * const path_setterm_font[] = {"setterm", "font"};
static const char * const path_setterm_textsize[] = {"setterm", "textsize"};
static const char * const path_setterm_keyboard[] = {"setterm", "keyboard"};
static const char * const path_setterm_keymap[] = {"setterm", "keymap"};
static const char * const path_setterm_keyrate[] = {"setterm", "keyrate"};
static const char * const path_setterm_typerate[] = {"setterm", "typerate"};
static const char * const path_setterm_repeat[] = {"setterm", "repeat"};
static const char * const path_setterm_timezone[] = {"setterm", "timezone"};
static const char * const path_job[] = {"job"};
static const char * const path_job_status[] = {"job", "status"};
static const char * const path_job_start[] = {"job", "start"};
static const char * const path_job_start_shell[] = {"job", "start", "shell"};
static const char * const path_job_start_log[] = {"job", "start", "log"};
static const char * const path_job_start_log_port[] = {"job", "start", "log", SHELL_COMPLETION_ANY};
static const char * const path_job_start_log_file[] = {"job", "start", "log", "file"};
static const char * const path_job_start_bridge[] = {"job", "start", "bridge"};
static const char * const path_job_start_bridge_port[] = {"job", "start", "bridge", SHELL_COMPLETION_ANY};
static const char * const path_job_start_httpd[] = {"job", "start", "httpd"};
static const char * const path_job_start_ntp_sync[] = {"job", "start", "ntp-sync"};
static const char * const path_job_start_slip[] = {"job", "start", "slip"};
static const char * const path_job_start_daq[] = {"job", "start", "daq"};
static const char * const path_job_start_daq_stream[] = {"job", "start", "daq", SHELL_COMPLETION_ANY};
static const char * const path_job_start_daq_stream_file[] = {
    "job",
    "start",
    "daq",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_job_stop[] = {"job", "stop"};
static const char * const path_stream[] = {"stream"};
static const char * const path_stream_status[] = {"stream", "status"};
static const char * const path_daq[] = {"daq"};
static const char * const path_daq_start[] = {"daq", "start"};
static const char * const path_daq_start_stream[] = {"daq", "start", SHELL_COMPLETION_ANY};
static const char * const path_daq_start_stream_file[] = {
    "daq",
    "start",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_ble[] = {"ble"};
static const char * const path_ble_gatt[] = {"ble", "gatt"};
static const char * const path_ble_gatt_connect_addr[] = {"ble", "gatt", "connect", SHELL_COMPLETION_ANY};
static const char * const path_wifi[] = {"wifi"};
static const char * const path_wifi_ap[] = {"wifi", "ap"};
static const char * const path_wifi_ap_on_auth[] = {
    "wifi",
    "ap",
    "on",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_wifi_nat[] = {"wifi", "nat"};
static const char * const path_wifi_forget[] = {"wifi", "forget"};
#if SOLAR_OS_PACKAGE_NET
static const char * const path_mqtt[] = {"mqtt"};
static const char * const path_mqtt_publish_payload[] = {
    "mqtt",
    "publish",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_mqtt_publish_qos[] = {
    "mqtt",
    "publish",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_mqtt_subscribe_topic[] = {
    "mqtt",
    "subscribe",
    SHELL_COMPLETION_ANY,
};
#endif
static const char * const path_sd[] = {"sd"};
static const char * const path_i2c[] = {"i2c"};
static const char * const path_uart[] = {"uart"};
static const char * const path_uart_mode[] = {"uart", "mode"};
static const char * const path_port[] = {"port"};
static const char * const path_port_status[] = {"port", "status"};
static const char * const path_log[] = {"log"};
static const char * const path_log_follow[] = {"log", "follow"};
static const char * const path_log_level[] = {"log", "level"};
static const char * const path_log_sink[] = {"log", "sink"};
static const char * const path_log_sink_cdc[] = {"log", "sink", "cdc"};
static const char * const path_gpio[] = {"gpio"};
static const char * const path_gpio_mode[] = {"gpio", "mode"};
static const char * const path_gpio_mode_pin[] = {"gpio", "mode", SHELL_COMPLETION_ANY};
static const char * const path_gpio_mode_pin_mode[] = {
    "gpio",
    "mode",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_gpio_read[] = {"gpio", "read"};
static const char * const path_gpio_write[] = {"gpio", "write"};
static const char * const path_gpio_write_pin[] = {"gpio", "write", SHELL_COMPLETION_ANY};
static const char * const path_adc[] = {"adc"};
static const char * const path_adc_read[] = {"adc", "read"};
static const char * const path_pwm[] = {"pwm"};
static const char * const path_pwm_set[] = {"pwm", "set"};
static const char * const path_pwm_off[] = {"pwm", "off"};
static const char * const path_power[] = {"power"};
static const char * const path_power_profile[] = {"power", "profile"};
static const char * const path_power_idle[] = {"power", "idle"};
static const char * const path_battery[] = {"battery"};
static const char * const path_audio[] = {"audio"};
#if SOLAR_OS_PACKAGE_NET
static const char * const path_sshkey[] = {"sshkey"};
static const char * const path_sshkey_gen[] = {"sshkey", "gen"};
static const char * const path_sshkey_gen_force[] = {"sshkey", "gen", "-f"};
#endif
static const char * const path_ota[] = {"ota"};
static const char * const path_ota_boot[] = {"ota", "boot"};
static const char * const path_ota_flavor[] = {"ota", "flavor"};

#define SHELL_COMPLETION_STATIC(path_array, value_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .values = value_array, \
        .value_count = SHELL_ARRAY_COUNT(value_array), \
    }
#define SHELL_COMPLETION_OPTIONS(path_array, value_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .values = value_array, \
        .value_count = SHELL_ARRAY_COUNT(value_array), \
        .required_prefix = "-", \
    }
#define SHELL_COMPLETION_COMMANDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .complete_commands = true, \
    }
#define SHELL_COMPLETION_JOBS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .complete_jobs = true, \
    }
#define SHELL_COMPLETION_PORTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .complete_ports = true, \
    }
#define SHELL_COMPLETION_STREAMS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .complete_streams = true, \
    }
#define SHELL_COMPLETION_SCALAR_STREAMS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .complete_streams = true, \
        .scalar_streams_only = true, \
    }
#define SHELL_COMPLETION_PATH(path_array, only_dirs) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .complete_path = true, \
        .dirs_only = only_dirs, \
    }

static const shell_completion_rule_t shell_completion_rules[] = {
    SHELL_COMPLETION_OPTIONS(path_ls, ls_options),
    SHELL_COMPLETION_OPTIONS(path_rm, rm_options),
#if SOLAR_OS_PACKAGE_MEDIA
    SHELL_COMPLETION_OPTIONS(path_view, view_options),
#endif
#if SOLAR_OS_PACKAGE_UTILS
    SHELL_COMPLETION_OPTIONS(path_plot, plot_options),
    SHELL_COMPLETION_SCALAR_STREAMS(path_plot),
    SHELL_COMPLETION_OPTIONS(path_plot_stream, plot_live_options),
    SHELL_COMPLETION_SCALAR_STREAMS(path_plot_stream),
    SHELL_COMPLETION_PATH(path_plot_file, false),
    SHELL_COMPLETION_PATH(path_plot_long_file, false),
#endif
    SHELL_COMPLETION_STATIC(path_watch, watch_subcommands),
    SHELL_COMPLETION_COMMANDS(path_watch),
    SHELL_COMPLETION_COMMANDS(path_watch_n_interval),
    SHELL_COMPLETION_STATIC(path_setterm, setterm_subcommands),
    SHELL_COMPLETION_STATIC(path_setterm_orientation, setterm_orientation_values),
    SHELL_COMPLETION_STATIC(path_setterm_font, setterm_font_values),
    SHELL_COMPLETION_STATIC(path_setterm_textsize, setterm_textsize_values),
    SHELL_COMPLETION_STATIC(path_setterm_keyboard, setterm_keyboard_values),
    SHELL_COMPLETION_STATIC(path_setterm_keymap, setterm_keyboard_values),
    SHELL_COMPLETION_STATIC(path_setterm_keyrate, setterm_keyrate_values),
    SHELL_COMPLETION_STATIC(path_setterm_typerate, setterm_keyrate_values),
    SHELL_COMPLETION_STATIC(path_setterm_repeat, setterm_keyrate_values),
    SHELL_COMPLETION_STATIC(path_setterm_timezone, setterm_timezone_values),
    SHELL_COMPLETION_STATIC(path_job, job_subcommands),
    SHELL_COMPLETION_JOBS(path_job_status),
    SHELL_COMPLETION_JOBS(path_job_start),
    SHELL_COMPLETION_PORTS(path_job_start_shell),
    SHELL_COMPLETION_STATIC(path_job_start_log, job_log_values),
    SHELL_COMPLETION_PORTS(path_job_start_log),
    SHELL_COMPLETION_STATIC(path_job_start_log_port, log_level_values),
    SHELL_COMPLETION_PATH(path_job_start_log_file, false),
    SHELL_COMPLETION_PORTS(path_job_start_bridge),
    SHELL_COMPLETION_PORTS(path_job_start_bridge_port),
    SHELL_COMPLETION_PATH(path_job_start_httpd, true),
    SHELL_COMPLETION_STATIC(path_job_start_ntp_sync, ntp_sync_values),
    SHELL_COMPLETION_PORTS(path_job_start_slip),
    SHELL_COMPLETION_STREAMS(path_job_start_daq),
    SHELL_COMPLETION_PATH(path_job_start_daq, false),
    SHELL_COMPLETION_STREAMS(path_job_start_daq_stream),
    SHELL_COMPLETION_PATH(path_job_start_daq_stream, false),
    SHELL_COMPLETION_STREAMS(path_job_start_daq_stream_file),
    SHELL_COMPLETION_PATH(path_job_start_daq_stream_file, false),
    SHELL_COMPLETION_STATIC(path_job_start_daq_stream_file, daq_options),
    SHELL_COMPLETION_JOBS(path_job_stop),
    SHELL_COMPLETION_STATIC(path_stream, stream_subcommands),
    SHELL_COMPLETION_STREAMS(path_stream_status),
    SHELL_COMPLETION_STATIC(path_daq, daq_subcommands),
    SHELL_COMPLETION_STREAMS(path_daq_start),
    SHELL_COMPLETION_PATH(path_daq_start, false),
    SHELL_COMPLETION_STREAMS(path_daq_start_stream),
    SHELL_COMPLETION_PATH(path_daq_start_stream, false),
    SHELL_COMPLETION_STREAMS(path_daq_start_stream_file),
    SHELL_COMPLETION_PATH(path_daq_start_stream_file, false),
    SHELL_COMPLETION_STATIC(path_daq_start_stream_file, daq_options),
    SHELL_COMPLETION_STATIC(path_ble, ble_subcommands),
    SHELL_COMPLETION_STATIC(path_ble_gatt, ble_gatt_subcommands),
    SHELL_COMPLETION_STATIC(path_ble_gatt_connect_addr, ble_addr_type_values),
    SHELL_COMPLETION_STATIC(path_wifi, wifi_subcommands),
    SHELL_COMPLETION_STATIC(path_wifi_ap, wifi_ap_subcommands),
    SHELL_COMPLETION_STATIC(path_wifi_ap_on_auth, wifi_ap_auth_values),
    SHELL_COMPLETION_STATIC(path_wifi_nat, wifi_nat_subcommands),
    SHELL_COMPLETION_STATIC(path_wifi_forget, wifi_forget_values),
#if SOLAR_OS_PACKAGE_NET
    SHELL_COMPLETION_STATIC(path_mqtt, mqtt_subcommands),
    SHELL_COMPLETION_STATIC(path_mqtt_publish_payload, mqtt_qos_values),
    SHELL_COMPLETION_STATIC(path_mqtt_publish_qos, mqtt_retain_values),
    SHELL_COMPLETION_STATIC(path_mqtt_subscribe_topic, mqtt_qos_values),
#endif
    SHELL_COMPLETION_STATIC(path_sd, sd_subcommands),
    SHELL_COMPLETION_STATIC(path_i2c, i2c_subcommands),
    SHELL_COMPLETION_STATIC(path_uart, uart_subcommands),
    SHELL_COMPLETION_STATIC(path_uart_mode, uart_mode_values),
    SHELL_COMPLETION_STATIC(path_port, port_subcommands),
    SHELL_COMPLETION_PORTS(path_port_status),
    SHELL_COMPLETION_STATIC(path_log, log_subcommands),
    SHELL_COMPLETION_STATIC(path_log_follow, log_level_values),
    SHELL_COMPLETION_STATIC(path_log_level, log_level_values),
    SHELL_COMPLETION_STATIC(path_log_sink, log_sink_values),
    SHELL_COMPLETION_STATIC(path_log_sink_cdc, on_off_values),
    SHELL_COMPLETION_STATIC(path_gpio, gpio_subcommands),
    SHELL_COMPLETION_STATIC(path_gpio_mode, expansion_gpio_values),
    SHELL_COMPLETION_STATIC(path_gpio_mode_pin, gpio_mode_values),
    SHELL_COMPLETION_STATIC(path_gpio_mode_pin_mode, gpio_pull_values),
    SHELL_COMPLETION_STATIC(path_gpio_read, expansion_gpio_values),
    SHELL_COMPLETION_STATIC(path_gpio_write, expansion_gpio_values),
    SHELL_COMPLETION_STATIC(path_gpio_write_pin, bit_values),
    SHELL_COMPLETION_STATIC(path_adc, adc_subcommands),
    SHELL_COMPLETION_STATIC(path_adc_read, expansion_gpio_values),
    SHELL_COMPLETION_STATIC(path_pwm, pwm_subcommands),
    SHELL_COMPLETION_STATIC(path_pwm_set, expansion_gpio_values),
    SHELL_COMPLETION_STATIC(path_pwm_off, expansion_gpio_values),
    SHELL_COMPLETION_STATIC(path_power, power_subcommands),
    SHELL_COMPLETION_STATIC(path_power_profile, power_profile_values),
    SHELL_COMPLETION_STATIC(path_power_idle, power_idle_values),
    SHELL_COMPLETION_STATIC(path_battery, battery_subcommands),
    SHELL_COMPLETION_STATIC(path_audio, audio_subcommands),
#if SOLAR_OS_PACKAGE_NET
    SHELL_COMPLETION_STATIC(path_sshkey, sshkey_subcommands),
    SHELL_COMPLETION_STATIC(path_sshkey_gen, sshkey_gen_values),
    SHELL_COMPLETION_STATIC(path_sshkey_gen_force, sshkey_bits_values),
#endif
    SHELL_COMPLETION_STATIC(path_ota, ota_subcommands),
    SHELL_COMPLETION_STATIC(path_ota_boot, ota_boot_values),
    SHELL_COMPLETION_STATIC(path_ota_flavor, ota_flavor_values),
};

#undef SHELL_COMPLETION_PATH
#undef SHELL_COMPLETION_PORTS
#undef SHELL_COMPLETION_STREAMS
#undef SHELL_COMPLETION_JOBS
#undef SHELL_COMPLETION_COMMANDS
#undef SHELL_COMPLETION_OPTIONS
#undef SHELL_COMPLETION_STATIC

static solar_os_shell_session_t *shell_session(solar_os_context_t *ctx)
{
    solar_os_shell_session_t *session = solar_os_context_shell_session(ctx);
    if (session == NULL) {
        session = &shell_display_session;
        solar_os_context_set_shell_session(ctx, session);
    }
    return session;
}

static solar_os_shell_io_t *shell_io(solar_os_context_t *ctx)
{
    solar_os_shell_session_t *session = shell_session(ctx);
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&session->io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &session->io);
        io = &session->io;
    }
    return io;
}

static void cmd_help(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = shell_io(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(io, "usage: help");
        return;
    }

    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        solar_os_shell_io_write_bold(io, shell_builtin_commands[i].name);
        solar_os_shell_io_printf(io, " - %s\n", shell_builtin_commands[i].summary);
    }
}

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return shell_io(ctx);
}

static bool shell_is_printable_char(char ch)
{
    const unsigned char value = (unsigned char)ch;

    return isprint(value) || value >= 0xa0;
}

static void shell_reset_cwd(solar_os_shell_session_t *session)
{
    if (session != NULL) {
        strlcpy(session->cwd, solar_os_storage_mount_point(), sizeof(session->cwd));
    }
}

static bool shell_path_has_storage_prefix(const char *path)
{
    return solar_os_storage_path_has_mount_prefix(path);
}

static esp_err_t resolve_path(solar_os_context_t *ctx, const char *arg, char *path, size_t path_len)
{
    solar_os_shell_session_t *session = shell_session(ctx);

    if (!shell_path_has_storage_prefix(session->cwd)) {
        shell_reset_cwd(session);
    }

    return solar_os_storage_resolve_path_at(session->cwd, arg, path, path_len);
}

static bool resolve_path_for_command(solar_os_context_t *ctx,
                                     solar_os_shell_io_t *term,
                                     const char *command,
                                     const char *arg,
                                     char *path,
                                     size_t path_len)
{
    const esp_err_t err = resolve_path(ctx, arg, path, path_len);
    if (err == ESP_OK) {
        return true;
    }

    const char *reason = err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path";
    solar_os_shell_io_printf(term,
                             "%s: %s: %s\n",
                             command,
                             reason,
                             arg != NULL && arg[0] != '\0' ? arg : "/");
    return false;
}

static void shell_format_display_path(const char *path, char *display, size_t display_len)
{
    char root[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    if (solar_os_storage_path_mount_point(path, root, sizeof(root)) != ESP_OK) {
        strlcpy(root, solar_os_storage_mount_point(), sizeof(root));
    }
    const size_t root_len = strlen(root);

    if (display == NULL || display_len == 0) {
        return;
    }

    if (!shell_path_has_storage_prefix(path)) {
        strlcpy(display, "/", display_len);
        return;
    }

    const char *relative = path + root_len;
    while (*relative == '/') {
        relative++;
    }

    if (*relative == '\0') {
        if (strcmp(root, solar_os_storage_mount_point()) == 0) {
            strlcpy(display, "/", display_len);
        } else {
            snprintf(display, display_len, "%s/", root);
        }
    } else {
        if (strcmp(root, solar_os_storage_mount_point()) == 0) {
            snprintf(display, display_len, "/%s/", relative);
        } else {
            snprintf(display, display_len, "%s/%s/", root, relative);
        }
    }
}

static void shell_prompt(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = shell_io(ctx);
    char identity[SOLAR_OS_IDENTITY_USER_MAX + SOLAR_OS_IDENTITY_HOSTNAME_MAX + 2];
    char display_path[SHELL_PATH_MAX];
    char prompt[SHELL_PATH_MAX + sizeof(identity) + 4];

    shell_session(ctx)->input_len = 0;
    shell_session(ctx)->input_cursor = 0;
    shell_session(ctx)->input_view_offset = 0;
    shell_session(ctx)->input[0] = '\0';
    shell_session(ctx)->history_index = -1;
    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->previous_key_was_tab = false;

    if (!shell_path_has_storage_prefix(shell_session(ctx)->cwd)) {
        shell_reset_cwd(shell_session(ctx));
    }
    shell_format_display_path(shell_session(ctx)->cwd, display_path, sizeof(display_path));
    solar_os_identity_format(identity, sizeof(identity));
    snprintf(prompt, sizeof(prompt), "%s:%s ", identity, display_path);
    solar_os_shell_io_write_bold(io, prompt);
    shell_session(ctx)->input_row = solar_os_shell_io_cursor_row(io);
    shell_session(ctx)->input_col = solar_os_shell_io_cursor_col(io);
}

static int tokenize(char *line, char **argv, int argv_max)
{
    int argc = 0;

    if (line == NULL || argv == NULL || argv_max <= 0) {
        return 0;
    }

    char *read = line;
    char *write = line;
    while (*read != '\0') {
        while (isspace((unsigned char)*read)) {
            read++;
        }
        if (*read == '\0') {
            break;
        }
        if (argc >= argv_max) {
            break;
        }

        argv[argc++] = write;
        char quote = '\0';
        while (*read != '\0') {
            const char ch = *read;
            if (quote == '\0' && isspace((unsigned char)ch)) {
                break;
            }
            if ((ch == '"' || ch == '\'') && (quote == '\0' || quote == ch)) {
                quote = quote == '\0' ? ch : '\0';
                read++;
                continue;
            }
            if (ch == '\\' && quote != '\'' && read[1] != '\0') {
                read++;
                *write++ = *read++;
                continue;
            }

            *write++ = *read++;
        }
        if (isspace((unsigned char)*read)) {
            read++;
        }
        *write++ = '\0';
    }

    return argc;
}

static char *shell_trim_line(char *line)
{
    if (line == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*line)) {
        line++;
    }

    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return line;
}

static size_t shell_max_input_len(solar_os_context_t *ctx)
{
    (void)ctx;

    return sizeof(shell_session(ctx)->input) - 1;
}

static size_t shell_visible_input_cols(solar_os_context_t *ctx)
{
    const size_t cols = solar_os_shell_io_cols(shell_io(ctx));

    return cols > shell_session(ctx)->input_col ? cols - shell_session(ctx)->input_col : 1;
}

static void shell_ensure_cursor_visible(solar_os_context_t *ctx)
{
    const size_t visible_cols = shell_visible_input_cols(ctx);

    if (shell_session(ctx)->input_cursor < shell_session(ctx)->input_view_offset) {
        shell_session(ctx)->input_view_offset = shell_session(ctx)->input_cursor;
    } else if (shell_session(ctx)->input_cursor >= shell_session(ctx)->input_view_offset + visible_cols) {
        shell_session(ctx)->input_view_offset = shell_session(ctx)->input_cursor - visible_cols + 1;
    }
}

static void shell_render_input(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = shell_io(ctx);
    const size_t visible_cols = shell_visible_input_cols(ctx);
    size_t written = 0;

    shell_ensure_cursor_visible(ctx);
    size_t cursor_col = shell_session(ctx)->input_cursor - shell_session(ctx)->input_view_offset;
    if (cursor_col >= visible_cols) {
        cursor_col = visible_cols - 1;
    }
    solar_os_shell_io_clear_line_from(io, shell_session(ctx)->input_row, shell_session(ctx)->input_col);
    solar_os_shell_io_set_cursor(io, shell_session(ctx)->input_row, shell_session(ctx)->input_col);
    for (size_t i = shell_session(ctx)->input_view_offset;
         i < shell_session(ctx)->input_len && written < visible_cols;
         i++, written++) {
        solar_os_shell_io_put_char(io, shell_session(ctx)->input[i]);
    }
    solar_os_shell_io_set_cursor(io, shell_session(ctx)->input_row, shell_session(ctx)->input_col + cursor_col);
}

static void shell_replace_input(solar_os_context_t *ctx, const char *text)
{
    const size_t max_len = shell_max_input_len(ctx);
    size_t copy_len = 0;

    if (text != NULL) {
        copy_len = strnlen(text, max_len);
        memcpy(shell_session(ctx)->input, text, copy_len);
    }

    shell_session(ctx)->input[copy_len] = '\0';
    shell_session(ctx)->input_len = copy_len;
    shell_session(ctx)->input_cursor = copy_len;
    shell_session(ctx)->input_view_offset = 0;
    shell_render_input(ctx);
}

static void shell_move_cursor_left(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->input_cursor == 0) {
        return;
    }

    shell_session(ctx)->input_cursor--;
    shell_render_input(ctx);
}

static void shell_move_cursor_right(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->input_cursor >= shell_session(ctx)->input_len) {
        return;
    }

    shell_session(ctx)->input_cursor++;
    shell_render_input(ctx);
}

static void shell_move_cursor_home(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->input_cursor == 0) {
        return;
    }

    shell_session(ctx)->input_cursor = 0;
    shell_render_input(ctx);
}

static void shell_move_cursor_end(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->input_cursor >= shell_session(ctx)->input_len) {
        return;
    }

    shell_session(ctx)->input_cursor = shell_session(ctx)->input_len;
    shell_render_input(ctx);
}

static bool shell_word_char(char ch)
{
    const unsigned char value = (unsigned char)ch;

    return isalnum(value) || value >= 0xa0 || ch == '_' || ch == '-' || ch == '/' || ch == '.';
}

static void shell_move_cursor_word_left(solar_os_context_t *ctx)
{
    size_t cursor = shell_session(ctx)->input_cursor;

    while (cursor > 0 && !shell_word_char(shell_session(ctx)->input[cursor - 1])) {
        cursor--;
    }
    while (cursor > 0 && shell_word_char(shell_session(ctx)->input[cursor - 1])) {
        cursor--;
    }

    if (cursor != shell_session(ctx)->input_cursor) {
        shell_session(ctx)->input_cursor = cursor;
        shell_render_input(ctx);
    }
}

static void shell_move_cursor_word_right(solar_os_context_t *ctx)
{
    size_t cursor = shell_session(ctx)->input_cursor;

    while (cursor < shell_session(ctx)->input_len && shell_word_char(shell_session(ctx)->input[cursor])) {
        cursor++;
    }
    while (cursor < shell_session(ctx)->input_len && !shell_word_char(shell_session(ctx)->input[cursor])) {
        cursor++;
    }

    if (cursor != shell_session(ctx)->input_cursor) {
        shell_session(ctx)->input_cursor = cursor;
        shell_render_input(ctx);
    }
}

static void shell_insert_char(solar_os_context_t *ctx, char ch)
{
    if (shell_session(ctx)->input_len >= shell_max_input_len(ctx)) {
        return;
    }

    memmove(&shell_session(ctx)->input[shell_session(ctx)->input_cursor + 1],
            &shell_session(ctx)->input[shell_session(ctx)->input_cursor],
            shell_session(ctx)->input_len - shell_session(ctx)->input_cursor + 1);
    shell_session(ctx)->input[shell_session(ctx)->input_cursor++] = ch;
    shell_session(ctx)->input_len++;
    shell_render_input(ctx);
}

static void shell_backspace(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->input_cursor == 0) {
        return;
    }

    memmove(&shell_session(ctx)->input[shell_session(ctx)->input_cursor - 1],
            &shell_session(ctx)->input[shell_session(ctx)->input_cursor],
            shell_session(ctx)->input_len - shell_session(ctx)->input_cursor + 1);
    shell_session(ctx)->input_cursor--;
    shell_session(ctx)->input_len--;
    shell_render_input(ctx);
}

static bool shell_make_state_path(char *path, size_t path_len, const char *leaf)
{
    int written = 0;

    if (path == NULL || path_len == 0) {
        return false;
    }

    if (leaf == NULL || leaf[0] == '\0') {
        written = snprintf(path, path_len, "%s/%s", solar_os_storage_mount_point(), SHELL_STATE_DIR);
    } else {
        written = snprintf(path,
                           path_len,
                           "%s/%s/%s",
                           solar_os_storage_mount_point(),
                           SHELL_STATE_DIR,
                           leaf);
    }

    return written >= 0 && (size_t)written < path_len;
}

static bool shell_directory_exists(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    closedir(dir);
    return true;
}

static bool shell_ensure_state_dir(void)
{
    char dir_path[SHELL_PATH_MAX];

    if (!solar_os_storage_is_mounted() ||
        !shell_make_state_path(dir_path, sizeof(dir_path), NULL)) {
        return false;
    }

    if (shell_directory_exists(dir_path)) {
        return true;
    }

    if (solar_os_storage_mkdir(dir_path) == ESP_OK) {
        return true;
    }

    return errno == EEXIST && shell_directory_exists(dir_path);
}

static bool shell_history_add_ram(solar_os_shell_session_t *session, const char *line)
{
    if (session == NULL || line == NULL || line[0] == '\0') {
        return false;
    }

    if (session->history_count > 0 &&
        strcmp(session->history[session->history_count - 1], line) == 0) {
        return false;
    }

    if (session->history_count < SHELL_HISTORY_LEN) {
        strlcpy(session->history[session->history_count++], line, sizeof(session->history[0]));
        return true;
    }

    memmove(session->history[0],
            session->history[1],
            sizeof(session->history[0]) * (SHELL_HISTORY_LEN - 1));
    strlcpy(session->history[SHELL_HISTORY_LEN - 1], line, sizeof(session->history[0]));
    return true;
}

static void shell_history_save(solar_os_shell_session_t *session)
{
    char path[SHELL_PATH_MAX];

    if (session == NULL) {
        return;
    }

    if (!shell_ensure_state_dir() ||
        !shell_make_state_path(path, sizeof(path), SHELL_HISTORY_FILE)) {
        return;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return;
    }

    for (size_t i = 0; i < session->history_count; i++) {
        for (const char *p = session->history[i]; *p != '\0'; p++) {
            if (*p != '\r' && *p != '\n') {
                fputc((unsigned char)*p, file);
            }
        }
        fputc('\n', file);
    }

    fclose(file);
}

static void shell_history_load(solar_os_shell_session_t *session)
{
    char path[SHELL_PATH_MAX];
    char line[SHELL_INPUT_MAX];
    size_t entries_seen = 0;
    bool should_rewrite = false;

    if (session == NULL ||
        !solar_os_storage_is_mounted() ||
        !shell_make_state_path(path, sizeof(path), SHELL_HISTORY_FILE)) {
        return;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        const bool complete_line = strchr(line, '\n') != NULL;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0') {
            entries_seen++;
            if (entries_seen > SHELL_HISTORY_LEN || !complete_line) {
                should_rewrite = true;
            }
            shell_history_add_ram(session, line);
        }

        if (!complete_line) {
            int ch = 0;
            do {
                ch = fgetc(file);
            } while (ch != EOF && ch != '\n');
        }
    }

    fclose(file);
    if (should_rewrite) {
        shell_history_save(session);
    }
}

static void shell_history_add(solar_os_context_t *ctx, const char *line)
{
    solar_os_shell_session_t *session = shell_session(ctx);

    if (shell_history_add_ram(session, line)) {
        shell_history_save(session);
    }
}

static void shell_history_previous(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->history_count == 0) {
        return;
    }

    if (!shell_session(ctx)->history_browsing) {
        strlcpy(shell_session(ctx)->history_draft, shell_session(ctx)->input, sizeof(shell_session(ctx)->history_draft));
        shell_session(ctx)->history_index = (int)shell_session(ctx)->history_count - 1;
        shell_session(ctx)->history_browsing = true;
    } else if (shell_session(ctx)->history_index > 0) {
        shell_session(ctx)->history_index--;
    }

    shell_replace_input(ctx, shell_session(ctx)->history[shell_session(ctx)->history_index]);
}

static void shell_history_next(solar_os_context_t *ctx)
{
    if (!shell_session(ctx)->history_browsing) {
        return;
    }

    if (shell_session(ctx)->history_index + 1 < (int)shell_session(ctx)->history_count) {
        shell_session(ctx)->history_index++;
        shell_replace_input(ctx, shell_session(ctx)->history[shell_session(ctx)->history_index]);
        return;
    }

    shell_session(ctx)->history_index = -1;
    shell_session(ctx)->history_browsing = false;
    shell_replace_input(ctx, shell_session(ctx)->history_draft);
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

typedef bool (*shell_alias_callback_t)(const char *name, int argc, char **argv, void *user);

static bool shell_alias_path(char *path, size_t path_len)
{
    return solar_os_storage_is_mounted() &&
        shell_make_state_path(path, path_len, SHELL_ALIAS_FILE);
}

static void shell_discard_file_line(FILE *file)
{
    int ch = 0;

    do {
        ch = fgetc(file);
    } while (ch != EOF && ch != '\n');
}

static void shell_alias_ensure_file(void)
{
    char path[SHELL_PATH_MAX];

    if (!shell_ensure_state_dir() || !shell_alias_path(path, sizeof(path))) {
        return;
    }

    FILE *file = fopen(path, "a");
    if (file != NULL) {
        fclose(file);
    }
}

static bool shell_for_each_alias(shell_alias_callback_t callback, void *user)
{
    char path[SHELL_PATH_MAX];
    char line[SHELL_INPUT_MAX + 1];

    if (callback == NULL || !shell_alias_path(path, sizeof(path))) {
        return false;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        const bool complete_line = strchr(line, '\n') != NULL;
        if (!complete_line && !feof(file)) {
            shell_discard_file_line(file);
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';
        char *alias_line = shell_trim_line(line);
        if (alias_line == NULL || alias_line[0] == '\0' || alias_line[0] == '#') {
            continue;
        }

        char *alias_argv[SHELL_ARG_MAX];
        const int alias_argc = tokenize(alias_line, alias_argv, SHELL_ARG_MAX);
        if (alias_argc < 2) {
            continue;
        }

        if (!callback(alias_argv[0], alias_argc, alias_argv, user)) {
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return true;
}

static bool shell_append_token(char *line, size_t line_len, const char *token)
{
    const size_t used = strlen(line);
    const size_t token_len = token != NULL ? strlen(token) : 0;
    const size_t space = used > 0 ? 1 : 0;

    if (token == NULL || token[0] == '\0') {
        return false;
    }

    bool needs_quotes = false;
    size_t escaped_len = token_len;
    for (const char *p = token; *p != '\0'; p++) {
        if (isspace((unsigned char)*p)) {
            needs_quotes = true;
        }
        if (*p == '"' || *p == '\\') {
            needs_quotes = true;
            escaped_len++;
        }
    }

    const size_t append_len = needs_quotes ? escaped_len + 2 : token_len;
    if (used + space + append_len + 1 > line_len) {
        return false;
    }

    char *out = &line[used];
    if (space != 0) {
        *out++ = ' ';
    }

    if (!needs_quotes) {
        memcpy(out, token, token_len + 1);
        return true;
    }

    *out++ = '"';
    for (const char *p = token; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            *out++ = '\\';
        }
        *out++ = *p;
    }
    *out++ = '"';
    *out = '\0';
    return true;
}

typedef struct {
    const char *name;
    int user_argc;
    char **user_argv;
    bool found;
    bool too_long;
    char expanded[SHELL_INPUT_MAX];
} shell_alias_expand_t;

static bool shell_alias_expand_callback(const char *name, int argc, char **argv, void *user)
{
    shell_alias_expand_t *expand = (shell_alias_expand_t *)user;

    if (strcmp(name, expand->name) != 0) {
        return true;
    }

    expand->found = true;
    expand->expanded[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if (!shell_append_token(expand->expanded, sizeof(expand->expanded), argv[i])) {
            expand->too_long = true;
            return false;
        }
    }

    for (int i = 1; i < expand->user_argc; i++) {
        if (!shell_append_token(expand->expanded,
                                sizeof(expand->expanded),
                                expand->user_argv[i])) {
            expand->too_long = true;
            return false;
        }
    }

    return false;
}

static bool shell_try_alias(solar_os_context_t *ctx,
                            int argc,
                            char **argv,
                            const char *source,
                            size_t line_number,
                            bool *matched)
{
    solar_os_shell_io_t *term = terminal(ctx);
    shell_alias_expand_t expand = {
        .name = argv[0],
        .user_argc = argc,
        .user_argv = argv,
    };

    if (matched != NULL) {
        *matched = false;
    }

    (void)shell_for_each_alias(shell_alias_expand_callback, &expand);
    if (!expand.found) {
        return true;
    }

    if (matched != NULL) {
        *matched = true;
    }

    if (shell_session(ctx)->alias_depth >= SHELL_ALIAS_MAX_DEPTH) {
        solar_os_shell_io_printf(term, "alias: nesting too deep: %s\n", argv[0]);
        return true;
    }
    if (expand.too_long) {
        solar_os_shell_io_printf(term, "alias: expansion too long: %s\n", argv[0]);
        return true;
    }

    shell_session(ctx)->alias_depth++;
    const bool should_prompt =
        shell_execute_line(ctx, expand.expanded, false, source, line_number);
    shell_session(ctx)->alias_depth--;
    return should_prompt;
}

typedef struct {
    const char *prefix;
    solar_os_shell_io_t *io;
} shell_alias_print_t;

static bool shell_alias_print_callback(const char *name, int argc, char **argv, void *user)
{
    shell_alias_print_t *print = (shell_alias_print_t *)user;

    (void)argc;
    (void)argv;

    if (print->prefix == NULL || starts_with(name, print->prefix)) {
        solar_os_shell_io_writeln(print->io, name);
    }
    return true;
}

typedef struct {
    const char *prefix;
    size_t count;
    char match[SHELL_INPUT_MAX];
} shell_alias_complete_t;

static bool shell_alias_complete_callback(const char *name, int argc, char **argv, void *user)
{
    shell_alias_complete_t *complete = (shell_alias_complete_t *)user;

    (void)argc;
    (void)argv;

    if (starts_with(name, complete->prefix)) {
        strlcpy(complete->match, name, sizeof(complete->match));
        complete->count++;
    }
    return true;
}

typedef struct {
    const char *name;
    bool found;
    char target[SHELL_INPUT_MAX];
} shell_alias_target_t;

static bool shell_alias_target_callback(const char *name, int argc, char **argv, void *user)
{
    shell_alias_target_t *target = (shell_alias_target_t *)user;

    if (strcmp(name, target->name) != 0) {
        return true;
    }

    if (argc >= 2) {
        strlcpy(target->target, argv[1], sizeof(target->target));
        target->found = true;
    }
    return false;
}

static bool shell_alias_lookup_target_command(const char *name, char *target, size_t target_len)
{
    shell_alias_target_t lookup = {
        .name = name,
    };

    if (target == NULL || target_len == 0) {
        return false;
    }

    (void)shell_for_each_alias(shell_alias_target_callback, &lookup);
    if (!lookup.found) {
        return false;
    }

    strlcpy(target, lookup.target, target_len);
    return true;
}

static uint32_t shell_now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void shell_watch_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: watch [-n seconds] <command> [args...]");
}

static bool shell_watch_parse_interval(const char *text, uint32_t *interval_ms)
{
    if (text == NULL || text[0] == '\0' || interval_ms == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long seconds = strtoul(text, &end, 10);
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        seconds < (SHELL_WATCH_MIN_INTERVAL_MS / 1000U) ||
        seconds > (SHELL_WATCH_MAX_INTERVAL_MS / 1000U)) {
        return false;
    }

    *interval_ms = (uint32_t)seconds * 1000U;
    return true;
}

static bool shell_watch_build_command(int argc, char **argv, int first_arg, char *command, size_t command_len)
{
    if (command == NULL || command_len == 0 || first_arg >= argc) {
        return false;
    }

    command[0] = '\0';
    for (int i = first_arg; i < argc; i++) {
        if (!shell_append_token(command, command_len, argv[i])) {
            return false;
        }
    }
    return command[0] != '\0';
}

static void shell_watch_refresh(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *term = terminal(ctx);

    solar_os_shell_io_clear(term);
    solar_os_shell_io_printf_bold(term,
                                  "Every %" PRIu32 "s: %s\n",
                                  shell_session(ctx)->watch_interval_ms / 1000U,
                                  shell_session(ctx)->watch_command);
    solar_os_shell_io_printf(term,
                             "%s, ESC, or q exits\n",
                             solar_os_shell_io_app_exit_key(term));
    solar_os_shell_io_put_char(term, '\n');

    shell_session(ctx)->watch_executing = true;
    (void)shell_execute_line(ctx, shell_session(ctx)->watch_command, false, NULL, 0);
    shell_session(ctx)->watch_executing = false;
}

static void shell_watch_stop(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *term = terminal(ctx);

    shell_session(ctx)->watch_active = false;
    shell_session(ctx)->watch_executing = false;
    shell_session(ctx)->watch_command[0] = '\0';
    shell_session(ctx)->watch_next_ms = 0;

    solar_os_shell_io_newline(term);
    solar_os_shell_io_writeln(term, "watch stopped");
    shell_prompt(ctx);
}

static char shell_log_level_letter(solar_os_log_level_t level)
{
    switch (level) {
    case SOLAR_OS_LOG_LEVEL_ERROR:
        return 'E';
    case SOLAR_OS_LOG_LEVEL_WARN:
        return 'W';
    case SOLAR_OS_LOG_LEVEL_DEBUG:
        return 'D';
    case SOLAR_OS_LOG_LEVEL_INFO:
    default:
        return 'I';
    }
}

static uint32_t shell_log_latest_sequence(void)
{
    solar_os_log_entry_t entry;
    size_t total = 0;
    const size_t copied = solar_os_log_snapshot(&entry, 1, &total);
    return copied > 0 ? entry.sequence : 0;
}

static void shell_log_follow_print_entry(solar_os_shell_io_t *term,
                                         const solar_os_log_entry_t *entry)
{
    if (term == NULL || entry == NULL) {
        return;
    }

    const uint32_t seconds = entry->timestamp_ms / 1000U;
    const uint32_t ms = entry->timestamp_ms % 1000U;
    solar_os_shell_io_printf(term,
                             "%06" PRIu32 " %5" PRIu32 ".%03" PRIu32 " %c %-16s %s%s\n",
                             entry->sequence,
                             seconds,
                             ms,
                             shell_log_level_letter(entry->level),
                             entry->tag,
                             entry->message,
                             entry->truncated ? "..." : "");
}

static void shell_log_follow_print_new(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *term = terminal(ctx);
    solar_os_log_entry_t entries[SHELL_LOG_FOLLOW_BATCH];

    while (shell_session(ctx)->log_follow_active) {
        size_t available = 0;
        const size_t copied =
            solar_os_log_snapshot_since(shell_session(ctx)->log_follow_last_sequence,
                                        shell_session(ctx)->log_follow_level,
                                        entries,
                                        sizeof(entries) / sizeof(entries[0]),
                                        &available);
        if (copied == 0) {
            break;
        }

        for (size_t i = 0; i < copied; i++) {
            shell_log_follow_print_entry(term, &entries[i]);
            shell_session(ctx)->log_follow_last_sequence = entries[i].sequence;
        }

        if (available <= copied) {
            break;
        }
    }

    solar_os_shell_io_flush(term);
}

static void shell_log_follow_stop(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *term = terminal(ctx);

    shell_session(ctx)->log_follow_active = false;
    shell_session(ctx)->log_follow_next_ms = 0;
    shell_session(ctx)->log_follow_last_sequence = 0;

    solar_os_shell_io_newline(term);
    solar_os_shell_io_writeln(term, "log follow stopped");
    shell_prompt(ctx);
}

esp_err_t solar_os_shell_session_start_log_follow(solar_os_context_t *ctx,
                                                  solar_os_log_level_t level)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_shell_session_t *session = shell_session(ctx);
    if (session->watch_active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_shell_io_t *term = terminal(ctx);
    session->log_follow_active = true;
    session->log_follow_level = level;
    session->log_follow_last_sequence = shell_log_latest_sequence();
    session->log_follow_next_ms = shell_now_ms() + SHELL_LOG_FOLLOW_POLL_MS;
    session->builtin_suppressed_prompt = true;

    solar_os_shell_io_clear(term);
    solar_os_shell_io_printf_bold(term, "Following SolarOS logs: %s\n",
                                  solar_os_log_level_name(level));
    solar_os_shell_io_printf(term,
                             "%s, ESC, Ctrl+C, or q exits\n",
                             solar_os_shell_io_app_exit_key(term));
    solar_os_shell_io_put_char(term, '\n');
    solar_os_shell_io_flush(term);
    return ESP_OK;
}

static void cmd_watch(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    uint32_t interval_ms = SHELL_WATCH_DEFAULT_INTERVAL_MS;
    int first_command_arg = 1;

    if (shell_session(ctx)->watch_executing) {
        solar_os_shell_io_writeln(term, "watch: nested watch is not supported");
        return;
    }

    if (argc < 2) {
        shell_watch_print_usage(term);
        return;
    }

    if (strcmp(argv[first_command_arg], "-n") == 0) {
        if (argc < 4 || !shell_watch_parse_interval(argv[first_command_arg + 1], &interval_ms)) {
            shell_watch_print_usage(term);
            return;
        }
        first_command_arg += 2;
    } else if (starts_with(argv[first_command_arg], "-n") && argv[first_command_arg][2] != '\0') {
        if (!shell_watch_parse_interval(&argv[first_command_arg][2], &interval_ms)) {
            shell_watch_print_usage(term);
            return;
        }
        first_command_arg++;
    } else if (argv[first_command_arg][0] == '-' && argv[first_command_arg][1] != '\0') {
        shell_watch_print_usage(term);
        return;
    }

    if (!shell_watch_build_command(argc,
                                   argv,
                                   first_command_arg,
                                   shell_session(ctx)->watch_command,
                                   sizeof(shell_session(ctx)->watch_command))) {
        solar_os_shell_io_writeln(term, "watch: command too long");
        return;
    }

    shell_session(ctx)->watch_active = true;
    shell_session(ctx)->watch_interval_ms = interval_ms;
    shell_session(ctx)->watch_next_ms = shell_now_ms() + shell_session(ctx)->watch_interval_ms;
    shell_watch_refresh(ctx);
    shell_session(ctx)->builtin_suppressed_prompt = true;
}

static bool shell_arg_has_wildcards(const char *arg)
{
    return arg != NULL && (strchr(arg, '*') != NULL || strchr(arg, '?') != NULL);
}

static bool shell_wildcard_match(const char *pattern, const char *text)
{
    const char *star = NULL;
    const char *retry = NULL;

    while (*text != '\0') {
        if (*pattern == '?' || *pattern == *text) {
            pattern++;
            text++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (star != NULL) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return false;
        }
    }

    while (*pattern == '*') {
        pattern++;
    }
    return *pattern == '\0';
}

static bool shell_is_path_command(const char *command)
{
    return strcmp(command, "cd") == 0 ||
           strcmp(command, "ls") == 0 ||
           strcmp(command, "cat") == 0 ||
           strcmp(command, "sh") == 0 ||
           strcmp(command, "mkdir") == 0 ||
           strcmp(command, "rm") == 0 ||
           strcmp(command, "mv") == 0 ||
           strcmp(command, "cp") == 0 ||
#if SOLAR_OS_PACKAGE_AUDIO
           strcmp(command, "aplay") == 0 ||
           strcmp(command, "arecord") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_UTILS
           strcmp(command, "edit") == 0 ||
           strcmp(command, "less") == 0 ||
           strcmp(command, "reader") == 0 ||
           strcmp(command, "sheet") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_PYTHON
           strcmp(command, "python") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_LUA
           strcmp(command, "lua") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_MEDIA
           strcmp(command, "view") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_NET
           strcmp(command, "scp") == 0;
#else
           false;
#endif
}

static bool shell_path_completion_dirs_only(const char *command)
{
    return strcmp(command, "cd") == 0;
}

static void join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    const size_t dir_len = strlen(dir);

    if (strcmp(dir, "/") == 0) {
        snprintf(out, out_len, "/%s", name);
    } else if (dir_len > 0 && dir[dir_len - 1] == '/') {
        snprintf(out, out_len, "%s%s", dir, name);
    } else {
        snprintf(out, out_len, "%s/%s", dir, name);
    }
}

static bool join_path_checked(char *out, size_t out_len, const char *dir, const char *name)
{
    const size_t dir_len = strlen(dir);
    int written = 0;

    if (strcmp(dir, "/") == 0) {
        written = snprintf(out, out_len, "/%s", name);
    } else if (dir_len > 0 && dir[dir_len - 1] == '/') {
        written = snprintf(out, out_len, "%s%s", dir, name);
    } else {
        written = snprintf(out, out_len, "%s/%s", dir, name);
    }

    return written >= 0 && (size_t)written < out_len;
}

static bool shell_path_is_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    closedir(dir);
    return true;
}

static bool shell_path_entry_is_dir(const char *dir_path, const char *name)
{
    char full_path[SHELL_PATH_MAX];
    join_path(full_path, sizeof(full_path), dir_path, name);
    return shell_path_is_dir(full_path);
}

static void shell_print_path_match(solar_os_shell_io_t *io, const char *dir_path, const char *name)
{
    solar_os_shell_io_write(io, name);
    if (shell_path_entry_is_dir(dir_path, name)) {
        solar_os_shell_io_put_char(io, '/');
    }
    solar_os_shell_io_put_char(io, '\n');
}

static void shell_print_builtin_command_matches(solar_os_context_t *ctx, const char *prefix)
{
    char original[SHELL_INPUT_MAX];
    solar_os_shell_io_t *io = shell_io(ctx);
    shell_alias_print_t alias_print = {
        .prefix = prefix,
        .io = io,
    };

    strlcpy(original, shell_session(ctx)->input, sizeof(original));

    solar_os_shell_io_newline(io);
    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        if (prefix == NULL || starts_with(shell_builtin_commands[i].name, prefix)) {
            solar_os_shell_io_writeln(io, shell_builtin_commands[i].name);
        }
    }
    for (size_t i = 0; i < solar_os_app_registry_count(); i++) {
        const solar_os_app_registry_entry_t *app = solar_os_app_registry_get(i);
        if (app != NULL && app->name != NULL &&
            (prefix == NULL || starts_with(app->name, prefix))) {
            solar_os_shell_io_writeln(io, app->name);
        }
    }
    (void)shell_for_each_alias(shell_alias_print_callback, &alias_print);

    shell_prompt(ctx);
    shell_replace_input(ctx, original);
}

static void shell_complete_builtin_command(solar_os_context_t *ctx, bool show_matches)
{
    const char *match = NULL;
    size_t match_count = 0;
    char alias_match[SHELL_INPUT_MAX];

    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        if (starts_with(shell_builtin_commands[i].name, shell_session(ctx)->input)) {
            match = shell_builtin_commands[i].name;
            match_count++;
        }
    }
    for (size_t i = 0; i < solar_os_app_registry_count(); i++) {
        const solar_os_app_registry_entry_t *app = solar_os_app_registry_get(i);
        if (app != NULL && app->name != NULL && starts_with(app->name, shell_session(ctx)->input)) {
            match = app->name;
            match_count++;
        }
    }
    shell_alias_complete_t alias_complete = {
        .prefix = shell_session(ctx)->input,
    };
    (void)shell_for_each_alias(shell_alias_complete_callback, &alias_complete);
    if (alias_complete.count > 0) {
        strlcpy(alias_match, alias_complete.match, sizeof(alias_match));
        match = alias_match;
        match_count += alias_complete.count;
    }

    if (match_count == 0) {
        return;
    }

    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;

    if (match_count == 1) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed, sizeof(completed), "%s ", match);
        shell_replace_input(ctx, completed);
        return;
    }

    if (show_matches) {
        shell_print_builtin_command_matches(ctx, shell_session(ctx)->input);
    }
}

typedef struct {
    char tokens[SHELL_ARG_MAX][SHELL_INPUT_MAX];
    size_t starts[SHELL_ARG_MAX];
    size_t count;
    bool trailing_space;
} shell_completion_parse_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_shell_io_t *io;
    const char *prefix;
    char match[SHELL_INPUT_MAX];
    size_t count;
    bool print;
} shell_completion_match_t;

static bool shell_completion_parse_input(solar_os_context_t *ctx, shell_completion_parse_t *parse)
{
    solar_os_shell_session_t *session = shell_session(ctx);
    size_t pos = 0;

    if (parse == NULL) {
        return false;
    }

    memset(parse, 0, sizeof(*parse));
    parse->trailing_space = session->input_len > 0 &&
        isspace((unsigned char)session->input[session->input_len - 1]);

    while (pos < session->input_len) {
        while (pos < session->input_len &&
               isspace((unsigned char)session->input[pos])) {
            pos++;
        }
        if (pos >= session->input_len) {
            break;
        }
        if (parse->count >= SHELL_ARG_MAX) {
            return false;
        }

        const size_t start = pos;
        while (pos < session->input_len &&
               !isspace((unsigned char)session->input[pos])) {
            pos++;
        }
        const size_t len = pos - start;
        if (len >= sizeof(parse->tokens[0])) {
            return false;
        }
        parse->starts[parse->count] = start;
        memcpy(parse->tokens[parse->count], &session->input[start], len);
        parse->tokens[parse->count][len] = '\0';
        parse->count++;
    }

    return true;
}

typedef struct {
    char original[SHELL_INPUT_MAX];
    char token[SHELL_PATH_MAX];
    char dir_arg[SHELL_PATH_MAX];
    char dir_path[SHELL_PATH_MAX];
    char base_arg[SHELL_PATH_MAX];
    char match_name[SHELL_PATH_MAX];
    char completed_arg[SHELL_PATH_MAX];
    char completed_line[SHELL_INPUT_MAX];
} shell_path_completion_work_t;

static shell_path_completion_work_t *shell_alloc_path_completion_work(void)
{
    shell_path_completion_work_t *work =
        heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work == NULL) {
        work = heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_8BIT);
    }
    return work;
}

static void shell_complete_path(solar_os_context_t *ctx, size_t token_start, bool dirs_only)
{
    solar_os_shell_io_t *io = shell_io(ctx);
    shell_path_completion_work_t *work = shell_alloc_path_completion_work();
    bool match_is_dir = false;
    size_t match_count = 0;
    bool printed_matches = false;

    if (work == NULL) {
        return;
    }

    strlcpy(work->original, shell_session(ctx)->input, sizeof(work->original));

    const size_t token_len = shell_session(ctx)->input_len - token_start;
    if (token_len >= sizeof(work->token)) {
        heap_caps_free(work);
        return;
    }
    memcpy(work->token, &shell_session(ctx)->input[token_start], token_len);
    work->token[token_len] = '\0';

    const char *prefix = work->token;
    const char *dir_to_resolve = NULL;
    char *slash = strrchr(work->token, '/');
    if (slash != NULL) {
        const size_t base_len = (size_t)(slash - work->token) + 1;
        memcpy(work->base_arg, work->token, base_len);
        work->base_arg[base_len] = '\0';
        prefix = slash + 1;

        const size_t dir_arg_len = (size_t)(slash - work->token);
        if (dir_arg_len == 0) {
            strlcpy(work->dir_arg, "/", sizeof(work->dir_arg));
        } else {
            memcpy(work->dir_arg, work->token, dir_arg_len);
            work->dir_arg[dir_arg_len] = '\0';
        }
        dir_to_resolve = work->dir_arg;
    } else {
        work->base_arg[0] = '\0';
    }

    if (resolve_path(ctx, dir_to_resolve, work->dir_path, sizeof(work->dir_path)) != ESP_OK) {
        heap_caps_free(work);
        return;
    }

    DIR *dir = opendir(work->dir_path);
    if (dir == NULL) {
        heap_caps_free(work);
        return;
    }

    const bool prefix_has_wildcards = shell_arg_has_wildcards(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (prefix_has_wildcards) {
            if (!shell_wildcard_match(prefix, entry->d_name)) {
                continue;
            }
        } else if (!starts_with(entry->d_name, prefix)) {
            continue;
        }

        const bool entry_is_dir = shell_path_entry_is_dir(work->dir_path, entry->d_name);
        if (dirs_only && !entry_is_dir) {
            continue;
        }

        match_count++;
        if (match_count == 1) {
            strlcpy(work->match_name, entry->d_name, sizeof(work->match_name));
            match_is_dir = entry_is_dir;
        } else {
            if (!printed_matches) {
                solar_os_shell_io_newline(io);
                shell_print_path_match(io, work->dir_path, work->match_name);
                printed_matches = true;
            }
            shell_print_path_match(io, work->dir_path, entry->d_name);
        }
    }

    closedir(dir);

    if (match_count == 0) {
        heap_caps_free(work);
        return;
    }

    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;

    if (match_count == 1) {
        snprintf(work->completed_arg,
                 sizeof(work->completed_arg),
                 "%s%s%s",
                 work->base_arg,
                 work->match_name,
                 match_is_dir ? "/" : " ");
        snprintf(work->completed_line,
                 sizeof(work->completed_line),
                 "%.*s%s",
                 (int)token_start,
                 shell_session(ctx)->input,
                 work->completed_arg);
        shell_replace_input(ctx, work->completed_line);
        heap_caps_free(work);
        return;
    }

    shell_prompt(ctx);
    shell_replace_input(ctx, work->original);
    heap_caps_free(work);
}

static bool shell_completion_path_matches(const shell_completion_rule_t *rule,
                                          const char * const *tokens,
                                          size_t token_count,
                                          size_t *wildcard_count)
{
    size_t wildcards = 0;

    if (rule == NULL || tokens == NULL || rule->path_count != token_count) {
        return false;
    }

    for (size_t i = 0; i < token_count; i++) {
        if (strcmp(rule->path[i], SHELL_COMPLETION_ANY) == 0) {
            wildcards++;
            continue;
        }
        if (strcmp(rule->path[i], tokens[i]) != 0) {
            return false;
        }
    }

    if (wildcard_count != NULL) {
        *wildcard_count = wildcards;
    }
    return true;
}

static void shell_completion_emit(shell_completion_match_t *state, const char *value)
{
    if (state == NULL || value == NULL) {
        return;
    }
    if (state->prefix != NULL && !starts_with(value, state->prefix)) {
        return;
    }

    strlcpy(state->match, value, sizeof(state->match));
    state->count++;
    if (state->print) {
        solar_os_shell_io_writeln(state->io, value);
    }
}

static bool shell_completion_alias_emit_callback(const char *name,
                                                 int argc,
                                                 char **argv,
                                                 void *user)
{
    shell_completion_match_t *state = (shell_completion_match_t *)user;

    (void)argc;
    (void)argv;

    shell_completion_emit(state, name);
    return true;
}

static void shell_completion_emit_commands(shell_completion_match_t *state)
{
    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        shell_completion_emit(state, shell_builtin_commands[i].name);
    }
    for (size_t i = 0; i < solar_os_app_registry_count(); i++) {
        const solar_os_app_registry_entry_t *app = solar_os_app_registry_get(i);
        if (app != NULL && app->name != NULL) {
            shell_completion_emit(state, app->name);
        }
    }
    (void)shell_for_each_alias(shell_completion_alias_emit_callback, state);
}

static void shell_completion_emit_jobs(shell_completion_match_t *state)
{
    for (size_t i = 0; i < solar_os_job_registry_count(); i++) {
        const solar_os_job_registry_entry_t *job = solar_os_job_registry_get(i);
        if (job != NULL && job->name != NULL) {
            shell_completion_emit(state, job->name);
        }
    }
}

static void shell_completion_emit_ports(shell_completion_match_t *state)
{
    solar_os_port_info_t ports[SOLAR_OS_PORT_MAX];
    const size_t count = solar_os_port_list(ports, SHELL_ARRAY_COUNT(ports));

    for (size_t i = 0; i < count && i < SHELL_ARRAY_COUNT(ports); i++) {
        shell_completion_emit(state, ports[i].name);
    }
}

static void shell_completion_emit_streams(shell_completion_match_t *state, bool scalar_only)
{
    const size_t count = solar_os_stream_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_stream_info_t info;
        if (solar_os_stream_get(i, &info) &&
            (!scalar_only || info.type == SOLAR_OS_STREAM_TYPE_SCALAR)) {
            shell_completion_emit(state, info.id);
        }
    }
}

static bool shell_completion_collect_matches(solar_os_context_t *ctx,
                                             const char * const *tokens,
                                             size_t token_count,
                                             const char *prefix,
                                             bool print,
                                             shell_completion_match_t *state)
{
    bool rule_seen = false;

    if (state == NULL) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->ctx = ctx;
    state->io = shell_io(ctx);
    state->prefix = prefix;
    state->print = print;

    for (size_t i = 0; i < SHELL_ARRAY_COUNT(shell_completion_rules); i++) {
        const shell_completion_rule_t *rule = &shell_completion_rules[i];
        if (rule->complete_path ||
            !shell_completion_path_matches(rule, tokens, token_count, NULL)) {
            continue;
        }
        if (rule->required_prefix != NULL &&
            (prefix == NULL || !starts_with(prefix, rule->required_prefix))) {
            continue;
        }

        rule_seen = true;
        for (size_t value_index = 0; value_index < rule->value_count; value_index++) {
            shell_completion_emit(state, rule->values[value_index]);
        }
        if (rule->complete_commands) {
            shell_completion_emit_commands(state);
        }
        if (rule->complete_jobs) {
            shell_completion_emit_jobs(state);
        }
        if (rule->complete_ports) {
            shell_completion_emit_ports(state);
        }
        if (rule->complete_streams) {
            shell_completion_emit_streams(state, rule->scalar_streams_only);
        }
    }

    return rule_seen;
}

static const shell_completion_rule_t *shell_completion_find_path_rule(const char * const *tokens,
                                                                      size_t token_count)
{
    const shell_completion_rule_t *best = NULL;
    size_t best_wildcards = SHELL_ARG_MAX + 1;

    for (size_t i = 0; i < SHELL_ARRAY_COUNT(shell_completion_rules); i++) {
        const shell_completion_rule_t *rule = &shell_completion_rules[i];
        size_t wildcards = 0;

        if (!rule->complete_path ||
            !shell_completion_path_matches(rule, tokens, token_count, &wildcards)) {
            continue;
        }
        if (best == NULL || wildcards < best_wildcards) {
            best = rule;
            best_wildcards = wildcards;
        }
    }

    return best;
}

static void shell_print_argument_completion_matches(solar_os_context_t *ctx,
                                                    const char * const *tokens,
                                                    size_t token_count,
                                                    const char *prefix)
{
    char original[SHELL_INPUT_MAX];
    shell_completion_match_t state;

    strlcpy(original, shell_session(ctx)->input, sizeof(original));
    solar_os_shell_io_newline(shell_io(ctx));
    (void)shell_completion_collect_matches(ctx, tokens, token_count, prefix, true, &state);
    shell_prompt(ctx);
    shell_replace_input(ctx, original);
}

static bool shell_complete_argument(solar_os_context_t *ctx,
                                    const shell_completion_parse_t *parse,
                                    size_t current_index,
                                    size_t token_start,
                                    bool show_matches)
{
    const char *completed_tokens[SHELL_ARG_MAX];
    char effective_command[SHELL_INPUT_MAX];
    const char *prefix = "";
    const size_t completed_count = current_index;
    shell_completion_match_t state;

    if (parse == NULL || parse->count == 0 || current_index >= SHELL_ARG_MAX) {
        return false;
    }

    for (size_t i = 0; i < completed_count; i++) {
        completed_tokens[i] = parse->tokens[i];
    }

    if (!shell_alias_lookup_target_command(parse->tokens[0],
                                           effective_command,
                                           sizeof(effective_command))) {
        strlcpy(effective_command, parse->tokens[0], sizeof(effective_command));
    }
    completed_tokens[0] = effective_command;

    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

    const shell_completion_rule_t *path_rule =
        shell_completion_find_path_rule(completed_tokens, completed_count);
    if (path_rule != NULL) {
        shell_complete_path(ctx, token_start, path_rule->dirs_only);
        return true;
    }

    const bool rule_seen = shell_completion_collect_matches(ctx,
                                                            completed_tokens,
                                                            completed_count,
                                                            prefix,
                                                            false,
                                                            &state);
    if (!rule_seen) {
        return false;
    }
    if (state.count == 0) {
        return true;
    }

    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;

    if (state.count == 1 && !show_matches) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed,
                 sizeof(completed),
                 "%.*s%s ",
                 (int)token_start,
                 shell_session(ctx)->input,
                 state.match);
        shell_replace_input(ctx, completed);
        return true;
    }

    if (show_matches) {
        shell_print_argument_completion_matches(ctx,
                                                completed_tokens,
                                                completed_count,
                                                prefix[0] == '\0' ? NULL : prefix);
    }
    return true;
}

static void shell_complete_command(solar_os_context_t *ctx, bool show_matches)
{
    if (shell_session(ctx)->input_cursor != shell_session(ctx)->input_len) {
        return;
    }

    if (shell_session(ctx)->input_len == 0) {
        if (show_matches) {
            shell_print_builtin_command_matches(ctx, NULL);
        }
        return;
    }
    if (isspace((unsigned char)shell_session(ctx)->input[0])) {
        return;
    }

    shell_completion_parse_t parse;
    if (!shell_completion_parse_input(ctx, &parse) || parse.count == 0) {
        if (show_matches && shell_session(ctx)->input_len == 0) {
            shell_print_builtin_command_matches(ctx, NULL);
        }
        return;
    }

    const size_t current_index = parse.trailing_space ? parse.count : parse.count - 1;
    if (current_index == 0 && !parse.trailing_space) {
        shell_complete_builtin_command(ctx, show_matches);
        return;
    }

    const char *command = parse.tokens[0];
    char effective_command[SHELL_INPUT_MAX];
    if (!shell_alias_lookup_target_command(command, effective_command, sizeof(effective_command))) {
        strlcpy(effective_command, command, sizeof(effective_command));
    }

    const size_t token_start =
        parse.trailing_space ? shell_session(ctx)->input_len : parse.starts[current_index];
    if (shell_complete_argument(ctx, &parse, current_index, token_start, show_matches)) {
        return;
    }

    if (!shell_is_path_command(effective_command)) {
        return;
    }
    if (strcmp(effective_command, "scp") == 0 &&
        memchr(&shell_session(ctx)->input[token_start], ':', shell_session(ctx)->input_len - token_start) != NULL) {
        return;
    }

    shell_complete_path(ctx, token_start, shell_path_completion_dirs_only(effective_command));
}

typedef struct {
    char dir_path[SHELL_PATH_MAX];
    char base_arg[SHELL_PATH_MAX];
    char name_pattern[SHELL_PATH_MAX];
} shell_wildcard_path_t;

typedef bool (*shell_wildcard_match_callback_t)(solar_os_context_t *ctx,
                                                const char *full_path,
                                                const char *display_path,
                                                const char *name,
                                                void *user);

static const char *shell_path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool shell_prepare_wildcard_path(solar_os_context_t *ctx,
                                        const char *arg,
                                        shell_wildcard_path_t *wildcard)
{
    char dir_arg[SHELL_PATH_MAX];
    const char *name_pattern = arg;
    const char *dir_to_resolve = NULL;
    const char *slash = strrchr(arg, '/');

    if (slash != NULL) {
        const size_t dir_len = (size_t)(slash - arg);
        const size_t base_len = dir_len + 1;
        if (base_len >= sizeof(wildcard->base_arg) ||
            strlen(slash + 1) >= sizeof(wildcard->name_pattern)) {
            return false;
        }

        memcpy(wildcard->base_arg, arg, base_len);
        wildcard->base_arg[base_len] = '\0';
        name_pattern = slash + 1;

        if (dir_len == 0) {
            strlcpy(dir_arg, "/", sizeof(dir_arg));
        } else {
            if (dir_len >= sizeof(dir_arg)) {
                return false;
            }
            memcpy(dir_arg, arg, dir_len);
            dir_arg[dir_len] = '\0';
        }

        if (shell_arg_has_wildcards(dir_arg)) {
            return false;
        }
        dir_to_resolve = dir_arg;
    } else {
        wildcard->base_arg[0] = '\0';
    }

    if (!shell_arg_has_wildcards(name_pattern)) {
        return false;
    }

    strlcpy(wildcard->name_pattern, name_pattern, sizeof(wildcard->name_pattern));
    return resolve_path(ctx, dir_to_resolve, wildcard->dir_path, sizeof(wildcard->dir_path)) == ESP_OK;
}

static size_t shell_for_each_wildcard_match(solar_os_context_t *ctx,
                                            const char *command,
                                            const char *arg,
                                            shell_wildcard_match_callback_t callback,
                                            void *user,
                                            bool *had_error)
{
    shell_wildcard_path_t wildcard;
    solar_os_shell_io_t *term = terminal(ctx);
    size_t match_count = 0;

    if (had_error != NULL) {
        *had_error = false;
    }

    if (!shell_prepare_wildcard_path(ctx, arg, &wildcard)) {
        solar_os_shell_io_printf(term,
                                 "%s: wildcards are only supported in the filename: %s\n",
                                 command,
                                 arg);
        if (had_error != NULL) {
            *had_error = true;
        }
        return 0;
    }

    DIR *dir = opendir(wildcard.dir_path);
    if (dir == NULL) {
        solar_os_shell_io_printf(term,
                                 "%s: cannot open %s: %s\n",
                                 command,
                                 wildcard.dir_path,
                                 strerror(errno));
        if (had_error != NULL) {
            *had_error = true;
        }
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!shell_wildcard_match(wildcard.name_pattern, entry->d_name)) {
            continue;
        }

        match_count++;

        if (callback != NULL) {
            char full_path[SHELL_PATH_MAX];
            char display_path[SHELL_PATH_MAX];
            join_path(full_path, sizeof(full_path), wildcard.dir_path, entry->d_name);
            snprintf(display_path,
                     sizeof(display_path),
                     "%s%s",
                     wildcard.base_arg,
                     entry->d_name);
            if (!callback(ctx, full_path, display_path, entry->d_name, user)) {
                break;
            }
        }
    }

    closedir(dir);
    return match_count;
}

static void shell_report_no_wildcard_matches(solar_os_shell_io_t *term,
                                             const char *command,
                                             const char *arg,
                                             size_t match_count,
                                             bool had_error)
{
    if (match_count == 0 && !had_error) {
        solar_os_shell_io_printf(term, "%s: no match: %s\n", command, arg);
    }
}

static void cmd_cd(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char path[SHELL_PATH_MAX];

    if (argc > 2) {
        solar_os_shell_io_writeln(term, "usage: cd [path]");
        return;
    }

    if (!resolve_path_for_command(ctx, term, "cd", argc == 2 ? argv[1] : "/", path, sizeof(path))) {
        return;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        solar_os_shell_io_printf(term,
                                 "cd: cannot open %s: %s\n",
                                 argc == 2 ? argv[1] : "/",
                                 strerror(errno));
        return;
    }
    closedir(dir);

    strlcpy(shell_session(ctx)->cwd, path, sizeof(shell_session(ctx)->cwd));
}

static bool shell_ls_hidden_name(const char *name)
{
    return name != NULL && name[0] == '.';
}

static void shell_format_file_size(char *buffer, size_t buffer_len, off_t size, bool human)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    if (size < 0) {
        strlcpy(buffer, "?", buffer_len);
        return;
    }

    if (!human) {
        snprintf(buffer, buffer_len, "%lld", (long long)size);
        return;
    }

    static const char units[] = {'B', 'K', 'M', 'G'};
    uint64_t scaled_x10 = (uint64_t)size * 10U;
    size_t unit = 0;
    while (scaled_x10 >= 10240U && unit + 1 < sizeof(units)) {
        scaled_x10 = (scaled_x10 + 512U) / 1024U;
        unit++;
    }

    if (unit == 0) {
        snprintf(buffer, buffer_len, "%lluB", (unsigned long long)(scaled_x10 / 10U));
    } else if (scaled_x10 < 100U) {
        snprintf(buffer,
                 buffer_len,
                 "%llu.%llu%c",
                 (unsigned long long)(scaled_x10 / 10U),
                 (unsigned long long)(scaled_x10 % 10U),
                 units[unit]);
    } else {
        snprintf(buffer,
                 buffer_len,
                 "%llu%c",
                 (unsigned long long)((scaled_x10 + 5U) / 10U),
                 units[unit]);
    }
}

static void shell_ls_print_entry_with_options(solar_os_shell_io_t *term,
                                              const char *full_path,
                                              const char *display_name,
                                              const shell_ls_options_t *options)
{
    struct stat st;
    const bool stat_ok = full_path != NULL && stat(full_path, &st) == 0;
    const bool is_dir = stat_ok ? S_ISDIR(st.st_mode) : shell_path_is_dir(full_path);
    char size_text[16];

    if (is_dir) {
        strlcpy(size_text, "<DIR>", sizeof(size_text));
    } else if (stat_ok) {
        shell_format_file_size(size_text,
                               sizeof(size_text),
                               st.st_size,
                               options != NULL && options->human);
    } else {
        strlcpy(size_text, "?", sizeof(size_text));
    }

    solar_os_shell_io_printf(term, "%8s ", size_text);
    if (is_dir) {
        solar_os_shell_io_write_bold(term, display_name);
        solar_os_shell_io_put_char(term, '/');
    } else {
        solar_os_shell_io_write(term, display_name);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void shell_list_directory(solar_os_shell_io_t *term,
                                 const char *path,
                                 const shell_ls_options_t *options)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        solar_os_shell_io_printf(term, "ls: cannot open %s: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char child_path[SHELL_PATH_MAX];
        if ((options == NULL || !options->show_all) && shell_ls_hidden_name(entry->d_name)) {
            continue;
        }
        if (!join_path_checked(child_path, sizeof(child_path), path, entry->d_name)) {
            solar_os_shell_io_printf(term, "%8s %s\n", "?", entry->d_name);
            continue;
        }

        shell_ls_print_entry_with_options(term, child_path, entry->d_name, options);
    }

    closedir(dir);
}

static bool shell_ls_match(solar_os_context_t *ctx,
                           const char *full_path,
                           const char *display_path,
                           const char *name,
                           void *user)
{
    solar_os_shell_io_t *term = terminal(ctx);
    const shell_ls_options_t *options = (const shell_ls_options_t *)user;

    if ((options == NULL || !options->show_all) && shell_ls_hidden_name(name)) {
        return true;
    }
    shell_ls_print_entry_with_options(term, full_path, display_path, options);
    return true;
}

static bool shell_ls_parse_options(solar_os_shell_io_t *term,
                                   int argc,
                                   char **argv,
                                   shell_ls_options_t *options,
                                   const char **path_arg)
{
    bool end_options = false;

    memset(options, 0, sizeof(*options));
    *path_arg = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!end_options && strcmp(arg, "--") == 0) {
            end_options = true;
            continue;
        }

        if (!end_options && arg[0] == '-' && arg[1] != '\0') {
            for (const char *p = &arg[1]; *p != '\0'; p++) {
                if (*p == 'a') {
                    options->show_all = true;
                } else if (*p == 'h') {
                    options->human = true;
                } else {
                    solar_os_shell_io_writeln(term, "usage: ls [-a] [-h] [path|pattern]");
                    return false;
                }
            }
            continue;
        }

        if (*path_arg != NULL) {
            solar_os_shell_io_writeln(term, "usage: ls [-a] [-h] [path|pattern]");
            return false;
        }
        *path_arg = arg;
    }

    return true;
}

static void cmd_ls(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    shell_ls_options_t options;
    const char *path_arg = NULL;

    if (!shell_ls_parse_options(term, argc, argv, &options, &path_arg)) {
        return;
    }

    char path[SHELL_PATH_MAX];
    if (path_arg != NULL && shell_arg_has_wildcards(path_arg)) {
        bool had_error = false;
        const size_t match_count =
            shell_for_each_wildcard_match(ctx, "ls", path_arg, shell_ls_match, &options, &had_error);
        shell_report_no_wildcard_matches(term, "ls", path_arg, match_count, had_error);
        return;
    }

    if (!resolve_path_for_command(ctx, term, "ls", path_arg, path, sizeof(path))) {
        return;
    }

    struct stat st;
    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        shell_ls_print_entry_with_options(term, path, path_arg != NULL ? path_arg : path, &options);
        return;
    }

    shell_list_directory(term, path, &options);
}

static bool shell_cat_file(solar_os_shell_io_t *term, const char *path, const char *display_path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        solar_os_shell_io_printf(term, "cat: cannot open %s: %s\n", display_path, strerror(errno));
        return false;
    }

    char buffer[96];
    size_t bytes_read = 0;
    while (bytes_read < SHELL_CAT_MAX_BYTES && fgets(buffer, sizeof(buffer), file) != NULL) {
        bytes_read += strlen(buffer);
        solar_os_shell_io_write(term, buffer);
    }

    if (!feof(file)) {
        solar_os_shell_io_printf(term, "\ncat: %s: truncated\n", display_path);
    }

    fclose(file);
    return true;
}

typedef struct {
    solar_os_shell_io_t *term;
    bool had_error;
} shell_file_action_t;

static bool shell_cat_match(solar_os_context_t *ctx,
                            const char *full_path,
                            const char *display_path,
                            const char *name,
                            void *user)
{
    shell_file_action_t *action = (shell_file_action_t *)user;

    (void)ctx;
    (void)name;

    if (!shell_cat_file(action->term, full_path, display_path)) {
        action->had_error = true;
    }
    return true;
}

static void cmd_cat(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc != 2) {
        solar_os_shell_io_writeln(term, "usage: cat <path|pattern>");
        return;
    }

    if (shell_arg_has_wildcards(argv[1])) {
        bool had_error = false;
        shell_file_action_t action = {
            .term = term,
            .had_error = false,
        };
        const size_t match_count =
            shell_for_each_wildcard_match(ctx, "cat", argv[1], shell_cat_match, &action, &had_error);
        shell_report_no_wildcard_matches(term,
                                         "cat",
                                         argv[1],
                                         match_count,
                                         had_error || action.had_error);
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_path_for_command(ctx, term, "cat", argv[1], path, sizeof(path))) {
        return;
    }
    shell_cat_file(term, path, path);
}

static void shell_script_discard_rest_of_line(FILE *file)
{
    int ch = 0;

    do {
        ch = fgetc(file);
    } while (ch != EOF && ch != '\n');
}

static bool shell_run_script(solar_os_context_t *ctx,
                             const char *path,
                             const char *display_path,
                             bool report_open_error)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char line[SHELL_INPUT_MAX + 1];
    size_t line_number = 0;

    if (shell_session(ctx)->script_depth >= SHELL_SCRIPT_MAX_DEPTH) {
        solar_os_shell_io_writeln(term, "sh: script nesting too deep");
        return true;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (report_open_error) {
            solar_os_shell_io_printf(term,
                                     "sh: cannot open %s: %s\n",
                                     display_path,
                                     strerror(errno));
        }
        return true;
    }

    shell_session(ctx)->script_depth++;
    bool should_prompt = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        const bool complete_line = strchr(line, '\n') != NULL;
        if (!complete_line && !feof(file)) {
            shell_script_discard_rest_of_line(file);
            solar_os_shell_io_printf(term,
                                     "sh: %s:%u: line too long\n",
                                     display_path,
                                     (unsigned)line_number);
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        char *command = shell_trim_line(line);
        if (command == NULL || command[0] == '\0' || command[0] == '#') {
            continue;
        }
        if (strlen(command) >= SHELL_INPUT_MAX) {
            solar_os_shell_io_printf(term,
                                     "sh: %s:%u: line too long\n",
                                     display_path,
                                     (unsigned)line_number);
            break;
        }

        if (!shell_execute_line(ctx, command, false, display_path, line_number)) {
            shell_session(ctx)->builtin_suppressed_prompt = true;
            should_prompt = false;
            break;
        }
    }

    if (ferror(file)) {
        solar_os_shell_io_printf(term, "sh: read failed: %s\n", strerror(errno));
    }

    shell_session(ctx)->script_depth--;
    fclose(file);
    return should_prompt;
}

static void cmd_sh(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char path[SHELL_PATH_MAX];

    if (argc != 2) {
        solar_os_shell_io_writeln(term, "usage: sh <file>");
        return;
    }
    if (!resolve_path_for_command(ctx, term, "sh", argv[1], path, sizeof(path))) {
        return;
    }

    shell_run_script(ctx, path, argv[1], true);
}

static bool shell_make_directory(solar_os_shell_io_t *term,
                                 const char *path,
                                 const char *display_path)
{
    if (solar_os_storage_mkdir(path) != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "mkdir: cannot create %s: %s\n",
                                 display_path,
                                 strerror(errno));
        return false;
    }

    return true;
}

static void cmd_mkdir(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        solar_os_shell_io_writeln(term, "usage: mkdir <path> [path...]");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (shell_arg_has_wildcards(argv[i])) {
            solar_os_shell_io_printf(term,
                                     "mkdir: wildcards are not supported: %s\n",
                                     argv[i]);
            continue;
        }

        char path[SHELL_PATH_MAX];
        if (!resolve_path_for_command(ctx, term, "mkdir", argv[i], path, sizeof(path))) {
            continue;
        }
        shell_make_directory(term, path, path);
    }
}

typedef struct {
    bool force;
    bool recursive;
} shell_rm_options_t;

typedef struct {
    solar_os_shell_io_t *term;
    shell_rm_options_t options;
    bool had_error;
} shell_rm_action_t;

static size_t trimmed_path_len(const char *path)
{
    size_t len = path != NULL ? strlen(path) : 0;

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    return len;
}

static bool paths_equal_trimmed(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    const size_t a_len = trimmed_path_len(a);
    const size_t b_len = trimmed_path_len(b);

    return a_len == b_len && strncmp(a, b, a_len) == 0;
}

static bool shell_path_is_protected_root(const char *path)
{
    return paths_equal_trimmed(path, "/") ||
           paths_equal_trimmed(path, solar_os_storage_mount_point());
}

static bool shell_remove_file(solar_os_shell_io_t *term,
                              const char *path,
                              const char *display_path,
                              const shell_rm_options_t *options)
{
    if (solar_os_storage_remove(path) == ESP_OK) {
        return true;
    }

    if (options != NULL && options->force && errno == ENOENT) {
        return true;
    }

    solar_os_shell_io_printf(term, "rm: cannot remove %s: %s\n", display_path, strerror(errno));
    return false;
}

static bool shell_remove_empty_directory(solar_os_shell_io_t *term,
                                         const char *path,
                                         const char *display_path)
{
    if (solar_os_storage_rmdir(path) == ESP_OK) {
        return true;
    }

    solar_os_shell_io_printf(term, "rm: cannot remove %s: %s\n", display_path, strerror(errno));
    return false;
}

static bool shell_remove_recursive(solar_os_shell_io_t *term,
                                   const char *path,
                                   const char *display_path,
                                   const shell_rm_options_t *options)
{
    if (shell_path_is_protected_root(path)) {
        solar_os_shell_io_printf(term, "rm: refusing to remove root: %s\n", display_path);
        return false;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        return shell_remove_file(term, path, display_path, options);
    }

    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[SHELL_PATH_MAX];
        char child_display[SHELL_PATH_MAX];
        if (!join_path_checked(child_path, sizeof(child_path), path, entry->d_name) ||
            !join_path_checked(child_display, sizeof(child_display), display_path, entry->d_name)) {
            solar_os_shell_io_printf(term, "rm: path too long below %s\n", display_path);
            ok = false;
            continue;
        }

        if (shell_path_is_dir(child_path)) {
            if (!shell_remove_recursive(term, child_path, child_display, options)) {
                ok = false;
            }
        } else if (!shell_remove_file(term, child_path, child_display, options)) {
            ok = false;
        }
    }

    closedir(dir);
    if (!shell_remove_empty_directory(term, path, display_path)) {
        ok = false;
    }
    return ok;
}

static bool shell_remove_path(solar_os_shell_io_t *term,
                              const char *path,
                              const char *display_path,
                              const shell_rm_options_t *options)
{
    if (shell_path_is_dir(path)) {
        if (options != NULL && options->recursive) {
            return shell_remove_recursive(term, path, display_path, options);
        }
        if (options != NULL && options->force) {
            return shell_remove_empty_directory(term, path, display_path);
        }

        solar_os_shell_io_printf(term,
                                 "rm: %s is a directory; use rm -f for empty dirs or rm -rf recursively\n",
                                 display_path);
        return false;
    }

    return shell_remove_file(term, path, display_path, options);
}

static bool shell_rm_match(solar_os_context_t *ctx,
                           const char *full_path,
                           const char *display_path,
                           const char *name,
                           void *user)
{
    shell_rm_action_t *action = (shell_rm_action_t *)user;

    (void)ctx;
    (void)name;

    if (!shell_remove_path(action->term, full_path, display_path, &action->options)) {
        action->had_error = true;
    }
    return true;
}

static bool shell_rm_parse_options(solar_os_shell_io_t *term,
                                   int argc,
                                   char **argv,
                                   shell_rm_options_t *options,
                                   int *first_path)
{
    memset(options, 0, sizeof(*options));
    *first_path = 1;

    while (*first_path < argc && argv[*first_path][0] == '-' && argv[*first_path][1] != '\0') {
        if (strcmp(argv[*first_path], "--") == 0) {
            (*first_path)++;
            break;
        }

        for (const char *p = &argv[*first_path][1]; *p != '\0'; p++) {
            if (*p == 'f') {
                options->force = true;
            } else if (*p == 'r' || *p == 'R') {
                options->recursive = true;
            } else {
                solar_os_shell_io_printf(term, "rm: unsupported option: -%c\n", *p);
                return false;
            }
        }
        (*first_path)++;
    }

    if (*first_path >= argc) {
        solar_os_shell_io_writeln(term, "usage: rm [-f|-rf] <path|pattern> [path|pattern...]");
        return false;
    }

    return true;
}

static void cmd_rm(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    shell_rm_options_t options;
    int first_path = 1;

    if (!shell_rm_parse_options(term, argc, argv, &options, &first_path)) {
        return;
    }

    for (int i = first_path; i < argc; i++) {
        if (shell_arg_has_wildcards(argv[i])) {
            bool had_error = false;
            shell_rm_action_t action = {
                .term = term,
                .options = options,
                .had_error = false,
            };
            const size_t match_count =
                shell_for_each_wildcard_match(ctx, "rm", argv[i], shell_rm_match, &action, &had_error);
            if (!options.force) {
                shell_report_no_wildcard_matches(term,
                                                 "rm",
                                                 argv[i],
                                                 match_count,
                                                 had_error || action.had_error);
            }
            continue;
        }

        char path[SHELL_PATH_MAX];
        if (!resolve_path_for_command(ctx, term, "rm", argv[i], path, sizeof(path))) {
            continue;
        }
        shell_remove_path(term, path, path, &options);
    }
}

typedef struct {
    solar_os_shell_io_t *term;
    const char *dest_path;
    const char *command;
    bool dest_is_dir;
    bool move;
    bool had_error;
} shell_copy_move_action_t;

static bool shell_copy_or_move_path(solar_os_shell_io_t *term,
                                    const char *command,
                                    const char *source_path,
                                    const char *source_display,
                                    const char *dest_path,
                                    bool dest_is_dir,
                                    bool move,
                                    const char *name)
{
    char final_dest[SHELL_PATH_MAX];
    const char *target_path = dest_path;

    if (dest_is_dir) {
        const char *target_name = name != NULL && name[0] != '\0' ? name : shell_path_basename(source_path);
        join_path(final_dest, sizeof(final_dest), dest_path, target_name);
        target_path = final_dest;
    }

    const esp_err_t err = move ? solar_os_storage_rename(source_path, target_path) :
                                 solar_os_storage_copy_file(source_path, target_path);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "%s: cannot %s %s to %s: %s\n",
                                 command,
                                 move ? "move" : "copy",
                                 source_display,
                                 target_path,
                                 strerror(errno));
        return false;
    }

    return true;
}

static bool shell_copy_move_match(solar_os_context_t *ctx,
                                  const char *full_path,
                                  const char *display_path,
                                  const char *name,
                                  void *user)
{
    shell_copy_move_action_t *action = (shell_copy_move_action_t *)user;

    (void)ctx;

    if (!shell_copy_or_move_path(action->term,
                                 action->command,
                                 full_path,
                                 display_path,
                                 action->dest_path,
                                 action->dest_is_dir,
                                 action->move,
                                 name)) {
        action->had_error = true;
    }
    return true;
}

static void shell_cmd_copy_move(solar_os_context_t *ctx, int argc, char **argv, bool move)
{
    solar_os_shell_io_t *term = terminal(ctx);
    const char *command = move ? "mv" : "cp";

    if (argc != 3) {
        solar_os_shell_io_printf(term,
                                 "usage: %s <source|pattern> <dest>\n",
                                 command);
        return;
    }
    if (shell_arg_has_wildcards(argv[2])) {
        solar_os_shell_io_printf(term,
                                 "%s: destination wildcards are not supported\n",
                                 command);
        return;
    }

    char dest[SHELL_PATH_MAX];
    if (!resolve_path_for_command(ctx, term, command, argv[2], dest, sizeof(dest))) {
        return;
    }
    const bool dest_is_dir = shell_path_is_dir(dest);

    if (shell_arg_has_wildcards(argv[1])) {
        bool had_error = false;
        const size_t match_count =
            shell_for_each_wildcard_match(ctx, command, argv[1], NULL, NULL, &had_error);
        shell_report_no_wildcard_matches(term, command, argv[1], match_count, had_error);
        if (match_count == 0 || had_error) {
            return;
        }
        if (match_count > 1 && !dest_is_dir) {
            solar_os_shell_io_printf(term,
                                     "%s: destination must be a directory for multiple sources: %s\n",
                                     command,
                                     dest);
            return;
        }

        shell_copy_move_action_t action = {
            .term = term,
            .dest_path = dest,
            .command = command,
            .dest_is_dir = dest_is_dir,
            .move = move,
            .had_error = false,
        };
        (void)shell_for_each_wildcard_match(ctx,
                                            command,
                                            argv[1],
                                            shell_copy_move_match,
                                            &action,
                                            &had_error);
        return;
    }

    char source[SHELL_PATH_MAX];
    if (!resolve_path_for_command(ctx, term, command, argv[1], source, sizeof(source))) {
        return;
    }
    shell_copy_or_move_path(term,
                            command,
                            source,
                            source,
                            dest,
                            dest_is_dir,
                            move,
                            shell_path_basename(source));
}

static void cmd_mv(solar_os_context_t *ctx, int argc, char **argv)
{
    shell_cmd_copy_move(ctx, argc, argv, true);
}

static void cmd_cp(solar_os_context_t *ctx, int argc, char **argv)
{
    shell_cmd_copy_move(ctx, argc, argv, false);
}

static void cmd_reboot(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argc;
    (void)argv;
    solar_os_shell_io_writeln(term, "rebooting");
    solar_os_shell_io_flush(term);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

solar_os_shell_session_t *solar_os_shell_session_create(void)
{
    solar_os_shell_session_t *session =
        heap_caps_calloc(1, sizeof(*session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (session == NULL) {
        session = heap_caps_calloc(1, sizeof(*session), MALLOC_CAP_8BIT);
    }
    return session;
}

void solar_os_shell_session_destroy(solar_os_shell_session_t *session)
{
    if (session != NULL && session != &shell_display_session) {
        heap_caps_free(session);
    }
}

solar_os_shell_io_t *solar_os_shell_session_io(solar_os_shell_session_t *session)
{
    return session != NULL ? &session->io : NULL;
}

const solar_os_app_t *solar_os_shell_session_foreground_app(solar_os_shell_session_t *session)
{
    return session != NULL ? session->foreground_app : NULL;
}

void solar_os_shell_session_set_foreground_app(solar_os_shell_session_t *session,
                                               const solar_os_app_t *app)
{
    if (session != NULL) {
        session->foreground_app = app;
    }
}

static bool shell_scp_arg_is_remote(const char *arg)
{
    const char *colon = arg != NULL ? strchr(arg, ':') : NULL;
    return colon != NULL && colon != arg;
}

static bool shell_prepare_scp_launch_args(solar_os_context_t *ctx,
                                          int argc,
                                          char **argv,
                                          char **launch_argv,
                                          char resolved_paths[2][SHELL_PATH_MAX])
{
    int argi = 1;
    int resolved_count = 0;

    if (argc >= 4 && strcmp(argv[argi], "-P") == 0) {
        argi += 2;
    }
    if (argc - argi != 2) {
        return true;
    }

    for (int i = 0; i < 2; i++) {
        const int index = argi + i;
        if (shell_scp_arg_is_remote(argv[index])) {
            continue;
        }
        if (!resolve_path_for_command(ctx, terminal(ctx),
                                      "scp",
                                      argv[index],
                                      resolved_paths[resolved_count],
                                      SHELL_PATH_MAX)) {
            return false;
        }
        launch_argv[index] = resolved_paths[resolved_count];
        resolved_count++;
    }

    return true;
}

static bool shell_execute_line(solar_os_context_t *ctx,
                               const char *line,
                               bool add_history,
                               const char *source,
                               size_t line_number)
{
    char command[SHELL_INPUT_MAX];
    char *argv[SHELL_ARG_MAX];

    shell_session(ctx)->builtin_suppressed_prompt = false;

    strlcpy(command, line, sizeof(command));
    const int argc = tokenize(command, argv, SHELL_ARG_MAX);
    if (argc == 0) {
        return true;
    }

    if (add_history) {
        shell_history_add(ctx, line);
    }

    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        if (strcmp(argv[0], shell_builtin_commands[i].name) == 0) {
            shell_builtin_commands[i].handler(ctx, argc, argv);
            const bool should_prompt = !shell_session(ctx)->builtin_suppressed_prompt;
            if (solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT &&
                solar_os_context_take_launch_request(ctx) != NULL) {
                solar_os_shell_io_writeln(terminal(ctx),
                                          "display foreground apps are only available on the display shell");
                return true;
            }
            return should_prompt;
        }
    }

    const solar_os_app_registry_entry_t *app = solar_os_app_registry_find(argv[0]);
    if (app != NULL) {
        char *launch_argv[SHELL_ARG_MAX];
        char resolved_path[SHELL_PATH_MAX];
        char resolved_scp_paths[2][SHELL_PATH_MAX];

        if (shell_session(ctx)->watch_executing) {
            solar_os_shell_io_printf(terminal(ctx),
                                     "watch: cannot launch foreground app: %s\n",
                                     app->name);
            return true;
        }
        if (solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT &&
            (app->capabilities & SOLAR_OS_APP_CAP_PORT) == 0) {
            solar_os_shell_io_printf(terminal(ctx),
                                     "%s: display-only app; use the display shell\n",
                                     app->name);
            return true;
        }

        char owner[SOLAR_OS_APP_OWNER_MAX];
        if (solar_os_app_registry_owner(app->app, owner, sizeof(owner))) {
            solar_os_shell_io_printf(terminal(ctx),
                                     "%s: already running on %s\n",
                                     app->name,
                                     owner[0] != '\0' ? owner : "another session");
            return true;
        }

        for (int i = 0; i < argc; i++) {
            launch_argv[i] = argv[i];
        }

        if (argc >= 2 &&
            (strcmp(app->name, "edit") == 0 ||
             strcmp(app->name, "less") == 0 ||
             strcmp(app->name, "reader") == 0 ||
             strcmp(app->name, "sheet") == 0)) {
            if (!resolve_path_for_command(ctx, terminal(ctx),
                                          app->name,
                                          argv[1],
                                          resolved_path,
                                          sizeof(resolved_path))) {
                return true;
            }
            launch_argv[1] = resolved_path;
        } else if (strcmp(app->name, "plot") == 0 &&
                   argc >= 3 &&
                   (strcmp(argv[1], "-f") == 0 || strcmp(argv[1], "--file") == 0)) {
            if (!resolve_path_for_command(ctx, terminal(ctx),
                                          app->name,
                                          argv[2],
                                          resolved_path,
                                          sizeof(resolved_path))) {
                return true;
            }
            launch_argv[2] = resolved_path;
        } else if (strcmp(app->name, "scp") == 0) {
            if (!shell_prepare_scp_launch_args(ctx,
                                               argc,
                                               argv,
                                               launch_argv,
                                               resolved_scp_paths)) {
                return true;
            }
        }

        const esp_err_t err = solar_os_context_request_launch(ctx, app->app, argc, launch_argv);
        if (err == ESP_OK) {
            return false;
        }

        solar_os_shell_io_printf(terminal(ctx),
                                 "%s: launch failed: %s\n",
                                 app->name,
                                 esp_err_to_name(err));
        return true;
    }

    bool alias_matched = false;
    const bool alias_should_prompt =
        shell_try_alias(ctx, argc, argv, source, line_number, &alias_matched);
    if (alias_matched) {
        return alias_should_prompt;
    }

    if (source != NULL) {
        solar_os_shell_io_printf(terminal(ctx),
                                 "%s:%u: %s: unknown command\n",
                                 source,
                                 (unsigned)line_number,
                                 argv[0]);
    } else {
        solar_os_shell_io_printf(terminal(ctx), "%s: unknown command\n", argv[0]);
    }
    return true;
}

static bool shell_execute(solar_os_context_t *ctx, const char *line)
{
    return shell_execute_line(ctx, line, true, NULL, 0);
}

static bool shell_handle_watch_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (!shell_session(ctx)->watch_active || event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_APP_EXIT ||
            ch == SOLAR_OS_KEY_ESCAPE ||
            ch == 'q' ||
            ch == 'Q') {
            shell_watch_stop(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if ((int32_t)(now_ms - shell_session(ctx)->watch_next_ms) >= 0) {
            shell_session(ctx)->watch_next_ms = now_ms + shell_session(ctx)->watch_interval_ms;
            shell_watch_refresh(ctx);
        }
        return true;
    }

    return false;
}

static bool shell_handle_log_follow_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (!shell_session(ctx)->log_follow_active || event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_APP_EXIT ||
            ch == SOLAR_OS_KEY_ESCAPE ||
            ch == 0x03 ||
            ch == 'q' ||
            ch == 'Q') {
            shell_log_follow_stop(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if ((int32_t)(now_ms - shell_session(ctx)->log_follow_next_ms) >= 0) {
            shell_session(ctx)->log_follow_next_ms = now_ms + SHELL_LOG_FOLLOW_POLL_MS;
            shell_log_follow_print_new(ctx);
        }
        return true;
    }

    return false;
}

static void shell_handle_char(solar_os_context_t *ctx, char ch)
{
    const bool repeated_tab = ch == '\t' && shell_session(ctx)->previous_key_was_tab;

    shell_session(ctx)->previous_key_was_tab = ch == '\t';

    switch ((uint8_t)ch) {
    case SOLAR_OS_KEY_UP:
        shell_history_previous(ctx);
        break;
    case SOLAR_OS_KEY_DOWN:
        shell_history_next(ctx);
        break;
    case SOLAR_OS_KEY_LEFT:
        shell_move_cursor_left(ctx);
        break;
    case SOLAR_OS_KEY_CTRL_LEFT:
        shell_move_cursor_word_left(ctx);
        break;
    case SOLAR_OS_KEY_RIGHT:
        shell_move_cursor_right(ctx);
        break;
    case SOLAR_OS_KEY_CTRL_RIGHT:
        shell_move_cursor_word_right(ctx);
        break;
    case SOLAR_OS_KEY_HOME:
    case SOLAR_OS_KEY_CTRL_HOME:
        shell_move_cursor_home(ctx);
        break;
    case SOLAR_OS_KEY_END:
    case SOLAR_OS_KEY_CTRL_END:
        shell_move_cursor_end(ctx);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        if (solar_os_shell_io_terminal(shell_io(ctx)) != NULL) {
            solar_os_terminal_page_up(solar_os_shell_io_terminal(shell_io(ctx)));
        }
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        if (solar_os_shell_io_terminal(shell_io(ctx)) != NULL) {
            solar_os_terminal_page_down(solar_os_shell_io_terminal(shell_io(ctx)));
        }
        break;
    case SOLAR_OS_KEY_ESCAPE:
        if (shell_session(ctx)->input_len > 0) {
            shell_session(ctx)->history_browsing = false;
            shell_session(ctx)->history_index = -1;
            shell_replace_input(ctx, "");
        }
        break;
    case '\r':
    case '\n':
        solar_os_shell_io_newline(shell_io(ctx));
        if (shell_execute(ctx, shell_session(ctx)->input)) {
            shell_prompt(ctx);
        }
        break;
    case '\b':
        if (shell_session(ctx)->input_cursor > 0) {
            shell_session(ctx)->history_browsing = false;
            shell_session(ctx)->history_index = -1;
            shell_backspace(ctx);
        }
        break;
    case '\t':
        shell_complete_command(ctx, repeated_tab);
        break;
    default:
        if (shell_is_printable_char(ch) &&
            shell_session(ctx)->input_len < shell_max_input_len(ctx)) {
            shell_session(ctx)->history_browsing = false;
            shell_session(ctx)->history_index = -1;
            shell_insert_char(ctx, ch);
        }
        break;
    }
}

static bool shell_run_startup_script(solar_os_context_t *ctx)
{
    char path[SHELL_PATH_MAX];

    if (shell_session(ctx)->startup_attempted) {
        return true;
    }
    shell_session(ctx)->startup_attempted = true;

    if (!solar_os_storage_is_mounted() ||
        !shell_make_state_path(path, sizeof(path), SHELL_STARTUP_FILE)) {
        return true;
    }

    return shell_run_script(ctx, path, "/.shell/startup", false);
}

esp_err_t solar_os_shell_session_start(solar_os_context_t *ctx,
                                       solar_os_shell_session_t *session,
                                       solar_os_shell_io_t *io,
                                       bool preserve_terminal,
                                       bool run_startup)
{
    if (ctx == NULL || session == NULL || io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_context_set_shell_session(ctx, session);
    solar_os_context_set_shell_io(ctx, io);

    memset(session->input, 0, sizeof(session->input));
    memset(session->history, 0, sizeof(session->history));
    memset(session->history_draft, 0, sizeof(session->history_draft));
    session->input_len = 0;
    session->input_cursor = 0;
    session->input_row = 0;
    session->input_col = 0;
    session->input_view_offset = 0;
    session->history_count = 0;
    session->history_index = -1;
    session->history_browsing = false;
    session->previous_key_was_tab = false;
    session->builtin_suppressed_prompt = false;
    session->script_depth = 0;
    session->alias_depth = 0;
    session->watch_active = false;
    session->watch_executing = false;
    session->log_follow_active = false;
    session->watch_interval_ms = SHELL_WATCH_DEFAULT_INTERVAL_MS;
    session->watch_next_ms = 0;
    session->log_follow_next_ms = 0;
    session->log_follow_last_sequence = 0;
    session->log_follow_level = SOLAR_OS_LOG_LEVEL_INFO;
    session->foreground_app = NULL;
    session->watch_command[0] = '\0';
    if (!preserve_terminal) {
        shell_reset_cwd(session);
    }
    shell_history_load(session);
    shell_alias_ensure_file();

    if (preserve_terminal) {
        if (solar_os_shell_io_cursor_col(io) != 0) {
            solar_os_shell_io_newline(io);
        }
        shell_prompt(ctx);
        return ESP_OK;
    }

    solar_os_shell_io_clear(io);
    solar_os_shell_io_write_bold(io, "Welcome to SolarOS");
    solar_os_shell_io_newline(io);
    if (!run_startup || shell_run_startup_script(ctx)) {
        shell_prompt(ctx);
    }
    return ESP_OK;
}

bool solar_os_shell_session_event(solar_os_context_t *ctx,
                                  solar_os_shell_session_t *session,
                                  const solar_os_event_t *event)
{
    if (ctx == NULL || session == NULL) {
        return false;
    }

    solar_os_context_set_shell_session(ctx, session);
    solar_os_context_set_shell_io(ctx, &session->io);

    if (shell_handle_log_follow_event(ctx, event)) {
        return true;
    }

    if (shell_handle_watch_event(ctx, event)) {
        return true;
    }

    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    shell_handle_char(ctx, event->data.ch);
    return true;
}

void solar_os_shell_session_prompt(solar_os_context_t *ctx, solar_os_shell_session_t *session)
{
    if (ctx == NULL || session == NULL) {
        return;
    }

    solar_os_context_set_shell_session(ctx, session);
    solar_os_context_set_shell_io(ctx, &session->io);
    shell_prompt(ctx);
}

static esp_err_t shell_start(solar_os_context_t *ctx)
{
    const bool preserve_terminal = solar_os_context_take_terminal_preserve(ctx);

    solar_os_shell_io_init_terminal(&shell_display_session.io, solar_os_context_terminal(ctx));
    return solar_os_shell_session_start(ctx,
                                        &shell_display_session,
                                        &shell_display_session.io,
                                        preserve_terminal,
                                        true);
}

static bool shell_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    return solar_os_shell_session_event(ctx, &shell_display_session, event);
}

static const solar_os_app_t shell_app = {
    .name = "shell",
    .summary = "SolarOS command shell",
    .start = shell_start,
    .stop = NULL,
    .event = shell_event,
};

const solar_os_app_t *solar_os_shell_app(void)
{
    return &shell_app;
}
