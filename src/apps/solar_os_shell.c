#include "solar_os_shell.h"

#include "solar_os_shell_commands.h"
#include "solar_os_shell_completion.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell_launch.h"
#include "solar_os_shell_line.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_app_registry.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
#include "solar_os_agent.h"
#endif
#include "solar_os_board_caps.h"
#include "solar_os_clipboard.h"
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
#include "solar_os_controls.h"
#include "solar_os_parameters.h"
#endif
#include "solar_os_display.h"
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_expansion.h"
#include "solar_os_pins.h"
#endif
#if SOLAR_OS_PACKAGE_APP_FLASH
#include "solar_os_flash.h"
#endif
#include "solar_os_gpio.h"
#include "solar_os_identity.h"
#include "solar_os_input.h"
#if SOLAR_OS_PACKAGE_APP_INBOX
#include "solar_os_inbox.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
#include "solar_os_messaging.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
#include "solar_os_contacts.h"
#endif
#include "solar_os_job_registry.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_manual.h"
#include "solar_os_memory.h"
#if SOLAR_OS_PACKAGE_SERVICE_OTA
#include "solar_os_ota.h"
#endif
#include "solar_os_port.h"
#include "solar_os_port_shell.h"
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
#include "solar_os_playground.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
#include "solar_os_radio.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_LINK
#include "solar_os_link.h"
#include "solar_os_link_stream.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_MESHCORE
#include "solar_os_meshcore_stream.h"
#endif
#include "solar_os_ramfs.h"
#include "solar_os_schedule.h"
#include "solar_os_sessions.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#if SOLAR_OS_PACKAGE_SERVICE_SSH
#include "solar_os_ssh.h"
#endif
#include "solar_os_terminal.h"
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
#include "solar_os_wifi.h"
#endif

#define SHELL_INPUT_MAX 192
#define SHELL_ARG_MAX 20
#define SHELL_PATH_MAX SOLAR_OS_STORAGE_PATH_MAX
#define SHELL_HISTORY_LEN 12
#define SHELL_STATE_DIR ".shell"
#define SHELL_HISTORY_FILE "history"
#define SHELL_STARTUP_FILE "startup"
#define SHELL_ALIAS_FILE "alias"
#define SHELL_PLAYGROUND_ALIAS_FILE "playground"
#define SHELL_NVS_NAMESPACE "shell"
#define SHELL_NVS_STARTUP_SOURCE_KEY "startup_src"
#define SHELL_SCRIPT_MAX_DEPTH 3
#define SHELL_ALIAS_MAX_DEPTH 4
#define SHELL_WAIT_MAX_SECONDS 86400U
#define SHELL_WATCH_DEFAULT_INTERVAL_MS 2000U
#define SHELL_WATCH_MIN_INTERVAL_MS 1000U
#define SHELL_WATCH_MAX_INTERVAL_MS 86400000U
#define SHELL_LOG_FOLLOW_POLL_MS 250U
#define SHELL_LOG_FOLLOW_BATCH 8
#define SHELL_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define SHELL_COMPLETION_ANY "*"
/* Scripts and aliases nest dispatch, so keep large rare-path helper frames separate. */
#define SHELL_NOINLINE __attribute__((noinline))
#define SHELL_PORT_TERM_MIN_COLS 20U
#define SHELL_PORT_TERM_MIN_ROWS 8U
#define SHELL_PORT_TERM_MAX_COLS 300U
#define SHELL_PORT_TERM_MAX_ROWS 120U

typedef void (*shell_command_handler_t)(solar_os_context_t *ctx, int argc, char **argv);

typedef struct {
    const char *name;
    const char *summary;
    shell_command_handler_t handler;
} shell_command_t;

typedef struct {
    char path[SHELL_PATH_MAX];
    char scp_paths[2][SHELL_PATH_MAX];
} shell_app_launch_storage_t;

typedef enum {
    SHELL_COMPLETION_SOURCE_NONE,
    SHELL_COMPLETION_SOURCE_COMMANDS,
    SHELL_COMPLETION_SOURCE_APPS,
    SHELL_COMPLETION_SOURCE_JOBS,
    SHELL_COMPLETION_SOURCE_MANUAL_PAGES,
    SHELL_COMPLETION_SOURCE_MANUAL_REFERENCES,
    SHELL_COMPLETION_SOURCE_AGENT_CONVERSATIONS,
    SHELL_COMPLETION_SOURCE_INBOX_IDS,
    SHELL_COMPLETION_SOURCE_MESSAGE_IDS,
    SHELL_COMPLETION_SOURCE_CONTACT_IDS,
    SHELL_COMPLETION_SOURCE_ENDPOINT_IDS,
    SHELL_COMPLETION_SOURCE_PLAYGROUND_APPS,
    SHELL_COMPLETION_SOURCE_AUDIO_OUTPUTS,
    SHELL_COMPLETION_SOURCE_EXPANSION_DRIVERS,
    SHELL_COMPLETION_SOURCE_EXPANSION_DEVICES,
    SHELL_COMPLETION_SOURCE_CONNECTORS,
    SHELL_COMPLETION_SOURCE_DISPLAY_SESSION_IDS,
    SHELL_COMPLETION_SOURCE_SESSION_IDS,
    SHELL_COMPLETION_SOURCE_PORTS,
    SHELL_COMPLETION_SOURCE_RADIOS,
    SHELL_COMPLETION_SOURCE_LINKS,
    SHELL_COMPLETION_SOURCE_LINK_STREAMS,
    SHELL_COMPLETION_SOURCE_MESHCORE_STREAMS,
    SHELL_COMPLETION_SOURCE_RADIO_PROFILES,
    SHELL_COMPLETION_SOURCE_USER_RADIO_PROFILES,
    SHELL_COMPLETION_SOURCE_RAMFS_MOUNTS,
    SHELL_COMPLETION_SOURCE_STORAGE_MOUNTABLES,
    SHELL_COMPLETION_SOURCE_STORAGE_BLOCKS,
    SHELL_COMPLETION_SOURCE_STORAGE_UNMOUNT_TARGETS,
    SHELL_COMPLETION_SOURCE_DISPLAY_TARGETS,
    SHELL_COMPLETION_SOURCE_DISPLAY_MODES,
    SHELL_COMPLETION_SOURCE_INPUT_SOURCES,
    SHELL_COMPLETION_SOURCE_GPIO_PINS,
    SHELL_COMPLETION_SOURCE_I2C_ARGUMENTS,
    SHELL_COMPLETION_SOURCE_ONEWIRE_BUSES,
    SHELL_COMPLETION_SOURCE_PS2_BUSES,
    SHELL_COMPLETION_SOURCE_MIDI_BUSES,
    SHELL_COMPLETION_SOURCE_SPI_BUSES,
    SHELL_COMPLETION_SOURCE_UART_BUSES,
    SHELL_COMPLETION_SOURCE_COM_ARGUMENTS,
    SHELL_COMPLETION_SOURCE_UART_ARGUMENTS,
    SHELL_COMPLETION_SOURCE_BUSES,
    SHELL_COMPLETION_SOURCE_SPI_CS,
    SHELL_COMPLETION_SOURCE_STREAMS,
    SHELL_COMPLETION_SOURCE_SCHEDULE_ENTRIES,
    SHELL_COMPLETION_SOURCE_CONTROLS,
    SHELL_COMPLETION_SOURCE_PARAMETERS,
    SHELL_COMPLETION_SOURCE_WIFI_SSIDS,
} shell_completion_source_t;

enum {
    SHELL_COMPLETION_FLAG_PATH = 1U << 0,
    SHELL_COMPLETION_FLAG_DIRS_ONLY = 1U << 1,
    SHELL_COMPLETION_FLAG_SCALAR_STREAMS = 1U << 2,
    SHELL_COMPLETION_FLAG_ABSOLUTE_POINTERS = 1U << 3,
};

typedef struct {
    const char * const *path;
    size_t path_count;
    const char * const *values;
    size_t value_count;
    const char *required_prefix;
    shell_completion_source_t source;
    uint8_t flags;
} shell_completion_rule_t;

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
    bool prompt_on_resume;
    bool clear_on_resume;
    bool watch_active;
    bool watch_executing;
    bool log_follow_active;
    bool exit_message_pending;
    uint8_t script_depth;
    uint8_t alias_depth;
    int last_exit_code;
    uint32_t watch_interval_ms;
    uint32_t watch_next_ms;
    uint32_t log_follow_next_ms;
    uint32_t log_follow_last_sequence;
    solar_os_log_level_t log_follow_level;
    const solar_os_app_t *foreground_app;
    char watch_command[SHELL_INPUT_MAX];
    char exit_message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    solar_os_shell_io_t io;
};

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("boot foreground shell session")
static EXT_RAM_BSS_ATTR solar_os_shell_session_t shell_display_session;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared boot startup policy")
static bool shell_startup_attempted;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared boot startup policy")
static bool shell_startup_source_loaded;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared boot startup policy")
static solar_os_shell_startup_source_t shell_startup_source = SOLAR_OS_SHELL_STARTUP_FLASH;

solar_os_shell_startup_source_t solar_os_shell_startup_source(void)
{
    if (shell_startup_source_loaded) {
        return shell_startup_source;
    }

    shell_startup_source = SOLAR_OS_SHELL_STARTUP_FLASH;

    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t stored = (uint8_t)SOLAR_OS_SHELL_STARTUP_FLASH;
        if (nvs_get_u8(nvs, SHELL_NVS_STARTUP_SOURCE_KEY, &stored) == ESP_OK &&
            stored <= (uint8_t)SOLAR_OS_SHELL_STARTUP_SD) {
            const solar_os_shell_startup_source_t source =
                (solar_os_shell_startup_source_t)stored;
            if (source != SOLAR_OS_SHELL_STARTUP_SD ||
                solar_os_board_has(SOLAR_OS_BOARD_CAP_SD)) {
                shell_startup_source = source;
            }
        }
        nvs_close(nvs);
    }

    shell_startup_source_loaded = true;
    return shell_startup_source;
}

const char *solar_os_shell_startup_source_name(solar_os_shell_startup_source_t source)
{
    switch (source) {
    case SOLAR_OS_SHELL_STARTUP_FLASH:
        return "flash";
    case SOLAR_OS_SHELL_STARTUP_SD:
        return "sd";
    default:
        return "unknown";
    }
}

bool solar_os_shell_parse_startup_source(const char *name,
                                         solar_os_shell_startup_source_t *source)
{
    if (name == NULL || source == NULL) {
        return false;
    }
    if (strcmp(name, "flash") == 0) {
        *source = SOLAR_OS_SHELL_STARTUP_FLASH;
        return true;
    }
    if (strcmp(name, "sd") == 0) {
        *source = SOLAR_OS_SHELL_STARTUP_SD;
        return true;
    }
    return false;
}

esp_err_t solar_os_shell_set_startup_source(solar_os_shell_startup_source_t source)
{
    if (source != SOLAR_OS_SHELL_STARTUP_FLASH && source != SOLAR_OS_SHELL_STARTUP_SD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (source == SOLAR_OS_SHELL_STARTUP_SD &&
        !solar_os_board_has(SOLAR_OS_BOARD_CAP_SD)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, SHELL_NVS_STARTUP_SOURCE_KEY, (uint8_t)source);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        shell_startup_source = source;
        shell_startup_source_loaded = true;
    }
    return ret;
}

esp_err_t solar_os_shell_startup_path(char *path, size_t path_len)
{
    if (path == NULL || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_shell_startup_source_t source = solar_os_shell_startup_source();
    const char *base_path = source == SOLAR_OS_SHELL_STARTUP_SD ?
        solar_os_storage_sd_mount_point() :
        solar_os_storage_flash_mount_point();
    char state_dir[SHELL_PATH_MAX];
    esp_err_t ret = solar_os_storage_join_path(base_path,
                                               SHELL_STATE_DIR,
                                               state_dir,
                                               sizeof(state_dir));
    if (ret != ESP_OK) {
        return ret;
    }
    return solar_os_storage_join_path(state_dir, SHELL_STARTUP_FILE, path, path_len);
}

static void cmd_commands(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_echo(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_sh(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_wait(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_watch(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_reboot(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_exit(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_sessions(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_session(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_fg(solar_os_context_t *ctx, int argc, char **argv);
static void cmd_close(solar_os_context_t *ctx, int argc, char **argv);
static bool shell_execute_line(solar_os_context_t *ctx,
                               const char *line,
                               bool add_history,
                               const char *source,
                               size_t line_number);
static bool shell_prepare_app_launch_args(
    solar_os_context_t *ctx,
    const solar_os_app_registry_entry_t *app,
    int argc,
    char **argv,
    char **launch_argv,
    shell_app_launch_storage_t *storage);

static const shell_command_t shell_builtin_commands[] = {
    {"help", "browse or refresh the SolarOS manual", solar_os_shell_cmd_help},
    {"commands", "list shell commands", cmd_commands},
    {"man", "search the SolarOS manual", solar_os_shell_cmd_man},
    {"apps", "list applications", solar_os_shell_cmd_apps},
    {"jobs", "list background jobs", solar_os_shell_cmd_jobs},
    {"job", "control background jobs", solar_os_shell_cmd_job},
    {"sessions", "list sessions", cmd_sessions},
    {"session", "manage sessions", cmd_session},
    {"fg", "resume a display app session", cmd_fg},
    {"close", "close a session", cmd_close},
    {"version", "show SolarOS version", solar_os_shell_cmd_version},
    {"pkg", "show compiled packages", solar_os_shell_cmd_pkg},
    {"board", "show board capabilities", solar_os_shell_cmd_board},
    {"identity", "show or configure device identity", solar_os_shell_cmd_identity},
    {"input", "show input sources", solar_os_shell_cmd_input},
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
    {"engine", "show engine utilization", solar_os_shell_cmd_engine},
#endif
    {"display", "list display targets", solar_os_shell_cmd_display},
    {"clear", "clear the screen", solar_os_shell_cmd_clear},
    {"echo", "print text", cmd_echo},
    {"wait", "pause the calling shell", cmd_wait},
    {"sleep", "enter light sleep", solar_os_shell_cmd_sleep},
    {"suspend", "keep services running with the display off", solar_os_shell_cmd_suspend},
    {"power", "power profile and sleep policy", solar_os_shell_cmd_power},
    {"rtc", "real-time clock hardware", solar_os_shell_cmd_rtc},
    {"schedule", "alarms and scheduled scripts", solar_os_shell_cmd_schedule},
    {"watch", "repeat a command", cmd_watch},
    {"setterm", "configure terminal settings", solar_os_shell_cmd_setterm},
    {"status", "show system status", solar_os_shell_cmd_status},
    {"uptime", "show time since boot", solar_os_shell_cmd_uptime},
    {"mem", "show free memory", solar_os_shell_cmd_mem},
    {"nvs", "inspect or erase persistent settings", solar_os_shell_cmd_nvs},
    {"ramfs", "PSRAM-backed volatile filesystem", solar_os_shell_cmd_ramfs},
    {"stream", "list data streams", solar_os_shell_cmd_stream},
#if SOLAR_OS_PACKAGE_JOB_DAQ
    {"daq", "capture data streams", solar_os_shell_cmd_daq},
#endif
    {"log", "show SolarOS logs", solar_os_shell_cmd_log},
#if SOLAR_OS_PACKAGE_APP_AGENT
    {"agent", "native LLM agent", solar_os_shell_cmd_agent},
#endif
#if SOLAR_OS_PACKAGE_APP_INBOX
    {"inbox", "read incoming messages", solar_os_shell_cmd_inbox},
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
    {"contacts", "browse and manage contacts", solar_os_shell_cmd_contacts},
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
    {"messages", "inspect and send provider-neutral messages", solar_os_shell_cmd_messages},
    {"outbox", "list or cancel pending messages", solar_os_shell_cmd_outbox},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GATEWAY
    {"gateway", "configure the gateway messaging provider", solar_os_shell_cmd_gateway},
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
    {"email", "IMAP email client", solar_os_shell_cmd_email},
#endif
#if SOLAR_OS_PACKAGE_JOB_MESHCORE
    {"meshcore", "MeshCore identity and radio messaging",
     solar_os_shell_cmd_meshcore},
#endif
#if SOLAR_OS_PACKAGE_JOB_POCSAG
    {"pocsag", "POCSAG pager send and receive", solar_os_shell_cmd_pocsag},
#endif
    {"port", "show byte-stream ports", solar_os_shell_cmd_port},
    {"xfer", "transfer files over byte-stream ports", solar_os_shell_cmd_xfer},
    {"df", "show filesystem free space", solar_os_shell_cmd_df},
    {"disk", "persistent disk control", solar_os_shell_cmd_disk},
    {"top", "show task resource usage", solar_os_shell_cmd_top},
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    {"battery", "battery status and config", solar_os_shell_cmd_battery},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC
    {"adc", "read expansion analog inputs", solar_os_shell_cmd_adc},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    {"dpad", "ADC D-pad tools", solar_os_shell_cmd_dpad},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    {"ble", "BLE keyboard control", solar_os_shell_cmd_ble},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    {"wifi", "Wi-Fi station control", solar_os_shell_cmd_wifi},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    {"wireguard", "WireGuard VPN client", solar_os_shell_cmd_wireguard},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ESPNOW
    {"espnow", "ESP-NOW transport and peers", solar_os_shell_cmd_espnow},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
    {"mqtt", "MQTT client", solar_os_shell_cmd_mqtt},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
    {"ping", "send ICMP echo requests", solar_os_shell_cmd_ping},
    {"netscan", "scan TCP ports", solar_os_shell_cmd_netscan},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    {"audio", "audio codec tools", solar_os_shell_cmd_audio},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART
    {"uart", "UART port tools", solar_os_shell_cmd_uart},
#endif
#if SOLAR_OS_PACKAGE_JOB_MIDI
    {"midi", "MIDI transport tools", solar_os_shell_cmd_midi},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
    {"control", "map continuous controls", solar_os_shell_cmd_control},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OSC
    {"osc", "configure OSC bindings", solar_os_shell_cmd_osc},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_I2C
    {"i2c", "I2C bus tools", solar_os_shell_cmd_i2c},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_SPI
    {"spi", "SPI bus tools", solar_os_shell_cmd_spi},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO && SOLAR_OS_BOARD_HAS_STATUS_LED
    {"led", "status LED control", solar_os_shell_cmd_led},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    {"gpio", "expansion GPIO tools", solar_os_shell_cmd_gpio},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    {"onewire", "1-Wire bus tools", solar_os_shell_cmd_onewire},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PWM
    {"pwm", "expansion PWM output", solar_os_shell_cmd_pwm},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    {"expansion", "manage expansion hardware", solar_os_shell_cmd_expansion},
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
    {"neopixel", "control attached NeoPixel strips", solar_os_shell_cmd_neopixel},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
    {"radio", "packet radio tools", solar_os_shell_cmd_radio},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_LINK
    {"link", "SolarOS Link messaging", solar_os_shell_cmd_link},
#endif
#if SOLAR_OS_PACKAGE_SYSTEM_SHELL
    {"date", "read or set local date", solar_os_shell_cmd_date},
    {"time", "read or set local time", solar_os_shell_cmd_time},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
    {"ntp", "sync RTC from network time", solar_os_shell_cmd_ntp},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    {"ota", "OTA update control", solar_os_shell_cmd_ota},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SSH
    {"sshkey", "manage SSH keys", solar_os_shell_cmd_sshkey},
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    {"temperature", "read SHTC3 temperature", solar_os_shell_cmd_temperature},
    {"humidity", "read SHTC3 humidity", solar_os_shell_cmd_humidity},
#endif
#if SOLAR_OS_PACKAGE_CORE_FS_COMMANDS
    {"cd", "change directory", solar_os_shell_cmd_cd},
    {"ls", "list storage files", solar_os_shell_cmd_ls},
    {"cat", "print a small text file", solar_os_shell_cmd_cat},
    {"sh", "run a shell script", cmd_sh},
    {"mkdir", "create directories", solar_os_shell_cmd_mkdir},
    {"rm", "remove files or directories", solar_os_shell_cmd_rm},
    {"mv", "rename or move a file", solar_os_shell_cmd_mv},
    {"cp", "copy a file", solar_os_shell_cmd_cp},
    {"zip", "create ZIP archives", solar_os_shell_cmd_zip},
    {"unzip", "list or extract ZIP archives", solar_os_shell_cmd_unzip},
#endif
    {"exit", "close this port shell", cmd_exit},
    {"reboot", "restart the board", cmd_reboot},
};

static const size_t shell_builtin_command_count =
    sizeof(shell_builtin_commands) / sizeof(shell_builtin_commands[0]);

static bool shell_builtin_command_exists(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        if (strcmp(shell_builtin_commands[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static const char * const setterm_subcommands[] = {
    "--display",
    "orientation",
    "font",
    "textsize",
    "palette",
    "statusbar",
    "brightness",
    "backlight",
    "profile",
    "charset",
    "keyboard",
    "keymap",
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    "ble",
#endif
    "powerkey",
    "key",
    "keyrate",
    "typerate",
    "repeat",
    "timezone",
    "startup",
    "otaurl",
    "ota",
};

static const char * const setterm_display_settings[] = {
    "orientation", "font", "textsize", "palette", "statusbar",
};

static const char * const setterm_orientation_values[] = {"0", "90", "180", "270"};
static const char * const setterm_font_values[] = {"mono", "compact"};
static const char * const setterm_textsize_values[] = {"10", "12", "14", "16", "18", "20"};
static const char * const setterm_palette_values[] = {"normal", "inverted"};
static const char * const setterm_statusbar_values[] = {"show", "hide"};
static const char * const setterm_brightness_values[] = {"0", "25", "50", "75", "100"};
static const char * const setterm_profile_values[] = {"vt100", "ansi", "dumb"};
static const char * const setterm_charset_values[] = {"utf8", "ascii"};
static const char * const setterm_keyboard_values[] = {"us", "de"};
#if SOLAR_OS_PACKAGE_SERVICE_BLE
static const char * const setterm_ble_values[] = {"default", "on", "off"};
#endif
static const char * const setterm_powerkey_values[] = {"sleep", "suspend"};
static const char * const setterm_keyrate_values[] = {"off"};
static const char * const setterm_timezone_values[] = {"UTC", "Europe/Berlin"};
static const char * const setterm_startup_values[] = {"flash", "sd"};

static const char * const display_subcommands[] = {
    "list",
    "test",
    "mode",
};
static const char * const input_subcommands[] = {
    "status", "test", "calibrate", "keyboard", "touch", "mouse", "joystick",
    "dpad", "buttons",
};
static const char * const input_class_subcommands[] = {"status"};
static const char * const input_calibration_subcommands[] = {"set", "reset"};

#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
static const char * const engine_subcommands[] = {"status", "list", "reset"};
#endif
static const char * const mem_subcommands[] = {"policy"};
static const char * const nvs_subcommands[] = {
    "status", "list", "erase", "backup", "restore", "clear",
};
static const char * const identity_subcommands[] = {"status", "user", "hostname"};

static const char * const ble_subcommands[] = {
    "status",
    "enable",
    "disable",
    "default",
    "scan",
    "pair",
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
    "release",
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
    "enable",
    "disable",
    "on",
    "off",
    "ap",
    "scan",
    "connect",
    "disconnect",
    "known",
    "forget",
    "nat",
    "repeater",
};

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
static const char * const wireguard_subcommands[] = {
    "status",
    "import",
    "forget",
    "up",
    "down",
};

static const char * const wireguard_policy_values[] = {
    "fail-open",
    "fail-closed",
};
#endif

static const char * const wifi_ap_subcommands[] = {"status", "on", "off"};
static const char * const wifi_nat_subcommands[] = {"status", "on", "off"};
static const char * const wifi_repeater_subcommands[] = {"on", "off"};
static const char * const wifi_ap_auth_values[] = {"open", "wpa", "wpa2", "wpa/wpa2"};
static const char * const wifi_forget_values[] = {"all"};

#if SOLAR_OS_PACKAGE_SERVICE_MQTT
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

static const char * const session_subcommands[] = {
    "list",
    "ls",
    "create",
    "fg",
    "foreground",
    "switch",
    "close",
    "send",
    "focus",
    "background",
    "bg",
};

static const char * const session_create_values[] = {"shell"};
static const char * const session_shell_options[] = {"--term", "--charset", "--size"};
static const char * const session_shell_term_values[] = {"auto", "vt100", "ansi", "dumb"};
static const char * const session_shell_charset_values[] = {"utf8", "ascii"};
static const char * const session_shell_size_values[] = {"80x24", "100x30", "132x43"};

static const char * const job_log_values[] = {"file"};
static const char * const batmon_interval_values[] = {"30", "60", "300", "900"};
static const char * const ntp_sync_values[] = {"once", "60", "300", "900", "3600"};
static const char * const ntp_sync_server_values[] = {
    "pool.ntp.org",
    "time.cloudflare.com",
    "time.google.com",
};
#if SOLAR_OS_PACKAGE_JOB_EMAIL_SYNC
static const char * const email_sync_values[] = {"once", "30", "60", "300", "900", "3600"};
#endif
#if SOLAR_OS_PACKAGE_JOB_CHATD
static const char * const chatd_values[] = {"7777", "--history", "--log"};
#endif
#if SOLAR_OS_PACKAGE_JOB_TELNETD
static const char * const telnetd_values[] = {"23", "2323", "--password"};
#endif
#if SOLAR_OS_PACKAGE_JOB_SLIP
static const char * const slip_baud_values[] = {"9600", "38400", "115200", "230400", "921600"};
#endif
#if SOLAR_OS_PACKAGE_JOB_POCSAG
static const char * const pocsag_subcommands[] = {"status", "send"};
static const char * const pocsag_format_values[] = {"alpha", "numeric"};
static const char * const pocsag_polarity_values[] = {"normal", "inverted"};
#endif
#if SOLAR_OS_PACKAGE_JOB_MESHCORE
static const char * const meshcore_subcommands[] = {
    "status", "identity", "name", "advert", "channel", "stream",
};
static const char * const meshcore_identity_subcommands[] = {
    "show", "generate", "import", "export",
};
static const char * const meshcore_channel_subcommands[] = {
    "list", "add", "remove", "public",
};
static const char * const meshcore_advert_values[] = {"zero", "flood"};
static const char * const meshcore_on_off_values[] = {"off", "on"};
static const char * const meshcore_stream_subcommands[] = {
    "status", "list", "create", "remove",
};
static const char * const meshcore_stream_port_values[] = {"mser0", "mser1"};
#endif

static const char * const disk_subcommands[] = {
    "status",
    "lsblk",
    "mount",
    "umount",
    "format",
};
static const char * const disk_format_force_values[] = {"--force"};

static const char * const ramfs_subcommands[] = {
    "status",
    "mount",
    "unmount",
};

static const char * const ramfs_size_values[] = {"64k", "256k", "1m", "4m"};

static const char * const i2c_subcommands[] = {
    "status",
    "speed",
    "scan",
    "probe",
    "read",
    "write",
};

static const char * const i2c_addr_values[] = {"0x18", "0x3c", "0x68", "0x76"};
static const char * const i2c_reg_values[] = {"0x00", "0x01", "0x10"};
static const char * const i2c_len_values[] = {"1", "2", "4", "16"};
static const char * const i2c_speed_values[] = {"100000", "400000", "1000000"};
static const char * const byte_values[] = {"0x00", "0x01", "0xff"};

#if SOLAR_OS_PACKAGE_SERVICE_SPI
static const char * const spi_subcommands[] = {
    "status",
    "xfer",
    "read",
    "write",
};

static const char * const spi_mode_values[] = {"0", "1", "2", "3"};
static const char * const spi_speed_values[] = {"100k", "1m", "4m", "10m", "20m"};
static const char * const spi_fill_values[] = {"0xff", "0x00"};
#endif

static const char * const expansion_subcommands[] = {
    "status",
    "layout",
    "scan",
    "drivers",
    "devices",
    "bus",
    "attach",
    "detach",
};
static const char * const expansion_bus_subcommands[] = {"create", "attach", "detach", "remove"};
#if SOLAR_OS_PACKAGE_JOB_MIDI
static const char * const midi_subcommands[] = {
    "status", "monitor", "note-on", "note-off", "cc", "program", "send",
    "stream",
};
static const char * const midi_stream_subcommands[] = {
    "list", "add", "remove", "clear",
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
static const char * const control_subcommands[] = {
    "list", "parameters", "bindings", "create", "delete", "clear",
    "get", "set", "bind", "unbind", "parameter",
};
static const char * const control_source_values[] = {"manual"};
static const char * const control_bind_targets[] = {"midi", "parameter"};
static const char * const control_parameter_subcommands[] = {"get", "set"};
#endif
static const char * const expansion_bus_protocols[] = {
#if SOLAR_OS_PACKAGE_SERVICE_I2C
    "i2c",
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    "onewire",
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PS2
    "ps2",
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    "spi",
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART
    "midi",
    "uart",
#endif
};
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
static const char * const neopixel_subcommands[] = {
    "status",
    "list",
    "set",
    "fill",
    "clear",
    "show",
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OSC
static const char * const osc_subcommands[] = {
    "bindings", "bind", "unbind", "clear",
};
static const char * const osc_source_types[] = {"stream", "control"};
#endif

static const char * const radio_subcommands[] = {
    "status",
    "list",
    "config",
    "profile",
    "state",
    "send",
    "recv",
};
static const char * const radio_profile_subcommands[] = {
    "list",
    "show",
    "apply",
    "save",
    "remove",
};
#if SOLAR_OS_PACKAGE_SERVICE_LINK
static const char * const link_subcommands[] = {
    "status",
    "list",
    "send",
    "send-binary",
    "receive",
    "recv",
    "stream",
};
static const char * const link_stream_subcommands[] = {
    "status",
    "list",
    "create",
    "remove",
};
static const char * const link_destination_values[] = {"broadcast"};
static const char * const radio_link_names[] = {"link0"};
static const char * const radio_link_option_values[] = {
    "inbox=off",
    "inbox=on",
    "chat=off",
    "chat=on",
    "repeater=off",
    "repeater=on",
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ESPNOW
static const char * const espnow_subcommands[] = {
    "status",
    "peers",
    "list",
    "peer",
};
static const char * const espnow_peer_subcommands[] = {"add", "remove"};
#endif
#if SOLAR_OS_PACKAGE_JOB_ESPNOW_LINK
static const char * const espnow_link_names[] = {"link0", "link1"};
static const char * const espnow_link_option_values[] = {
    "channel=auto",
    "channel=1",
    "channel=2",
    "channel=3",
    "channel=4",
    "channel=5",
    "channel=6",
    "channel=7",
    "channel=8",
    "channel=9",
    "channel=10",
    "channel=11",
    "channel=12",
    "channel=13",
    "phy=normal",
    "phy=lr500",
    "phy=lr250",
    "inbox=off",
    "inbox=on",
    "chat=off",
    "chat=on",
};
#endif
static const char * const radio_config_fields[] = {
    "frequency",
    "modulation",
    "bitrate",
    "deviation",
    "bandwidth",
    "sf",
    "coding-rate",
    "power",
    "crc",
    "variable",
    "length",
    "preamble",
    "sync",
    "node",
    "network",
};
static const char * const radio_modulation_values[] = {
    "fsk",
    "gfsk",
    "msk",
    "gmsk",
    "ook",
    "lora",
};
static const char * const radio_spreading_factor_values[] = {
    "6", "7", "8", "9", "10", "11", "12",
};
static const char * const radio_coding_rate_values[] = {
    "4/5", "4/6", "4/7", "4/8",
};
static const char * const radio_state_values[] = {
    "sleep",
    "standby",
    "rx",
    "tx",
};

static const char * const uart_subcommands[] = {
    "status",
    "baud",
    "mode",
    "write",
    "read",
};

static const char * const uart_mode_values[] = {"raw", "line"};
static const char * const uart_baud_values[] = {"9600", "115200", "230400", "921600"};
static const char * const uart_read_ms_values[] = {"0", "100", "500", "1000"};

static const char * const port_subcommands[] = {
    "list",
    "status",
};

static const char * const xfer_subcommands[] = {
    "send",
    "recv",
    "protocols",
};

static const char * const xfer_options[] = {
    "--raw",
    "--protocol",
    "--delay-ms",
    "-d",
    "--append",
    "--replace",
    "--idle-ms",
};

static const char * const xfer_protocol_values[] = {"raw", "zmodem", "kermit"};

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

#if SOLAR_OS_PACKAGE_APP_INBOX
static const char * const inbox_subcommands[] = {
    "status", "list", "read", "delete", "clear", "post", "notify"
};
static const char * const inbox_list_values[] = {"all", "unread"};
static const char * const inbox_notify_values[] = {"on", "off", "test"};
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
static const char * const contacts_subcommands[] = {
    "status",
    "list",
    "show",
    "rename",
    "trust",
    "block",
    "remove",
    "link",
};
static const char * const contacts_list_values[] = {
    "all",
    "discovered",
    "trusted",
    "blocked",
};
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
static const char * const chat_selectors[] = {
    "gateway", "meshcore", "link"
};
static const char * const messages_subcommands[] = {
    "status",
    "conversations",
    "list",
    "send",
    "read",
    "delete",
    "clear",
    "outbox",
    "cancel",
};
static const char * const outbox_subcommands[] = {"list", "cancel"};
static const char * const messages_clear_values[] = {
    "gateway", "meshcore", "link", "all"
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GATEWAY
static const char * const gateway_subcommands[] = {
    "status", "configure", "connect", "disconnect", "rooms", "join",
    "leave", "delete"
};
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
static const char * const email_subcommands[] = {"status", "configure", "sync", "forget"};
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
static const char * const agent_subcommands[] = {
    "help",
    "new",
    "list",
    "resume",
    "delete",
    "status",
    "tools",
    "config",
    "forget",
    "ask",
    "script",
};
static const char * const agent_config_fields[] = {
    "endpoint",
    "model",
    "key",
    "reasoning",
    "tools",
    "max-tools",
};
static const char * const agent_key_values[] = {"clear"};
static const char * const agent_max_tools_values[] = {
    "1",
    "4",
    "8",
    "12",
    "16",
    "24",
    "32",
};
static const char * const agent_tool_policy_values[] = {
    "off",
    "readonly",
    "confirm",
    "all",
};
#if SOLAR_OS_PACKAGE_APP_PYTHON || SOLAR_OS_PACKAGE_APP_LUA
static const char * const agent_script_languages[] = {
#if SOLAR_OS_PACKAGE_APP_PYTHON
    "python",
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
    "lua",
#endif
};
#endif
static const char * const agent_reasoning_values[] = {
    "none",
    "minimal",
    "low",
    "medium",
    "high",
    "xhigh",
    "max",
};
#endif

static const char * const gpio_subcommands[] = {
    "status",
    "list",
    "mode",
    "read",
    "write",
    "release",
};

static const char * const led_subcommands[] = {
    "status",
    "on",
    "off",
    "toggle",
};

static const char * const gpio_mode_values[] = {"in", "out"};
static const char * const gpio_pull_values[] = {"none", "up", "down"};
static const char * const bit_values[] = {"0", "1"};

static const char * const onewire_subcommands[] = {"status", "reset", "scan", "xfer"};
static const char * const onewire_read_lengths[] = {"0", "1", "2", "8", "9", "16", "32"};

static const char * const adc_subcommands[] = {
    "status",
    "read",
};

static const char * const dpad_subcommands[] = {
    "status",
    "calibrate",
};

static const char * const dpad_calibrate_subcommands[] = {
    "idle",
    "reset",
};

static const char * const pwm_subcommands[] = {
    "status",
    "set",
    "off",
};
static const char * const pwm_freq_values[] = {"100", "1000", "5000"};
static const char * const pwm_duty_values[] = {"0", "25", "50", "75", "100"};

static const char * const power_subcommands[] = {
    "status",
    "profile",
    "idle",
    "key",
    "sleep",
    "suspend",
};
static const char * const rtc_subcommands[] = {
    "status", "alarm", "timer", "pending", "ack",
};
static const char * const rtc_alarm_subcommands[] = {"set", "clear"};
static const char * const rtc_timer_subcommands[] = {"set", "clear"};
static const char * const schedule_subcommands[] = {
    "list", "show", "add", "enable", "disable", "remove", "run", "stop",
};
static const char * const schedule_kinds[] = {"in", "every", "at", "daily", "weekly"};

static const char * const power_profile_values[] = {
    "performance",
    "balanced",
    "battery",
    "lowpower",
};
static const char * const power_idle_values[] = {"off"};
static const char * const power_key_values[] = {"off", "sleep", "suspend"};

static const char * const battery_subcommands[] = {
    "status",
    "config",
    "capacity",
    "min_voltage",
    "max_voltage",
};

static const char * const battery_capacity_values[] = {"500", "1000", "2000", "3000"};
static const char * const battery_min_voltage_values[] = {"3.0", "3.2", "3000", "3200"};
static const char * const battery_max_voltage_values[] = {"4.1", "4.2", "4100", "4200"};

static const char * const audio_subcommands[] = {
    "status",
    "devices",
    "device",
    "default",
    "tone",
    "tone-async",
    "queue",
    "cancel",
    "level",
    "mic",
    "loopback",
    "off",
};

static const char * const audio_hz_values[] = {"440", "880", "1000"};
static const char * const audio_ms_values[] = {"100", "500", "1000", "3000"};
static const char * const audio_volume_values[] = {"0", "25", "50", "75", "100"};

#if SOLAR_OS_PACKAGE_SERVICE_SSH
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
static const char * const stream_subcommands[] = {"list", "status"};
static const char * const daq_subcommands[] = {"help", "status", "streams", "start", "stop"};
static const char * const daq_options[] = {
    "--rate",
    "--rate-ms",
    "--changes",
    "--append",
    "--replace",
    "--raw",
};
static const char * const daq_rate_values[] = {"1", "5", "10", "60"};
static const char * const daq_rate_ms_values[] = {"0", "25", "100", "1000"};
static const char * const watch_subcommands[] = {"-n"};
#if SOLAR_OS_PACKAGE_SERVICE_NET
static const char * const ping_count_values[] = {"1", "4", "10"};
static const char * const netscan_port_values[] = {"22", "80", "443", "22,80,443", "1-1024"};
static const char * const ntp_server_values[] = {"pool.ntp.org", "time.google.com"};
#endif
static const char * const ls_options[] = {"-a", "-h", "--"};
static const char * const rm_options[] = {"-f", "-rf"};
static const char * const zip_options[] = {"-0", "--"};
static const char * const unzip_options[] = {"-l", "--"};
#if SOLAR_OS_PACKAGE_APP_APLAY
static const char * const aplay_options[] = {"-v"};
static const char * const aplay_volume_values[] = {"0", "25", "50", "75", "100"};
#endif
#if SOLAR_OS_PACKAGE_APP_ARECORD
static const char * const arecord_options[] = {"-d", "-i"};
static const char * const arecord_duration_values[] = {"1", "5", "10", "30", "60"};
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
static const char * const recorder_options[] = {"--tui"};
#endif
#if SOLAR_OS_PACKAGE_APP_CLOCK
static const char * const clock_options[] = {"-s", "-a"};
static const char * const clock_alarm_values[] = {"00:30", "01:00", "05:00", "10:00"};
#endif
#if SOLAR_OS_PACKAGE_APP_CURL
static const char * const curl_options[] = {"-L", "-o"};
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
static const char * const webradio_subcommands[] = {
    "--tui",
    "add",
    "list",
    "remove",
    "reset",
};
static const char * const webradio_tui_subcommands[] = {
    "add",
    "list",
    "remove",
    "reset",
};
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYER
static const char * const player_options[] = {"--tui"};
#endif
#if SOLAR_OS_PACKAGE_APP_FUNCGEN
static const char * const funcgen_options[] = {"--tui"};
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
static const char * const logic_rate_values[] = {"10000", "100000", "500000", "1000000", "2000000"};
static const char * const logic_sample_values[] = {"1024", "4096", "16384", "32768"};
static const char * const logic_trigger_options[] = {"trigger="};
#endif
#if SOLAR_OS_PACKAGE_APP_SCP
static const char * const scp_options[] = {"-P"};
static const char * const scp_port_values[] = {"22", "2222"};
#endif
#if SOLAR_OS_PACKAGE_APP_TELNET
static const char * const telnet_options[] = {"-r"};
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
static const char * const playground_subcommands[] = {
    "delete",
    "install",
    "refresh",
    "reload",
    "run",
    "search",
    "source",
    "storage",
};
static const char * const playground_source_values[] = {"reset"};
static const char * const playground_target_values[] = {"auto", "flash", "sd"};
static const char * const playground_storage_values[] = {"flash", "sd"};
#endif
#if SOLAR_OS_PACKAGE_MEDIA
static const char * const view_options[] = {"-fit", "-actual"};
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
static const char * const plot_options[] = {"-f", "--file", "--rate"};
static const char * const plot_live_options[] = {"--rate"};
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
static const char * const files_options[] = {"--launcher"};
static const char * const reader_options[] = {"--pager"};
#endif

static const char * const path_ls[] = {"ls"};
static const char * const path_rm[] = {"rm"};
#if SOLAR_OS_PACKAGE_APP_APLAY
static const char * const path_aplay[] = {"aplay"};
static const char * const path_aplay_volume[] = {"aplay", "-v"};
#endif
#if SOLAR_OS_PACKAGE_APP_ARECORD
static const char * const path_arecord[] = {"arecord"};
static const char * const path_arecord_duration[] = {"arecord", "-d"};
static const char * const path_arecord_input[] = {"arecord", "-i"};
#endif
#if SOLAR_OS_PACKAGE_APP_CLOCK
static const char * const path_clock[] = {"clock"};
static const char * const path_clock_alarm[] = {"clock", "-a"};
#endif
#if SOLAR_OS_PACKAGE_APP_CALC
static const char * const calc_options[] = {"--tui", "-e", "--eval"};
static const char * const path_calc[] = {"calc"};
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
static const char * const path_recorder[] = {"recorder"};
static const char * const path_recorder_tui[] = {"recorder", "--tui"};
#endif
#if SOLAR_OS_PACKAGE_APP_COM
static const char * const com_options[] = {"--autobaud", "--hex"};
static const char * const path_com[] = {"com"};
static const char * const path_com_arg[] = {"com", SHELL_COMPLETION_ANY};
static const char * const path_com_arg2[] = {
    "com",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
#endif
#if SOLAR_OS_PACKAGE_APP_CURL
static const char * const path_curl[] = {"curl"};
static const char * const path_curl_output[] = {"curl", "-o"};
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
static const char * const path_webradio[] = {"webradio"};
static const char * const path_webradio_tui[] = {"webradio", "--tui"};
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYER
static const char * const path_player[] = {"player"};
static const char * const path_player_tui[] = {"player", "--tui"};
#endif
#if SOLAR_OS_PACKAGE_APP_FUNCGEN
static const char * const path_funcgen[] = {"funcgen"};
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
static const char * const path_logic[] = {"logic"};
static const char * const path_logic_pins[] = {"logic", SHELL_COMPLETION_ANY};
static const char * const path_logic_rate[] = {
    "logic",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_logic_samples[] = {
    "logic",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
#endif
#if SOLAR_OS_PACKAGE_APP_SCP
static const char * const path_scp[] = {"scp"};
static const char * const path_scp_port[] = {"scp", "-P"};
#endif
#if SOLAR_OS_PACKAGE_APP_TELNET
static const char * const path_telnet[] = {"telnet"};
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
static const char * const path_playground[] = {"playground"};
static const char * const path_playground_install[] = {"playground", "install"};
static const char * const path_playground_run[] = {"playground", "run"};
static const char * const path_playground_run_file[] = {
    "playground",
    "run",
    SHELL_COMPLETION_ANY,
    "--file",
};
static const char * const path_playground_source[] = {"playground", "source"};
static const char * const path_playground_storage[] = {"playground", "storage"};
static const char * const path_playground_install_target[] = {
    "playground",
    "install",
    SHELL_COMPLETION_ANY,
};
#endif
static const char * const path_zip[] = {"zip"};
static const char * const path_zip_after_archive[] = {"zip", SHELL_COMPLETION_ANY};
static const char * const path_zip_after_option[] = {"zip", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY};
static const char * const path_unzip[] = {"unzip"};
static const char * const path_unzip_after_archive[] = {"unzip", SHELL_COMPLETION_ANY};
static const char * const path_unzip_after_option[] = {"unzip", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY};
#if SOLAR_OS_PACKAGE_MEDIA
static const char * const path_view[] = {"view"};
static const char * const path_view_after_option[] = {"view", SHELL_COMPLETION_ANY};
#endif
#if SOLAR_OS_PACKAGE_APP_GAMEBOY
static const char * const path_gameboy[] = {"gameboy"};
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
static const char * const path_files[] = {"files"};
static const char * const path_files_launcher[] = {"files", "--launcher"};
#endif
#if SOLAR_OS_PACKAGE_APP_NOTES
static const char * const path_notes[] = {"notes"};
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
static const char * const path_plot[] = {"plot"};
static const char * const path_plot_file[] = {"plot", "-f"};
static const char * const path_plot_long_file[] = {"plot", "--file"};
static const char * const path_plot_stream[] = {"plot", SHELL_COMPLETION_ANY};
#endif
#if SOLAR_OS_PACKAGE_APP_READER
static const char * const path_reader[] = {"reader"};
static const char * const path_reader_pager[] = {"reader", "--pager"};
#endif
#if SOLAR_OS_PACKAGE_APP_WRITER
static const char * const path_writer[] = {"writer"};
#endif
static const char * const path_watch[] = {"watch"};
static const char * const path_watch_n_interval[] = {"watch", "-n", SHELL_COMPLETION_ANY};
static const char * const path_man[] = {"man"};
static const char * const man_options[] = {"--list", "--apropos", "-k"};
static const char * const path_help[] = {"help"};
static const char * const help_subcommands[] = {"status", "update", "reset"};
static const char * const path_setterm[] = {"setterm"};
static const char * const path_setterm_display[] = {"setterm", "--display"};
static const char * const path_setterm_display_target[] = {
    "setterm", "--display", SHELL_COMPLETION_ANY,
};
static const char * const path_setterm_display_orientation[] = {
    "setterm", "--display", SHELL_COMPLETION_ANY, "orientation",
};
static const char * const path_setterm_display_font[] = {
    "setterm", "--display", SHELL_COMPLETION_ANY, "font",
};
static const char * const path_setterm_display_textsize[] = {
    "setterm", "--display", SHELL_COMPLETION_ANY, "textsize",
};
static const char * const path_setterm_display_palette[] = {
    "setterm", "--display", SHELL_COMPLETION_ANY, "palette",
};
static const char * const path_setterm_display_statusbar[] = {
    "setterm", "--display", SHELL_COMPLETION_ANY, "statusbar",
};
static const char * const path_setterm_orientation[] = {"setterm", "orientation"};
static const char * const path_setterm_font[] = {"setterm", "font"};
static const char * const path_setterm_textsize[] = {"setterm", "textsize"};
static const char * const path_setterm_palette[] = {"setterm", "palette"};
static const char * const path_setterm_statusbar[] = {"setterm", "statusbar"};
static const char * const path_setterm_brightness[] = {"setterm", "brightness"};
static const char * const path_setterm_backlight[] = {"setterm", "backlight"};
static const char * const path_setterm_profile[] = {"setterm", "profile"};
static const char * const path_setterm_charset[] = {"setterm", "charset"};
static const char * const path_setterm_keyboard[] = {"setterm", "keyboard"};
static const char * const path_setterm_keymap[] = {"setterm", "keymap"};
#if SOLAR_OS_PACKAGE_SERVICE_BLE
static const char * const path_setterm_ble[] = {"setterm", "ble"};
#endif
static const char * const path_setterm_powerkey[] = {"setterm", "powerkey"};
static const char * const path_setterm_key[] = {"setterm", "key"};
static const char * const path_setterm_keyrate[] = {"setterm", "keyrate"};
static const char * const path_setterm_typerate[] = {"setterm", "typerate"};
static const char * const path_setterm_repeat[] = {"setterm", "repeat"};
static const char * const path_setterm_timezone[] = {"setterm", "timezone"};
static const char * const path_setterm_startup[] = {"setterm", "startup"};
static const char * const path_display[] = {"display"};
static const char * const path_display_test[] = {"display", "test"};
static const char * const path_display_mode[] = {"display", "mode"};
static const char * const path_display_mode_target[] = {"display", "mode", SHELL_COMPLETION_ANY};
static const char * const path_input[] = {"input"};
static const char * const path_input_test[] = {"input", "test"};
static const char * const path_input_calibrate[] = {"input", "calibrate"};
static const char * const path_input_keyboard[] = {"input", "keyboard"};
static const char * const path_input_touch[] = {"input", "touch"};
static const char * const path_input_mouse[] = {"input", "mouse"};
static const char * const path_input_joystick[] = {"input", "joystick"};
static const char * const path_input_dpad[] = {"input", "dpad"};
static const char * const path_input_buttons[] = {"input", "buttons"};
static const char * const path_input_calibrate_source[] = {
    "input", "calibrate", SHELL_COMPLETION_ANY,
};
#if SOLAR_OS_PACKAGE_APP_INBOX
static const char * const path_inbox[] = {"inbox"};
static const char * const path_inbox_list[] = {"inbox", "list"};
static const char * const path_inbox_read[] = {"inbox", "read"};
static const char * const path_inbox_delete[] = {"inbox", "delete"};
static const char * const path_inbox_notify[] = {"inbox", "notify"};
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
static const char * const path_contacts[] = {"contacts"};
static const char * const path_contacts_list[] = {"contacts", "list"};
static const char * const path_contacts_show[] = {"contacts", "show"};
static const char * const path_contacts_rename[] = {"contacts", "rename"};
static const char * const path_contacts_trust[] = {"contacts", "trust"};
static const char * const path_contacts_trust_endpoint[] = {
    "contacts", "trust", SHELL_COMPLETION_ANY
};
static const char * const path_contacts_block[] = {"contacts", "block"};
static const char * const path_contacts_block_endpoint[] = {
    "contacts", "block", SHELL_COMPLETION_ANY
};
static const char * const path_contacts_remove[] = {"contacts", "remove"};
static const char * const path_contacts_link[] = {"contacts", "link"};
static const char * const path_contacts_link_source[] = {
    "contacts", "link", SHELL_COMPLETION_ANY
};
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
static const char * const path_chat[] = {"chat"};
static const char * const path_messages[] = {"messages"};
static const char * const path_messages_delete[] = {"messages", "delete"};
static const char * const path_messages_cancel[] = {"messages", "cancel"};
static const char * const path_messages_clear[] = {"messages", "clear"};
static const char * const path_outbox[] = {"outbox"};
static const char * const path_outbox_cancel[] = {"outbox", "cancel"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GATEWAY
static const char * const path_gateway[] = {"gateway"};
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
static const char * const path_email[] = {"email"};
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
static const char * const path_agent[] = {"agent"};
static const char * const path_agent_resume[] = {"agent", "resume"};
static const char * const path_agent_delete[] = {"agent", "delete"};
static const char * const path_agent_config[] = {"agent", "config"};
static const char * const path_agent_script[] = {"agent", "script"};
#if SOLAR_OS_PACKAGE_APP_PYTHON || SOLAR_OS_PACKAGE_APP_LUA
static const char * const path_agent_script_language[] = {
    "agent",
    "script",
    SHELL_COMPLETION_ANY,
};
static const char * const agent_script_options[] = {"-c"};
#endif
static const char * const path_agent_config_key[] = {"agent", "config", "key"};
static const char * const path_agent_config_reasoning[] = {
    "agent",
    "config",
    "reasoning",
};
static const char * const path_agent_config_tools[] = {
    "agent",
    "config",
    "tools",
};
static const char * const path_agent_config_max_tools[] = {
    "agent",
    "config",
    "max-tools",
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
static const char * const path_engine[] = {"engine"};
#endif
static const char * const path_mem[] = {"mem"};
static const char * const path_nvs[] = {"nvs"};
static const char * const path_nvs_backup[] = {"nvs", "backup"};
static const char * const path_nvs_restore[] = {"nvs", "restore"};
static const char * const path_identity[] = {"identity"};
static const char * const path_fg[] = {"fg"};
static const char * const path_close[] = {"close"};
static const char * const path_job[] = {"job"};
static const char * const path_job_status[] = {"job", "status"};
static const char * const path_job_start[] = {"job", "start"};
static const char * const path_job_start_batmon[] = {"job", "start", "batmon"};
static const char * const path_job_start_log[] = {"job", "start", "log"};
static const char * const path_job_start_log_port[] = {"job", "start", "log", SHELL_COMPLETION_ANY};
static const char * const path_job_start_log_file[] = {"job", "start", "log", "file"};
static const char * const path_job_start_bridge[] = {"job", "start", "bridge"};
static const char * const path_job_start_bridge_port[] = {"job", "start", "bridge", SHELL_COMPLETION_ANY};
#if SOLAR_OS_PACKAGE_JOB_GPIO_KEYS
static const char * const gpio_keys_options[] = {"--config"};
static const char * const path_job_start_gpio_keys[] = {"job", "start", "gpio-keys"};
static const char * const path_job_start_gpio_keys_config[] = {
    "job", "start", "gpio-keys", "--config"
};
#endif
#if SOLAR_OS_PACKAGE_JOB_PS2_KEYBOARD
static const char * const path_job_start_ps2_keyboard[] = {
    "job", "start", "ps2-keyboard"
};
#endif
#if SOLAR_OS_PACKAGE_JOB_MIDI
static const char * const path_job_start_midi[] = {"job", "start", "midi"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_LINK
static const char * const path_job_start_bridge_link[] = {
    "job", "start", "bridge", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY
};
#endif
static const char * const path_job_start_httpd[] = {"job", "start", "httpd"};
#if SOLAR_OS_PACKAGE_JOB_DISPLAYD
static const char * const path_job_start_displayd[] = {"job", "start", "displayd"};
#endif
static const char * const path_job_start_ntp_sync[] = {"job", "start", "ntp-sync"};
static const char * const path_job_start_ntp_sync_interval[] = {
    "job",
    "start",
    "ntp-sync",
    SHELL_COMPLETION_ANY,
};
#if SOLAR_OS_PACKAGE_JOB_CHATD
static const char * const path_job_start_chatd[] = {"job", "start", "chatd"};
static const char * const path_job_start_chatd_history[] = {
    "job",
    "start",
    "chatd",
    "--history",
};
static const char * const path_job_start_chatd_log[] = {
    "job",
    "start",
    "chatd",
    "--log",
};
#endif
#if SOLAR_OS_PACKAGE_JOB_TELNETD
static const char * const path_job_start_telnetd[] = {"job", "start", "telnetd"};
#endif
#if SOLAR_OS_PACKAGE_JOB_EMAIL_SYNC
static const char * const path_job_start_email_sync[] = {"job", "start", "email-sync"};
#endif
#if SOLAR_OS_PACKAGE_JOB_POCSAG
static const char * const path_job_start_pocsag[] = {"job", "start", "pocsag"};
static const char * const path_pocsag[] = {"pocsag"};
static const char * const path_pocsag_send[] = {"pocsag", "send"};
static const char * const path_pocsag_send_message[] = {
    "pocsag", "send", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
};
static const char * const path_pocsag_send_format[] = {
    "pocsag", "send", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
#endif
#if SOLAR_OS_PACKAGE_JOB_RADIO_LINK
static const char * const path_job_start_radio_link[] = {
    "job", "start", "radio-link"
};
static const char * const path_job_start_radio_link_name[] = {
    "job", "start", "radio-link", SHELL_COMPLETION_ANY
};
static const char * const path_job_start_radio_link_radio[] = {
    "job", "start", "radio-link", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY
};
static const char * const path_job_start_radio_link_profile[] = {
    "job", "start", "radio-link", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY
};
static const char * const path_job_start_radio_link_option[] = {
    "job", "start", "radio-link", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY
};
static const char * const path_job_start_radio_link_option_2[] = {
    "job", "start", "radio-link", SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY
};
#endif
#if SOLAR_OS_PACKAGE_JOB_ESPNOW_LINK
static const char * const path_job_start_espnow_link[] = {
    "job", "start", "espnow-link"
};
static const char * const path_job_start_espnow_link_name[] = {
    "job", "start", "espnow-link", SHELL_COMPLETION_ANY
};
static const char * const path_job_start_espnow_link_option[] = {
    "job", "start", "espnow-link", SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY
};
static const char * const path_job_start_espnow_link_option2[] = {
    "job", "start", "espnow-link", SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY
};
static const char * const path_job_start_espnow_link_option3[] = {
    "job", "start", "espnow-link", SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY
};
static const char * const path_job_start_espnow_link_option4[] = {
    "job", "start", "espnow-link", SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY, SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY
};
#endif
#if SOLAR_OS_PACKAGE_JOB_MESHCORE
static const char * const path_job_start_meshcore[] = {
    "job", "start", "meshcore"
};
static const char * const path_job_start_meshcore_radio[] = {
    "job", "start", "meshcore", SHELL_COMPLETION_ANY
};
static const char * const path_meshcore[] = {"meshcore"};
static const char * const path_meshcore_identity[] = {
    "meshcore", "identity"
};
static const char * const path_meshcore_advert[] = {
    "meshcore", "advert"
};
static const char * const path_meshcore_channel[] = {
    "meshcore", "channel"
};
static const char * const path_meshcore_channel_public[] = {
    "meshcore", "channel", "public"
};
static const char * const path_meshcore_stream[] = {
    "meshcore", "stream"
};
static const char * const path_meshcore_stream_status[] = {
    "meshcore", "stream", "status"
};
static const char * const path_meshcore_stream_create[] = {
    "meshcore", "stream", "create"
};
static const char * const path_meshcore_stream_create_port[] = {
    "meshcore", "stream", "create", SHELL_COMPLETION_ANY
};
static const char * const path_meshcore_stream_remove[] = {
    "meshcore", "stream", "remove"
};
#endif
static const char * const path_job_start_slip[] = {"job", "start", "slip"};
static const char * const path_job_start_slip_port[] = {
    "job",
    "start",
    "slip",
    SHELL_COMPLETION_ANY,
};
static const char * const path_job_start_sump[] = {"job", "start", "sump"};
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
static const char * const path_session[] = {"session"};
static const char * const path_session_create[] = {"session", "create"};
static const char * const path_session_create_shell[] = {"session", "create", "shell"};
static const char * const path_session_create_app[] = {
    "session",
    "create",
    SHELL_COMPLETION_ANY,
};
static const char * const path_session_create_shell_target[] = {
    "session",
    "create",
    "shell",
    SHELL_COMPLETION_ANY,
};
static const char * const path_session_create_shell_term[] = {
    "session",
    "create",
    "shell",
    SHELL_COMPLETION_ANY,
    "--term",
};
static const char * const path_session_create_shell_size[] = {
    "session",
    "create",
    "shell",
    SHELL_COMPLETION_ANY,
    "--size",
};
static const char * const path_session_create_shell_charset[] = {
    "session",
    "create",
    "shell",
    SHELL_COMPLETION_ANY,
    "--charset",
};
static const char * const path_session_fg[] = {"session", "fg"};
static const char * const path_session_foreground[] = {"session", "foreground"};
static const char * const path_session_switch[] = {"session", "switch"};
static const char * const path_session_close[] = {"session", "close"};
static const char * const path_session_send[] = {"session", "send"};
static const char * const path_session_focus[] = {"session", "focus"};
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
static const char * const path_wifi_connect[] = {"wifi", "connect"};
static const char * const path_wifi_nat[] = {"wifi", "nat"};
static const char * const path_wifi_repeater[] = {"wifi", "repeater"};
static const char * const path_wifi_forget[] = {"wifi", "forget"};
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
static const char * const path_wireguard[] = {"wireguard"};
static const char * const path_wireguard_up[] = {"wireguard", "up"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
static const char * const path_mqtt[] = {"mqtt"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
static const char * const path_ping_count[] = {"ping", SHELL_COMPLETION_ANY};
static const char * const path_netscan_ports[] = {"netscan", SHELL_COMPLETION_ANY};
static const char * const path_ntp[] = {"ntp"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
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
static const char * const path_disk[] = {"disk"};
static const char * const path_disk_mount[] = {"disk", "mount"};
static const char * const path_disk_mount_target[] = {"disk", "mount", SHELL_COMPLETION_ANY};
static const char * const path_disk_umount[] = {"disk", "umount"};
static const char * const path_disk_format[] = {"disk", "format"};
static const char * const path_disk_format_force[] = {
    "disk", "format", SHELL_COMPLETION_ANY,
};
static const char * const path_ramfs[] = {"ramfs"};
static const char * const path_ramfs_mount_path[] = {"ramfs", "mount", SHELL_COMPLETION_ANY};
static const char * const path_ramfs_unmount[] = {"ramfs", "unmount"};
static const char * const path_i2c[] = {"i2c"};
static const char * const path_i2c_status[] = {"i2c", "status"};
static const char * const path_i2c_speed[] = {"i2c", "speed"};
static const char * const path_i2c_scan[] = {"i2c", "scan"};
static const char * const path_i2c_probe[] = {"i2c", "probe"};
static const char * const path_i2c_probe_bus[] = {"i2c", "probe", SHELL_COMPLETION_ANY};
static const char * const path_i2c_read[] = {"i2c", "read"};
static const char * const path_i2c_read_addr[] = {"i2c", "read", SHELL_COMPLETION_ANY};
static const char * const path_i2c_read_reg[] = {
    "i2c",
    "read",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_i2c_read_bus_reg[] = {
    "i2c",
    "read",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_i2c_write[] = {"i2c", "write"};
static const char * const path_i2c_write_addr[] = {"i2c", "write", SHELL_COMPLETION_ANY};
static const char * const path_i2c_write_reg[] = {
    "i2c",
    "write",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_i2c_write_bus_reg[] = {
    "i2c",
    "write",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
#if SOLAR_OS_PACKAGE_SERVICE_SPI
static const char * const path_spi[] = {"spi"};
static const char * const path_spi_status[] = {"spi", "status"};
static const char * const path_spi_xfer[] = {"spi", "xfer"};
static const char * const path_spi_xfer_bus[] = {"spi", "xfer", SHELL_COMPLETION_ANY};
static const char * const path_spi_xfer_cs[] = {
    "spi",
    "xfer",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_spi_xfer_mode[] = {
    "spi",
    "xfer",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_spi_read[] = {"spi", "read"};
static const char * const path_spi_read_bus[] = {"spi", "read", SHELL_COMPLETION_ANY};
static const char * const path_spi_read_cs[] = {
    "spi",
    "read",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_spi_read_mode[] = {
    "spi",
    "read",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_spi_read_len[] = {
    "spi",
    "read",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_spi_write[] = {"spi", "write"};
static const char * const path_spi_write_bus[] = {"spi", "write", SHELL_COMPLETION_ANY};
static const char * const path_spi_write_cs[] = {
    "spi",
    "write",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_spi_write_mode[] = {
    "spi",
    "write",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
#endif
static const char * const path_uart[] = {"uart"};
static const char * const path_uart_status[] = {"uart", "status"};
static const char * const path_uart_baud[] = {"uart", "baud"};
static const char * const path_uart_mode[] = {"uart", "mode"};
static const char * const path_uart_write[] = {"uart", "write"};
static const char * const path_uart_read[] = {"uart", "read"};
static const char * const path_port[] = {"port"};
static const char * const path_port_status[] = {"port", "status"};
static const char * const path_xfer[] = {"xfer"};
static const char * const path_xfer_send[] = {"xfer", "send"};
static const char * const path_xfer_recv[] = {"xfer", "recv"};
static const char * const path_xfer_send_port[] = {"xfer", "send", SHELL_COMPLETION_ANY};
static const char * const path_xfer_recv_port[] = {"xfer", "recv", SHELL_COMPLETION_ANY};
static const char * const path_xfer_send_port_file[] = {
    "xfer",
    "send",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_xfer_recv_port_file[] = {
    "xfer",
    "recv",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_xfer_send_protocol[] = {
    "xfer",
    "send",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    "--protocol",
};
static const char * const path_xfer_recv_protocol[] = {
    "xfer",
    "recv",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
    "--protocol",
};
static const char * const path_log[] = {"log"};
static const char * const path_log_follow[] = {"log", "follow"};
static const char * const path_log_level[] = {"log", "level"};
static const char * const path_log_sink[] = {"log", "sink"};
static const char * const path_log_sink_cdc[] = {"log", "sink", "cdc"};
static const char * const path_led[] = {"led"};
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
static const char * const path_gpio_release[] = {"gpio", "release"};
static const char * const path_onewire[] = {"onewire"};
static const char * const path_onewire_status[] = {"onewire", "status"};
static const char * const path_onewire_reset[] = {"onewire", "reset"};
static const char * const path_onewire_scan[] = {"onewire", "scan"};
static const char * const path_onewire_xfer[] = {"onewire", "xfer"};
static const char * const path_onewire_xfer_pin[] = {
    "onewire",
    "xfer",
    SHELL_COMPLETION_ANY,
};
static const char * const path_onewire_xfer_len[] = {
    "onewire",
    "xfer",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_adc[] = {"adc"};
static const char * const path_adc_read[] = {"adc", "read"};
static const char * const path_dpad[] = {"dpad"};
static const char * const path_dpad_calibrate[] = {"dpad", "calibrate"};
static const char * const path_pwm[] = {"pwm"};
static const char * const path_pwm_set[] = {"pwm", "set"};
static const char * const path_pwm_set_pin[] = {"pwm", "set", SHELL_COMPLETION_ANY};
static const char * const path_pwm_set_freq[] = {
    "pwm",
    "set",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_pwm_off[] = {"pwm", "off"};
static const char * const path_expansion[] = {"expansion"};
static const char * const path_expansion_layout[] = {"expansion", "layout"};
static const char * const path_expansion_bus[] = {"expansion", "bus"};
static const char * const path_expansion_bus_create[] = {"expansion", "bus", "create"};
static const char * const path_expansion_bus_attach[] = {"expansion", "bus", "attach"};
static const char * const path_expansion_bus_detach[] = {"expansion", "bus", "detach"};
static const char * const path_expansion_bus_remove[] = {"expansion", "bus", "remove"};
static const char * const path_expansion_attach[] = {"expansion", "attach"};
static const char * const path_expansion_detach[] = {"expansion", "detach"};
#if SOLAR_OS_PACKAGE_JOB_MIDI
static const char * const path_midi[] = {"midi"};
static const char * const path_midi_stream[] = {"midi", "stream"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
static const char * const path_control[] = {"control"};
static const char * const path_control_create_source[] = {
    "control", "create", SHELL_COMPLETION_ANY
};
static const char * const path_control_bind[] = {"control", "bind"};
static const char * const path_control_bind_target[] = {
    "control", "bind", SHELL_COMPLETION_ANY
};
static const char * const path_control_bind_parameter[] = {
    "control", "bind", SHELL_COMPLETION_ANY, "parameter"
};
static const char * const path_control_get[] = {"control", "get"};
static const char * const path_control_set[] = {"control", "set"};
static const char * const path_control_unbind[] = {"control", "unbind"};
static const char * const path_control_parameter[] = {"control", "parameter"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OSC
static const char * const path_osc[] = {"osc"};
static const char * const path_osc_bind_type[] = {
    "osc", "bind", SHELL_COMPLETION_ANY
};
static const char * const path_osc_bind_stream[] = {
    "osc", "bind", SHELL_COMPLETION_ANY, "stream"
};
static const char * const path_osc_bind_control[] = {
    "osc", "bind", SHELL_COMPLETION_ANY, "control"
};
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
static const char * const path_neopixel[] = {"neopixel"};
static const char * const path_neopixel_status[] = {"neopixel", "status"};
static const char * const path_neopixel_list[] = {"neopixel", "list"};
static const char * const path_neopixel_set[] = {"neopixel", "set"};
static const char * const path_neopixel_fill[] = {"neopixel", "fill"};
static const char * const path_neopixel_clear[] = {"neopixel", "clear"};
static const char * const path_neopixel_show[] = {"neopixel", "show"};
#endif
static const char * const path_radio[] = {"radio"};
static const char * const path_radio_status[] = {"radio", "status"};
static const char * const path_radio_config[] = {"radio", "config"};
static const char * const path_radio_config_name[] = {"radio", "config", SHELL_COMPLETION_ANY};
static const char * const path_radio_config_modulation[] = {"radio", "config", SHELL_COMPLETION_ANY, "modulation"};
static const char * const path_radio_config_crc[] = {"radio", "config", SHELL_COMPLETION_ANY, "crc"};
static const char * const path_radio_config_variable[] = {"radio", "config", SHELL_COMPLETION_ANY, "variable"};
static const char * const path_radio_config_sf[] = {"radio", "config", SHELL_COMPLETION_ANY, "sf"};
static const char * const path_radio_config_coding_rate[] = {"radio", "config", SHELL_COMPLETION_ANY, "coding-rate"};
static const char * const path_radio_profile[] = {"radio", "profile"};
static const char * const path_radio_profile_show[] = {"radio", "profile", "show"};
static const char * const path_radio_profile_apply[] = {"radio", "profile", "apply"};
static const char * const path_radio_profile_apply_radio[] = {
    "radio", "profile", "apply", SHELL_COMPLETION_ANY
};
static const char * const path_radio_profile_save[] = {"radio", "profile", "save"};
static const char * const path_radio_profile_save_radio[] = {
    "radio", "profile", "save", SHELL_COMPLETION_ANY
};
static const char * const path_radio_profile_remove[] = {"radio", "profile", "remove"};
static const char * const path_radio_state[] = {"radio", "state"};
static const char * const path_radio_state_name[] = {"radio", "state", SHELL_COMPLETION_ANY};
static const char * const path_radio_send[] = {"radio", "send"};
static const char * const path_radio_recv[] = {"radio", "recv"};
#if SOLAR_OS_PACKAGE_SERVICE_ESPNOW
static const char * const path_espnow[] = {"espnow"};
static const char * const path_espnow_peer[] = {"espnow", "peer"};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_LINK
static const char * const path_link[] = {"link"};
static const char * const path_link_status[] = {"link", "status"};
static const char * const path_link_send[] = {"link", "send"};
static const char * const path_link_send_link[] = {
    "link", "send", SHELL_COMPLETION_ANY
};
static const char * const path_link_send_binary[] = {"link", "send-binary"};
static const char * const path_link_send_binary_link[] = {
    "link", "send-binary", SHELL_COMPLETION_ANY
};
static const char * const path_link_receive[] = {"link", "receive"};
static const char * const path_link_recv[] = {"link", "recv"};
static const char * const path_link_stream[] = {"link", "stream"};
static const char * const path_link_stream_status[] = {"link", "stream", "status"};
static const char * const path_link_stream_create[] = {"link", "stream", "create"};
static const char * const path_link_stream_remove[] = {"link", "stream", "remove"};
#endif
static const char * const path_power[] = {"power"};
static const char * const path_power_profile[] = {"power", "profile"};
static const char * const path_power_idle[] = {"power", "idle"};
static const char * const path_power_key[] = {"power", "key"};
static const char * const path_rtc[] = {"rtc"};
static const char * const path_rtc_alarm[] = {"rtc", "alarm"};
static const char * const path_rtc_timer[] = {"rtc", "timer"};
static const char * const path_schedule[] = {"schedule"};
static const char * const path_schedule_add_name[] = {"schedule", "add", "*"};
static const char * const path_schedule_show[] = {"schedule", "show"};
static const char * const path_schedule_enable[] = {"schedule", "enable"};
static const char * const path_schedule_disable[] = {"schedule", "disable"};
static const char * const path_schedule_remove[] = {"schedule", "remove"};
static const char * const path_schedule_run[] = {"schedule", "run"};
static const char * const path_schedule_stop[] = {"schedule", "stop"};
static const char * const path_battery[] = {"battery"};
static const char * const path_battery_capacity[] = {"battery", "capacity"};
static const char * const path_battery_min_voltage[] = {"battery", "min_voltage"};
static const char * const path_battery_max_voltage[] = {"battery", "max_voltage"};
static const char * const path_audio[] = {"audio"};
static const char * const path_audio_default[] = {"audio", "default"};
static const char * const path_audio_tone[] = {"audio", "tone"};
static const char * const path_audio_tone_hz[] = {"audio", "tone", SHELL_COMPLETION_ANY};
static const char * const path_audio_tone_ms[] = {
    "audio",
    "tone",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_audio_tone_async[] = {"audio", "tone-async"};
static const char * const path_audio_tone_async_hz[] = {
    "audio", "tone-async", SHELL_COMPLETION_ANY,
};
static const char * const path_audio_tone_async_ms[] = {
    "audio",
    "tone-async",
    SHELL_COMPLETION_ANY,
    SHELL_COMPLETION_ANY,
};
static const char * const path_audio_level[] = {"audio", "level"};
static const char * const path_audio_mic[] = {"audio", "mic"};
static const char * const path_audio_loopback[] = {"audio", "loopback"};
static const char * const path_audio_loopback_ms[] = {"audio", "loopback", SHELL_COMPLETION_ANY};
#if SOLAR_OS_PACKAGE_SERVICE_SSH
static const char * const path_sshkey[] = {"sshkey"};
static const char * const path_sshkey_gen[] = {"sshkey", "gen"};
static const char * const path_sshkey_gen_force[] = {"sshkey", "gen", "-f"};
#endif
static const char * const path_ota[] = {"ota"};
static const char * const path_ota_boot[] = {"ota", "boot"};

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
        .source = SHELL_COMPLETION_SOURCE_COMMANDS, \
    }
#define SHELL_COMPLETION_APPS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_APPS, \
    }
#define SHELL_COMPLETION_JOBS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_JOBS, \
    }
#define SHELL_COMPLETION_AGENT_CONVERSATIONS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_AGENT_CONVERSATIONS, \
    }
#define SHELL_COMPLETION_INBOX_IDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_INBOX_IDS, \
    }
#define SHELL_COMPLETION_MESSAGE_IDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_MESSAGE_IDS, \
    }
#define SHELL_COMPLETION_CONTACT_IDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_CONTACT_IDS, \
    }
#define SHELL_COMPLETION_ENDPOINT_IDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_ENDPOINT_IDS, \
    }
#define SHELL_COMPLETION_PLAYGROUND_APPS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_PLAYGROUND_APPS, \
    }
#define SHELL_COMPLETION_AUDIO_OUTPUTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_AUDIO_OUTPUTS, \
    }
#define SHELL_COMPLETION_EXPANSION_DRIVERS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_EXPANSION_DRIVERS, \
    }
#define SHELL_COMPLETION_EXPANSION_DEVICES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_EXPANSION_DEVICES, \
    }
#define SHELL_COMPLETION_CONNECTORS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_CONNECTORS, \
    }
#define SHELL_COMPLETION_MANUAL(path_array, value_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .values = value_array, \
        .value_count = SHELL_ARRAY_COUNT(value_array), \
        .source = SHELL_COMPLETION_SOURCE_MANUAL_PAGES, \
    }
#define SHELL_COMPLETION_MANUAL_REFERENCES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .required_prefix = "man:", \
        .source = SHELL_COMPLETION_SOURCE_MANUAL_REFERENCES, \
    }
#define SHELL_COMPLETION_DISPLAY_SESSION_IDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_DISPLAY_SESSION_IDS, \
    }
#define SHELL_COMPLETION_SESSION_IDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_SESSION_IDS, \
    }
#define SHELL_COMPLETION_PORTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_PORTS, \
    }
#define SHELL_COMPLETION_RADIOS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_RADIOS, \
    }
#define SHELL_COMPLETION_LINKS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_LINKS, \
    }
#define SHELL_COMPLETION_LINK_STREAMS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_LINK_STREAMS, \
    }
#define SHELL_COMPLETION_MESHCORE_STREAMS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_MESHCORE_STREAMS, \
    }
#define SHELL_COMPLETION_RADIO_PROFILES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_RADIO_PROFILES, \
    }
#define SHELL_COMPLETION_USER_RADIO_PROFILES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_USER_RADIO_PROFILES, \
    }
#define SHELL_COMPLETION_RAMFS_MOUNTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_RAMFS_MOUNTS, \
    }
#define SHELL_COMPLETION_STORAGE_MOUNTABLES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_STORAGE_MOUNTABLES, \
    }
#define SHELL_COMPLETION_STORAGE_BLOCKS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_STORAGE_BLOCKS, \
    }
#define SHELL_COMPLETION_STORAGE_UNMOUNT_TARGETS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_STORAGE_UNMOUNT_TARGETS, \
    }
#define SHELL_COMPLETION_DISPLAY_TARGETS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_DISPLAY_TARGETS, \
    }
#define SHELL_COMPLETION_DISPLAY_MODES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_DISPLAY_MODES, \
    }
#define SHELL_COMPLETION_INPUT_SOURCES(path_array, absolute_only) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_INPUT_SOURCES, \
        .flags = (absolute_only) ? SHELL_COMPLETION_FLAG_ABSOLUTE_POINTERS : 0U, \
    }
#define SHELL_COMPLETION_GPIO_PINS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_GPIO_PINS, \
    }
#define SHELL_COMPLETION_I2C_ARGUMENTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_I2C_ARGUMENTS, \
    }
#define SHELL_COMPLETION_ONEWIRE_BUSES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_ONEWIRE_BUSES, \
    }
#define SHELL_COMPLETION_SPI_BUSES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_SPI_BUSES, \
    }
#define SHELL_COMPLETION_UART_BUSES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_UART_BUSES, \
    }
#define SHELL_COMPLETION_COM_ARGUMENTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_COM_ARGUMENTS, \
    }
#define SHELL_COMPLETION_UART_ARGUMENTS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_UART_ARGUMENTS, \
    }
#define SHELL_COMPLETION_BUSES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_BUSES, \
    }
#define SHELL_COMPLETION_PS2_BUSES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_PS2_BUSES, \
    }
#define SHELL_COMPLETION_MIDI_BUSES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_MIDI_BUSES, \
    }
#define SHELL_COMPLETION_SPI_CS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_SPI_CS, \
    }
#define SHELL_COMPLETION_STREAMS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_STREAMS, \
    }
#define SHELL_COMPLETION_SCHEDULE_ENTRIES(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_SCHEDULE_ENTRIES, \
    }
#define SHELL_COMPLETION_SCALAR_STREAMS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_STREAMS, \
        .flags = SHELL_COMPLETION_FLAG_SCALAR_STREAMS, \
    }
#define SHELL_COMPLETION_CONTROLS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_CONTROLS, \
    }
#define SHELL_COMPLETION_PARAMETERS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_PARAMETERS, \
    }
#define SHELL_COMPLETION_WIFI_SSIDS(path_array) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .source = SHELL_COMPLETION_SOURCE_WIFI_SSIDS, \
    }
#define SHELL_COMPLETION_PATH(path_array, only_dirs) \
    { \
        .path = path_array, \
        .path_count = SHELL_ARRAY_COUNT(path_array), \
        .flags = SHELL_COMPLETION_FLAG_PATH | \
            ((only_dirs) ? SHELL_COMPLETION_FLAG_DIRS_ONLY : 0U), \
    }

static const shell_completion_rule_t shell_completion_rules[] = {
    SHELL_COMPLETION_MANUAL(path_man, man_options),
    SHELL_COMPLETION_MANUAL(path_help, help_subcommands),
    SHELL_COMPLETION_OPTIONS(path_ls, ls_options),
    SHELL_COMPLETION_OPTIONS(path_rm, rm_options),
#if SOLAR_OS_PACKAGE_APP_APLAY
    SHELL_COMPLETION_OPTIONS(path_aplay, aplay_options),
    SHELL_COMPLETION_STATIC(path_aplay_volume, aplay_volume_values),
#endif
#if SOLAR_OS_PACKAGE_APP_ARECORD
    SHELL_COMPLETION_OPTIONS(path_arecord, arecord_options),
    SHELL_COMPLETION_STATIC(path_arecord_duration, arecord_duration_values),
    SHELL_COMPLETION_STREAMS(path_arecord_input),
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
    SHELL_COMPLETION_OPTIONS(path_recorder, recorder_options),
    SHELL_COMPLETION_PATH(path_recorder, false),
    SHELL_COMPLETION_PATH(path_recorder_tui, false),
#endif
#if SOLAR_OS_PACKAGE_APP_CLOCK
    SHELL_COMPLETION_STATIC(path_clock, clock_options),
    SHELL_COMPLETION_STATIC(path_clock_alarm, clock_alarm_values),
#endif
#if SOLAR_OS_PACKAGE_APP_CALC
    SHELL_COMPLETION_OPTIONS(path_calc, calc_options),
#endif
#if SOLAR_OS_PACKAGE_APP_COM
    SHELL_COMPLETION_COM_ARGUMENTS(path_com),
    SHELL_COMPLETION_COM_ARGUMENTS(path_com_arg),
    SHELL_COMPLETION_COM_ARGUMENTS(path_com_arg2),
#endif
#if SOLAR_OS_PACKAGE_APP_CURL
    SHELL_COMPLETION_OPTIONS(path_curl, curl_options),
    SHELL_COMPLETION_PATH(path_curl_output, false),
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
    SHELL_COMPLETION_STATIC(path_webradio, webradio_subcommands),
    SHELL_COMPLETION_STATIC(path_webradio_tui, webradio_tui_subcommands),
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYER
    SHELL_COMPLETION_OPTIONS(path_player, player_options),
    SHELL_COMPLETION_PATH(path_player, false),
    SHELL_COMPLETION_PATH(path_player_tui, false),
#endif
#if SOLAR_OS_PACKAGE_APP_FUNCGEN
    SHELL_COMPLETION_OPTIONS(path_funcgen, funcgen_options),
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
    SHELL_COMPLETION_GPIO_PINS(path_logic),
    SHELL_COMPLETION_STATIC(path_logic_pins, logic_rate_values),
    SHELL_COMPLETION_STATIC(path_logic_rate, logic_sample_values),
    SHELL_COMPLETION_STATIC(path_logic_samples, logic_trigger_options),
#endif
#if SOLAR_OS_PACKAGE_APP_SCP
    SHELL_COMPLETION_OPTIONS(path_scp, scp_options),
    SHELL_COMPLETION_STATIC(path_scp_port, scp_port_values),
#endif
#if SOLAR_OS_PACKAGE_APP_TELNET
    SHELL_COMPLETION_OPTIONS(path_telnet, telnet_options),
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
    SHELL_COMPLETION_STATIC(path_playground, playground_subcommands),
    SHELL_COMPLETION_PLAYGROUND_APPS(path_playground_install),
    SHELL_COMPLETION_PLAYGROUND_APPS(path_playground_run),
    SHELL_COMPLETION_PATH(path_playground_run_file, false),
    SHELL_COMPLETION_STATIC(path_playground_source, playground_source_values),
    SHELL_COMPLETION_STATIC(path_playground_storage, playground_storage_values),
    SHELL_COMPLETION_STATIC(path_playground_install_target, playground_target_values),
#endif
    SHELL_COMPLETION_OPTIONS(path_zip, zip_options),
    SHELL_COMPLETION_PATH(path_zip_after_archive, false),
    SHELL_COMPLETION_PATH(path_zip_after_option, false),
    SHELL_COMPLETION_OPTIONS(path_unzip, unzip_options),
    SHELL_COMPLETION_PATH(path_unzip_after_archive, false),
    SHELL_COMPLETION_PATH(path_unzip_after_option, false),
#if SOLAR_OS_PACKAGE_MEDIA
    SHELL_COMPLETION_OPTIONS(path_view, view_options),
    SHELL_COMPLETION_PATH(path_view_after_option, false),
#endif
#if SOLAR_OS_PACKAGE_APP_GAMEBOY
    SHELL_COMPLETION_PATH(path_gameboy, false),
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
    SHELL_COMPLETION_OPTIONS(path_files, files_options),
    SHELL_COMPLETION_PATH(path_files, false),
    SHELL_COMPLETION_PATH(path_files_launcher, false),
#endif
#if SOLAR_OS_PACKAGE_APP_NOTES
    SHELL_COMPLETION_PATH(path_notes, false),
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
    SHELL_COMPLETION_OPTIONS(path_plot, plot_options),
    SHELL_COMPLETION_SCALAR_STREAMS(path_plot),
    SHELL_COMPLETION_OPTIONS(path_plot_stream, plot_live_options),
    SHELL_COMPLETION_SCALAR_STREAMS(path_plot_stream),
    SHELL_COMPLETION_PATH(path_plot_file, false),
    SHELL_COMPLETION_PATH(path_plot_long_file, false),
#endif
#if SOLAR_OS_PACKAGE_APP_READER
    SHELL_COMPLETION_OPTIONS(path_reader, reader_options),
    SHELL_COMPLETION_PATH(path_reader, false),
    SHELL_COMPLETION_MANUAL_REFERENCES(path_reader),
    SHELL_COMPLETION_PATH(path_reader_pager, false),
    SHELL_COMPLETION_MANUAL_REFERENCES(path_reader_pager),
#endif
#if SOLAR_OS_PACKAGE_APP_WRITER
    SHELL_COMPLETION_PATH(path_writer, false),
#endif
    SHELL_COMPLETION_STATIC(path_watch, watch_subcommands),
    SHELL_COMPLETION_COMMANDS(path_watch),
    SHELL_COMPLETION_COMMANDS(path_watch_n_interval),
    SHELL_COMPLETION_STATIC(path_setterm, setterm_subcommands),
    SHELL_COMPLETION_DISPLAY_TARGETS(path_setterm_display),
    SHELL_COMPLETION_STATIC(path_setterm_display_target, setterm_display_settings),
    SHELL_COMPLETION_STATIC(path_setterm_display_orientation, setterm_orientation_values),
    SHELL_COMPLETION_STATIC(path_setterm_display_font, setterm_font_values),
    SHELL_COMPLETION_STATIC(path_setterm_display_textsize, setterm_textsize_values),
    SHELL_COMPLETION_STATIC(path_setterm_display_palette, setterm_palette_values),
    SHELL_COMPLETION_STATIC(path_setterm_display_statusbar, setterm_statusbar_values),
    SHELL_COMPLETION_STATIC(path_setterm_orientation, setterm_orientation_values),
    SHELL_COMPLETION_STATIC(path_setterm_font, setterm_font_values),
    SHELL_COMPLETION_STATIC(path_setterm_textsize, setterm_textsize_values),
    SHELL_COMPLETION_STATIC(path_setterm_palette, setterm_palette_values),
    SHELL_COMPLETION_STATIC(path_setterm_statusbar, setterm_statusbar_values),
    SHELL_COMPLETION_STATIC(path_setterm_brightness, setterm_brightness_values),
    SHELL_COMPLETION_STATIC(path_setterm_backlight, setterm_brightness_values),
    SHELL_COMPLETION_STATIC(path_setterm_profile, setterm_profile_values),
    SHELL_COMPLETION_STATIC(path_setterm_charset, setterm_charset_values),
    SHELL_COMPLETION_STATIC(path_setterm_keyboard, setterm_keyboard_values),
    SHELL_COMPLETION_STATIC(path_setterm_keymap, setterm_keyboard_values),
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    SHELL_COMPLETION_STATIC(path_setterm_ble, setterm_ble_values),
#endif
    SHELL_COMPLETION_STATIC(path_setterm_powerkey, setterm_powerkey_values),
    SHELL_COMPLETION_STATIC(path_setterm_key, setterm_powerkey_values),
    SHELL_COMPLETION_STATIC(path_setterm_keyrate, setterm_keyrate_values),
    SHELL_COMPLETION_STATIC(path_setterm_typerate, setterm_keyrate_values),
    SHELL_COMPLETION_STATIC(path_setterm_repeat, setterm_keyrate_values),
    SHELL_COMPLETION_STATIC(path_setterm_timezone, setterm_timezone_values),
    SHELL_COMPLETION_STATIC(path_setterm_startup, setterm_startup_values),
    SHELL_COMPLETION_STATIC(path_display, display_subcommands),
    SHELL_COMPLETION_STATIC(path_input, input_subcommands),
    SHELL_COMPLETION_INPUT_SOURCES(path_input_test, false),
    SHELL_COMPLETION_INPUT_SOURCES(path_input_calibrate, true),
    SHELL_COMPLETION_STATIC(path_input_keyboard, input_class_subcommands),
    SHELL_COMPLETION_STATIC(path_input_touch, input_class_subcommands),
    SHELL_COMPLETION_STATIC(path_input_mouse, input_class_subcommands),
    SHELL_COMPLETION_STATIC(path_input_joystick, input_class_subcommands),
    SHELL_COMPLETION_STATIC(path_input_dpad, input_class_subcommands),
    SHELL_COMPLETION_STATIC(path_input_buttons, input_class_subcommands),
    SHELL_COMPLETION_STATIC(path_input_calibrate_source, input_calibration_subcommands),
    SHELL_COMPLETION_DISPLAY_TARGETS(path_display_test),
    SHELL_COMPLETION_DISPLAY_TARGETS(path_display_mode),
    SHELL_COMPLETION_DISPLAY_MODES(path_display_mode_target),
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
    SHELL_COMPLETION_STATIC(path_engine, engine_subcommands),
#endif
    SHELL_COMPLETION_STATIC(path_mem, mem_subcommands),
    SHELL_COMPLETION_STATIC(path_nvs, nvs_subcommands),
    SHELL_COMPLETION_PATH(path_nvs_backup, false),
    SHELL_COMPLETION_PATH(path_nvs_restore, false),
    SHELL_COMPLETION_STATIC(path_identity, identity_subcommands),
    SHELL_COMPLETION_DISPLAY_SESSION_IDS(path_fg),
    SHELL_COMPLETION_SESSION_IDS(path_close),
    SHELL_COMPLETION_STATIC(path_job, job_subcommands),
    SHELL_COMPLETION_JOBS(path_job_status),
    SHELL_COMPLETION_JOBS(path_job_start),
    SHELL_COMPLETION_STATIC(path_job_start_batmon, batmon_interval_values),
    SHELL_COMPLETION_STATIC(path_job_start_log, job_log_values),
    SHELL_COMPLETION_PORTS(path_job_start_log),
    SHELL_COMPLETION_STATIC(path_job_start_log_port, log_level_values),
    SHELL_COMPLETION_PATH(path_job_start_log_file, false),
    SHELL_COMPLETION_PORTS(path_job_start_bridge),
    SHELL_COMPLETION_PORTS(path_job_start_bridge_port),
#if SOLAR_OS_PACKAGE_JOB_GPIO_KEYS
    SHELL_COMPLETION_STATIC(path_job_start_gpio_keys, gpio_keys_options),
    SHELL_COMPLETION_PATH(path_job_start_gpio_keys_config, false),
#endif
#if SOLAR_OS_PACKAGE_JOB_PS2_KEYBOARD
    SHELL_COMPLETION_PS2_BUSES(path_job_start_ps2_keyboard),
#endif
#if SOLAR_OS_PACKAGE_JOB_MIDI
    SHELL_COMPLETION_MIDI_BUSES(path_job_start_midi),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_LINK
    SHELL_COMPLETION_LINKS(path_job_start_bridge_port),
    SHELL_COMPLETION_STATIC(path_job_start_bridge_link,
                            link_destination_values),
#endif
    SHELL_COMPLETION_PATH(path_job_start_httpd, true),
#if SOLAR_OS_PACKAGE_JOB_DISPLAYD
    SHELL_COMPLETION_DISPLAY_TARGETS(path_job_start_displayd),
#endif
    SHELL_COMPLETION_STATIC(path_job_start_ntp_sync, ntp_sync_values),
    SHELL_COMPLETION_STATIC(path_job_start_ntp_sync_interval, ntp_sync_server_values),
#if SOLAR_OS_PACKAGE_JOB_CHATD
    SHELL_COMPLETION_STATIC(path_job_start_chatd, chatd_values),
    SHELL_COMPLETION_PATH(path_job_start_chatd_history, false),
    SHELL_COMPLETION_PATH(path_job_start_chatd_log, false),
#endif
#if SOLAR_OS_PACKAGE_JOB_TELNETD
    SHELL_COMPLETION_STATIC(path_job_start_telnetd, telnetd_values),
#endif
#if SOLAR_OS_PACKAGE_JOB_EMAIL_SYNC
    SHELL_COMPLETION_STATIC(path_job_start_email_sync, email_sync_values),
#endif
#if SOLAR_OS_PACKAGE_JOB_POCSAG
    SHELL_COMPLETION_RADIOS(path_job_start_pocsag),
    SHELL_COMPLETION_STATIC(path_pocsag, pocsag_subcommands),
    SHELL_COMPLETION_RADIOS(path_pocsag_send),
    SHELL_COMPLETION_STATIC(path_pocsag_send_message, pocsag_format_values),
    SHELL_COMPLETION_STATIC(path_pocsag_send_format, pocsag_polarity_values),
#endif
#if SOLAR_OS_PACKAGE_JOB_RADIO_LINK
    SHELL_COMPLETION_STATIC(path_job_start_radio_link, radio_link_names),
    SHELL_COMPLETION_RADIOS(path_job_start_radio_link_name),
    SHELL_COMPLETION_RADIO_PROFILES(path_job_start_radio_link_radio),
    SHELL_COMPLETION_STATIC(path_job_start_radio_link_profile,
                            radio_link_option_values),
    SHELL_COMPLETION_STATIC(path_job_start_radio_link_option,
                            radio_link_option_values),
    SHELL_COMPLETION_STATIC(path_job_start_radio_link_option_2,
                            radio_link_option_values),
#endif
#if SOLAR_OS_PACKAGE_JOB_ESPNOW_LINK
    SHELL_COMPLETION_STATIC(path_job_start_espnow_link, espnow_link_names),
    SHELL_COMPLETION_STATIC(path_job_start_espnow_link_name,
                            espnow_link_option_values),
    SHELL_COMPLETION_STATIC(path_job_start_espnow_link_option,
                            espnow_link_option_values),
    SHELL_COMPLETION_STATIC(path_job_start_espnow_link_option2,
                            espnow_link_option_values),
    SHELL_COMPLETION_STATIC(path_job_start_espnow_link_option3,
                            espnow_link_option_values),
    SHELL_COMPLETION_STATIC(path_job_start_espnow_link_option4,
                            espnow_link_option_values),
#endif
#if SOLAR_OS_PACKAGE_JOB_MESHCORE
    SHELL_COMPLETION_RADIOS(path_job_start_meshcore),
    SHELL_COMPLETION_RADIO_PROFILES(path_job_start_meshcore_radio),
    SHELL_COMPLETION_STATIC(path_meshcore, meshcore_subcommands),
    SHELL_COMPLETION_STATIC(path_meshcore_identity,
                            meshcore_identity_subcommands),
    SHELL_COMPLETION_STATIC(path_meshcore_advert, meshcore_advert_values),
    SHELL_COMPLETION_STATIC(path_meshcore_channel,
                            meshcore_channel_subcommands),
    SHELL_COMPLETION_STATIC(path_meshcore_channel_public,
                            meshcore_on_off_values),
    SHELL_COMPLETION_STATIC(path_meshcore_stream,
                            meshcore_stream_subcommands),
    SHELL_COMPLETION_MESHCORE_STREAMS(path_meshcore_stream_status),
    SHELL_COMPLETION_STATIC(path_meshcore_stream_create,
                            meshcore_stream_port_values),
    SHELL_COMPLETION_ENDPOINT_IDS(path_meshcore_stream_create_port),
    SHELL_COMPLETION_MESHCORE_STREAMS(path_meshcore_stream_remove),
#endif
    SHELL_COMPLETION_PORTS(path_job_start_slip),
#if SOLAR_OS_PACKAGE_JOB_SLIP
    SHELL_COMPLETION_STATIC(path_job_start_slip_port, slip_baud_values),
#endif
    SHELL_COMPLETION_GPIO_PINS(path_job_start_sump),
    SHELL_COMPLETION_STREAMS(path_job_start_daq),
    SHELL_COMPLETION_PATH(path_job_start_daq, false),
    SHELL_COMPLETION_STREAMS(path_job_start_daq_stream),
    SHELL_COMPLETION_PATH(path_job_start_daq_stream, false),
    SHELL_COMPLETION_STREAMS(path_job_start_daq_stream_file),
    SHELL_COMPLETION_PATH(path_job_start_daq_stream_file, false),
    SHELL_COMPLETION_STATIC(path_job_start_daq_stream_file, daq_options),
    SHELL_COMPLETION_JOBS(path_job_stop),
    SHELL_COMPLETION_STATIC(path_session, session_subcommands),
    SHELL_COMPLETION_STATIC(path_session_create, session_create_values),
    SHELL_COMPLETION_APPS(path_session_create),
    SHELL_COMPLETION_PORTS(path_session_create_shell),
    SHELL_COMPLETION_DISPLAY_TARGETS(path_session_create_shell),
    SHELL_COMPLETION_DISPLAY_TARGETS(path_session_create_app),
    SHELL_COMPLETION_OPTIONS(path_session_create_shell_target, session_shell_options),
    SHELL_COMPLETION_STATIC(path_session_create_shell_term, session_shell_term_values),
    SHELL_COMPLETION_STATIC(path_session_create_shell_charset, session_shell_charset_values),
    SHELL_COMPLETION_STATIC(path_session_create_shell_size, session_shell_size_values),
    SHELL_COMPLETION_DISPLAY_SESSION_IDS(path_session_fg),
    SHELL_COMPLETION_DISPLAY_SESSION_IDS(path_session_foreground),
    SHELL_COMPLETION_DISPLAY_SESSION_IDS(path_session_switch),
    SHELL_COMPLETION_SESSION_IDS(path_session_close),
    SHELL_COMPLETION_DISPLAY_SESSION_IDS(path_session_send),
    SHELL_COMPLETION_DISPLAY_TARGETS(path_session_focus),
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
    SHELL_COMPLETION_WIFI_SSIDS(path_wifi_connect),
    SHELL_COMPLETION_STATIC(path_wifi_nat, wifi_nat_subcommands),
    SHELL_COMPLETION_STATIC(path_wifi_repeater, wifi_repeater_subcommands),
    SHELL_COMPLETION_STATIC(path_wifi_forget, wifi_forget_values),
    SHELL_COMPLETION_WIFI_SSIDS(path_wifi_forget),
#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
    SHELL_COMPLETION_STATIC(path_wireguard, wireguard_subcommands),
    SHELL_COMPLETION_STATIC(path_wireguard_up, wireguard_policy_values),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NET
    SHELL_COMPLETION_STATIC(path_ping_count, ping_count_values),
    SHELL_COMPLETION_STATIC(path_netscan_ports, netscan_port_values),
    SHELL_COMPLETION_STATIC(path_ntp, ntp_server_values),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MQTT
    SHELL_COMPLETION_STATIC(path_mqtt, mqtt_subcommands),
    SHELL_COMPLETION_STATIC(path_mqtt_publish_payload, mqtt_qos_values),
    SHELL_COMPLETION_STATIC(path_mqtt_publish_qos, mqtt_retain_values),
    SHELL_COMPLETION_STATIC(path_mqtt_subscribe_topic, mqtt_qos_values),
#endif
    SHELL_COMPLETION_STATIC(path_disk, disk_subcommands),
    SHELL_COMPLETION_STORAGE_MOUNTABLES(path_disk_mount),
    SHELL_COMPLETION_PATH(path_disk_mount_target, true),
    SHELL_COMPLETION_STORAGE_UNMOUNT_TARGETS(path_disk_umount),
    SHELL_COMPLETION_STORAGE_BLOCKS(path_disk_format),
    SHELL_COMPLETION_STATIC(path_disk_format_force, disk_format_force_values),
    SHELL_COMPLETION_STATIC(path_ramfs, ramfs_subcommands),
    SHELL_COMPLETION_STATIC(path_ramfs_mount_path, ramfs_size_values),
    SHELL_COMPLETION_RAMFS_MOUNTS(path_ramfs_unmount),
    SHELL_COMPLETION_STATIC(path_i2c, i2c_subcommands),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_status),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_speed),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_scan),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_probe),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_probe_bus),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_read),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_read_addr),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_read_reg),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_read_bus_reg),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_write),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_write_addr),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_write_reg),
    SHELL_COMPLETION_I2C_ARGUMENTS(path_i2c_write_bus_reg),
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    SHELL_COMPLETION_STATIC(path_spi, spi_subcommands),
    SHELL_COMPLETION_SPI_BUSES(path_spi_status),
    SHELL_COMPLETION_SPI_BUSES(path_spi_xfer),
    SHELL_COMPLETION_SPI_CS(path_spi_xfer_bus),
    SHELL_COMPLETION_STATIC(path_spi_xfer_cs, spi_mode_values),
    SHELL_COMPLETION_STATIC(path_spi_xfer_mode, spi_speed_values),
    SHELL_COMPLETION_SPI_BUSES(path_spi_read),
    SHELL_COMPLETION_SPI_CS(path_spi_read_bus),
    SHELL_COMPLETION_STATIC(path_spi_read_cs, spi_mode_values),
    SHELL_COMPLETION_STATIC(path_spi_read_mode, spi_speed_values),
    SHELL_COMPLETION_STATIC(path_spi_read_len, spi_fill_values),
    SHELL_COMPLETION_SPI_BUSES(path_spi_write),
    SHELL_COMPLETION_SPI_CS(path_spi_write_bus),
    SHELL_COMPLETION_STATIC(path_spi_write_cs, spi_mode_values),
    SHELL_COMPLETION_STATIC(path_spi_write_mode, spi_speed_values),
#endif
    SHELL_COMPLETION_STATIC(path_uart, uart_subcommands),
    SHELL_COMPLETION_UART_ARGUMENTS(path_uart_baud),
    SHELL_COMPLETION_UART_ARGUMENTS(path_uart_mode),
    SHELL_COMPLETION_UART_ARGUMENTS(path_uart_read),
    SHELL_COMPLETION_UART_ARGUMENTS(path_uart_status),
    SHELL_COMPLETION_UART_ARGUMENTS(path_uart_write),
    SHELL_COMPLETION_STATIC(path_port, port_subcommands),
    SHELL_COMPLETION_PORTS(path_port_status),
    SHELL_COMPLETION_STATIC(path_xfer, xfer_subcommands),
    SHELL_COMPLETION_PORTS(path_xfer_send),
    SHELL_COMPLETION_PORTS(path_xfer_recv),
    SHELL_COMPLETION_PATH(path_xfer_send_port, false),
    SHELL_COMPLETION_PATH(path_xfer_recv_port, false),
    SHELL_COMPLETION_STATIC(path_xfer_send_port_file, xfer_options),
    SHELL_COMPLETION_STATIC(path_xfer_recv_port_file, xfer_options),
    SHELL_COMPLETION_STATIC(path_xfer_send_protocol, xfer_protocol_values),
    SHELL_COMPLETION_STATIC(path_xfer_recv_protocol, xfer_protocol_values),
    SHELL_COMPLETION_STATIC(path_log, log_subcommands),
    SHELL_COMPLETION_STATIC(path_log_follow, log_level_values),
    SHELL_COMPLETION_STATIC(path_log_level, log_level_values),
    SHELL_COMPLETION_STATIC(path_log_sink, log_sink_values),
    SHELL_COMPLETION_STATIC(path_log_sink_cdc, on_off_values),
#if SOLAR_OS_PACKAGE_APP_INBOX
    SHELL_COMPLETION_STATIC(path_inbox, inbox_subcommands),
    SHELL_COMPLETION_STATIC(path_inbox_list, inbox_list_values),
    SHELL_COMPLETION_INBOX_IDS(path_inbox_read),
    SHELL_COMPLETION_INBOX_IDS(path_inbox_delete),
    SHELL_COMPLETION_STATIC(path_inbox_notify, inbox_notify_values),
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
    SHELL_COMPLETION_STATIC(path_contacts, contacts_subcommands),
    SHELL_COMPLETION_STATIC(path_contacts_list, contacts_list_values),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_show),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_rename),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_trust),
    SHELL_COMPLETION_ENDPOINT_IDS(path_contacts_trust_endpoint),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_block),
    SHELL_COMPLETION_ENDPOINT_IDS(path_contacts_block_endpoint),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_remove),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_link),
    SHELL_COMPLETION_CONTACT_IDS(path_contacts_link_source),
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
    SHELL_COMPLETION_STATIC(path_chat, chat_selectors),
    SHELL_COMPLETION_STATIC(path_messages, messages_subcommands),
    SHELL_COMPLETION_MESSAGE_IDS(path_messages_delete),
    SHELL_COMPLETION_MESSAGE_IDS(path_messages_cancel),
    SHELL_COMPLETION_STATIC(path_messages_clear, messages_clear_values),
    SHELL_COMPLETION_STATIC(path_outbox, outbox_subcommands),
    SHELL_COMPLETION_MESSAGE_IDS(path_outbox_cancel),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GATEWAY
    SHELL_COMPLETION_STATIC(path_gateway, gateway_subcommands),
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
    SHELL_COMPLETION_STATIC(path_email, email_subcommands),
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
    SHELL_COMPLETION_STATIC(path_agent, agent_subcommands),
    SHELL_COMPLETION_AGENT_CONVERSATIONS(path_agent_resume),
    SHELL_COMPLETION_AGENT_CONVERSATIONS(path_agent_delete),
    SHELL_COMPLETION_STATIC(path_agent_config, agent_config_fields),
#if SOLAR_OS_PACKAGE_APP_PYTHON || SOLAR_OS_PACKAGE_APP_LUA
    SHELL_COMPLETION_STATIC(path_agent_script, agent_script_languages),
    SHELL_COMPLETION_OPTIONS(path_agent_script_language, agent_script_options),
    SHELL_COMPLETION_PATH(path_agent_script_language, false),
#endif
    SHELL_COMPLETION_STATIC(path_agent_config_key, agent_key_values),
    SHELL_COMPLETION_STATIC(path_agent_config_reasoning, agent_reasoning_values),
    SHELL_COMPLETION_STATIC(path_agent_config_tools, agent_tool_policy_values),
    SHELL_COMPLETION_STATIC(path_agent_config_max_tools, agent_max_tools_values),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO && SOLAR_OS_BOARD_HAS_STATUS_LED
    SHELL_COMPLETION_STATIC(path_led, led_subcommands),
#endif
    SHELL_COMPLETION_STATIC(path_gpio, gpio_subcommands),
    SHELL_COMPLETION_GPIO_PINS(path_gpio_mode),
    SHELL_COMPLETION_STATIC(path_gpio_mode_pin, gpio_mode_values),
    SHELL_COMPLETION_STATIC(path_gpio_mode_pin_mode, gpio_pull_values),
    SHELL_COMPLETION_GPIO_PINS(path_gpio_read),
    SHELL_COMPLETION_GPIO_PINS(path_gpio_write),
    SHELL_COMPLETION_STATIC(path_gpio_write_pin, bit_values),
    SHELL_COMPLETION_GPIO_PINS(path_gpio_release),
    SHELL_COMPLETION_STATIC(path_onewire, onewire_subcommands),
    SHELL_COMPLETION_ONEWIRE_BUSES(path_onewire_status),
    SHELL_COMPLETION_GPIO_PINS(path_onewire_reset),
    SHELL_COMPLETION_ONEWIRE_BUSES(path_onewire_reset),
    SHELL_COMPLETION_GPIO_PINS(path_onewire_scan),
    SHELL_COMPLETION_ONEWIRE_BUSES(path_onewire_scan),
    SHELL_COMPLETION_GPIO_PINS(path_onewire_xfer),
    SHELL_COMPLETION_ONEWIRE_BUSES(path_onewire_xfer),
    SHELL_COMPLETION_STATIC(path_onewire_xfer_pin, onewire_read_lengths),
    SHELL_COMPLETION_STATIC(path_onewire_xfer_len, byte_values),
    SHELL_COMPLETION_STATIC(path_adc, adc_subcommands),
    SHELL_COMPLETION_GPIO_PINS(path_adc_read),
    SHELL_COMPLETION_STATIC(path_dpad, dpad_subcommands),
    SHELL_COMPLETION_STATIC(path_dpad_calibrate, dpad_calibrate_subcommands),
    SHELL_COMPLETION_STATIC(path_pwm, pwm_subcommands),
    SHELL_COMPLETION_GPIO_PINS(path_pwm_set),
    SHELL_COMPLETION_STATIC(path_pwm_set_pin, pwm_freq_values),
    SHELL_COMPLETION_STATIC(path_pwm_set_freq, pwm_duty_values),
    SHELL_COMPLETION_GPIO_PINS(path_pwm_off),
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    SHELL_COMPLETION_STATIC(path_expansion, expansion_subcommands),
    SHELL_COMPLETION_CONNECTORS(path_expansion_layout),
    SHELL_COMPLETION_STATIC(path_expansion_bus, expansion_bus_subcommands),
    SHELL_COMPLETION_STATIC(path_expansion_bus_create, expansion_bus_protocols),
    SHELL_COMPLETION_BUSES(path_expansion_bus_attach),
    SHELL_COMPLETION_BUSES(path_expansion_bus_detach),
    SHELL_COMPLETION_BUSES(path_expansion_bus_remove),
    SHELL_COMPLETION_EXPANSION_DRIVERS(path_expansion_attach),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_expansion_detach),
#endif
#if SOLAR_OS_PACKAGE_JOB_MIDI
    SHELL_COMPLETION_STATIC(path_midi, midi_subcommands),
    SHELL_COMPLETION_STATIC(path_midi_stream, midi_stream_subcommands),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
    SHELL_COMPLETION_STATIC(path_control, control_subcommands),
    SHELL_COMPLETION_STATIC(path_control_create_source, control_source_values),
    SHELL_COMPLETION_SCALAR_STREAMS(path_control_create_source),
    SHELL_COMPLETION_CONTROLS(path_control_bind),
    SHELL_COMPLETION_STATIC(path_control_bind_target, control_bind_targets),
    SHELL_COMPLETION_PARAMETERS(path_control_bind_parameter),
    SHELL_COMPLETION_CONTROLS(path_control_get),
    SHELL_COMPLETION_CONTROLS(path_control_set),
    SHELL_COMPLETION_CONTROLS(path_control_unbind),
    SHELL_COMPLETION_STATIC(path_control_parameter,
                            control_parameter_subcommands),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_OSC
    SHELL_COMPLETION_STATIC(path_osc, osc_subcommands),
    SHELL_COMPLETION_STATIC(path_osc_bind_type, osc_source_types),
    SHELL_COMPLETION_STREAMS(path_osc_bind_stream),
    SHELL_COMPLETION_CONTROLS(path_osc_bind_control),
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
    SHELL_COMPLETION_STATIC(path_neopixel, neopixel_subcommands),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_neopixel_status),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_neopixel_list),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_neopixel_set),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_neopixel_fill),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_neopixel_clear),
    SHELL_COMPLETION_EXPANSION_DEVICES(path_neopixel_show),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
    SHELL_COMPLETION_STATIC(path_radio, radio_subcommands),
    SHELL_COMPLETION_RADIOS(path_radio_status),
    SHELL_COMPLETION_RADIOS(path_radio_config),
    SHELL_COMPLETION_STATIC(path_radio_config_name, radio_config_fields),
    SHELL_COMPLETION_STATIC(path_radio_config_modulation, radio_modulation_values),
    SHELL_COMPLETION_STATIC(path_radio_config_crc, on_off_values),
    SHELL_COMPLETION_STATIC(path_radio_config_variable, on_off_values),
    SHELL_COMPLETION_STATIC(path_radio_config_sf, radio_spreading_factor_values),
    SHELL_COMPLETION_STATIC(path_radio_config_coding_rate, radio_coding_rate_values),
    SHELL_COMPLETION_STATIC(path_radio_profile, radio_profile_subcommands),
    SHELL_COMPLETION_RADIO_PROFILES(path_radio_profile_show),
    SHELL_COMPLETION_RADIOS(path_radio_profile_apply),
    SHELL_COMPLETION_RADIO_PROFILES(path_radio_profile_apply_radio),
    SHELL_COMPLETION_RADIOS(path_radio_profile_save),
    SHELL_COMPLETION_USER_RADIO_PROFILES(path_radio_profile_save_radio),
    SHELL_COMPLETION_USER_RADIO_PROFILES(path_radio_profile_remove),
    SHELL_COMPLETION_RADIOS(path_radio_state),
    SHELL_COMPLETION_STATIC(path_radio_state_name, radio_state_values),
    SHELL_COMPLETION_RADIOS(path_radio_send),
    SHELL_COMPLETION_RADIOS(path_radio_recv),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ESPNOW
    SHELL_COMPLETION_STATIC(path_espnow, espnow_subcommands),
    SHELL_COMPLETION_STATIC(path_espnow_peer, espnow_peer_subcommands),
#endif
#if SOLAR_OS_PACKAGE_SERVICE_LINK
    SHELL_COMPLETION_STATIC(path_link, link_subcommands),
    SHELL_COMPLETION_LINKS(path_link_status),
    SHELL_COMPLETION_LINKS(path_link_send),
    SHELL_COMPLETION_STATIC(path_link_send_link, link_destination_values),
    SHELL_COMPLETION_LINKS(path_link_send_binary),
    SHELL_COMPLETION_STATIC(path_link_send_binary_link,
                            link_destination_values),
    SHELL_COMPLETION_LINKS(path_link_receive),
    SHELL_COMPLETION_LINKS(path_link_recv),
    SHELL_COMPLETION_STATIC(path_link_stream, link_stream_subcommands),
    SHELL_COMPLETION_LINK_STREAMS(path_link_stream_status),
    SHELL_COMPLETION_LINKS(path_link_stream_create),
    SHELL_COMPLETION_LINK_STREAMS(path_link_stream_remove),
#endif
    SHELL_COMPLETION_STATIC(path_power, power_subcommands),
    SHELL_COMPLETION_STATIC(path_power_profile, power_profile_values),
    SHELL_COMPLETION_STATIC(path_power_idle, power_idle_values),
    SHELL_COMPLETION_STATIC(path_power_key, power_key_values),
    SHELL_COMPLETION_STATIC(path_rtc, rtc_subcommands),
    SHELL_COMPLETION_STATIC(path_rtc_alarm, rtc_alarm_subcommands),
    SHELL_COMPLETION_STATIC(path_rtc_timer, rtc_timer_subcommands),
    SHELL_COMPLETION_STATIC(path_schedule, schedule_subcommands),
    SHELL_COMPLETION_STATIC(path_schedule_add_name, schedule_kinds),
    SHELL_COMPLETION_SCHEDULE_ENTRIES(path_schedule_show),
    SHELL_COMPLETION_SCHEDULE_ENTRIES(path_schedule_enable),
    SHELL_COMPLETION_SCHEDULE_ENTRIES(path_schedule_disable),
    SHELL_COMPLETION_SCHEDULE_ENTRIES(path_schedule_remove),
    SHELL_COMPLETION_SCHEDULE_ENTRIES(path_schedule_run),
    SHELL_COMPLETION_SCHEDULE_ENTRIES(path_schedule_stop),
    SHELL_COMPLETION_STATIC(path_battery, battery_subcommands),
    SHELL_COMPLETION_STATIC(path_battery_capacity, battery_capacity_values),
    SHELL_COMPLETION_STATIC(path_battery_min_voltage, battery_min_voltage_values),
    SHELL_COMPLETION_STATIC(path_battery_max_voltage, battery_max_voltage_values),
    SHELL_COMPLETION_STATIC(path_audio, audio_subcommands),
    SHELL_COMPLETION_AUDIO_OUTPUTS(path_audio_default),
    SHELL_COMPLETION_STATIC(path_audio_tone, audio_hz_values),
    SHELL_COMPLETION_STATIC(path_audio_tone_hz, audio_ms_values),
    SHELL_COMPLETION_STATIC(path_audio_tone_ms, audio_volume_values),
    SHELL_COMPLETION_STATIC(path_audio_tone_async, audio_hz_values),
    SHELL_COMPLETION_STATIC(path_audio_tone_async_hz, audio_ms_values),
    SHELL_COMPLETION_STATIC(path_audio_tone_async_ms, audio_volume_values),
    SHELL_COMPLETION_STATIC(path_audio_level, audio_volume_values),
    SHELL_COMPLETION_STATIC(path_audio_mic, audio_ms_values),
    SHELL_COMPLETION_STATIC(path_audio_loopback, audio_ms_values),
    SHELL_COMPLETION_STATIC(path_audio_loopback_ms, audio_volume_values),
#if SOLAR_OS_PACKAGE_SERVICE_SSH
    SHELL_COMPLETION_STATIC(path_sshkey, sshkey_subcommands),
    SHELL_COMPLETION_STATIC(path_sshkey_gen, sshkey_gen_values),
    SHELL_COMPLETION_STATIC(path_sshkey_gen_force, sshkey_bits_values),
#endif
    SHELL_COMPLETION_STATIC(path_ota, ota_subcommands),
    SHELL_COMPLETION_STATIC(path_ota_boot, ota_boot_values),
};

#define SHELL_COMPLETION_INDEX_BUCKETS 128U
#define SHELL_COMPLETION_RULE_NONE UINT16_MAX

typedef struct {
    uint16_t heads[SHELL_COMPLETION_INDEX_BUCKETS];
    uint16_t next[SHELL_ARRAY_COUNT(shell_completion_rules)];
    bool ready;
} shell_completion_index_t;

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared shell completion command index")
static shell_completion_index_t shell_completion_index;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shell completion index initialization lock")
static portMUX_TYPE shell_completion_index_lock = portMUX_INITIALIZER_UNLOCKED;

_Static_assert(SHELL_ARRAY_COUNT(shell_completion_rules) < UINT16_MAX,
               "shell completion rule index exceeds uint16_t");

static uint32_t shell_completion_command_hash(const char *command)
{
    uint32_t hash = 2166136261U;
    if (command == NULL) {
        return hash;
    }
    for (const unsigned char *p = (const unsigned char *)command;
         *p != '\0';
         p++) {
        hash ^= *p;
        hash *= 16777619U;
    }
    return hash;
}

static void shell_completion_index_init(void)
{
    portENTER_CRITICAL(&shell_completion_index_lock);
    if (!shell_completion_index.ready) {
        for (size_t i = 0U; i < SHELL_COMPLETION_INDEX_BUCKETS; i++) {
            shell_completion_index.heads[i] = SHELL_COMPLETION_RULE_NONE;
        }
        for (size_t remaining = SHELL_ARRAY_COUNT(shell_completion_rules);
             remaining > 0U;
             remaining--) {
            const size_t i = remaining - 1U;
            const char *command = shell_completion_rules[i].path[0];
            const size_t bucket =
                shell_completion_command_hash(command) % SHELL_COMPLETION_INDEX_BUCKETS;
            shell_completion_index.next[i] = shell_completion_index.heads[bucket];
            shell_completion_index.heads[bucket] = (uint16_t)i;
        }
        shell_completion_index.ready = true;
    }
    portEXIT_CRITICAL(&shell_completion_index_lock);
}

static uint16_t shell_completion_rule_first(const char *command)
{
    if (command == NULL || command[0] == '\0') {
        return SHELL_COMPLETION_RULE_NONE;
    }
    shell_completion_index_init();
    const size_t bucket =
        shell_completion_command_hash(command) % SHELL_COMPLETION_INDEX_BUCKETS;
    uint16_t index = shell_completion_index.heads[bucket];
    while (index != SHELL_COMPLETION_RULE_NONE &&
           strcmp(shell_completion_rules[index].path[0], command) != 0) {
        index = shell_completion_index.next[index];
    }
    return index;
}

static uint16_t shell_completion_rule_next(uint16_t index, const char *command)
{
    if (index == SHELL_COMPLETION_RULE_NONE) {
        return index;
    }
    index = shell_completion_index.next[index];
    while (index != SHELL_COMPLETION_RULE_NONE &&
           strcmp(shell_completion_rules[index].path[0], command) != 0) {
        index = shell_completion_index.next[index];
    }
    return index;
}

#undef SHELL_COMPLETION_PATH
#undef SHELL_COMPLETION_WIFI_SSIDS
#undef SHELL_COMPLETION_PORTS
#undef SHELL_COMPLETION_RADIOS
#undef SHELL_COMPLETION_RAMFS_MOUNTS
#undef SHELL_COMPLETION_DISPLAY_MODES
#undef SHELL_COMPLETION_DISPLAY_TARGETS
#undef SHELL_COMPLETION_INPUT_SOURCES
#undef SHELL_COMPLETION_GPIO_PINS
#undef SHELL_COMPLETION_STREAMS
#undef SHELL_COMPLETION_SCALAR_STREAMS
#undef SHELL_COMPLETION_SPI_CS
#undef SHELL_COMPLETION_UART_BUSES
#undef SHELL_COMPLETION_UART_ARGUMENTS
#undef SHELL_COMPLETION_STORAGE_UNMOUNT_TARGETS
#undef SHELL_COMPLETION_STORAGE_BLOCKS
#undef SHELL_COMPLETION_STORAGE_MOUNTABLES
#undef SHELL_COMPLETION_SESSION_IDS
#undef SHELL_COMPLETION_DISPLAY_SESSION_IDS
#undef SHELL_COMPLETION_JOBS
#undef SHELL_COMPLETION_EXPANSION_DEVICES
#undef SHELL_COMPLETION_INBOX_IDS
#undef SHELL_COMPLETION_MESSAGE_IDS
#undef SHELL_COMPLETION_PLAYGROUND_APPS
#undef SHELL_COMPLETION_AGENT_CONVERSATIONS
#undef SHELL_COMPLETION_COMMANDS
#undef SHELL_COMPLETION_OPTIONS
#undef SHELL_COMPLETION_STATIC
#undef SHELL_COMPLETION_MANUAL_REFERENCES

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

static void cmd_commands(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = shell_io(ctx);

    if (argc != 1) {
        solar_os_shell_diag_unexpected(io, "commands", argv[1], "commands");
        return;
    }

    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        solar_os_shell_io_write_bold(io, shell_builtin_commands[i].name);
        solar_os_shell_io_printf(io, " - %s\n", shell_builtin_commands[i].summary);
    }
}

static void cmd_echo(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = shell_io(ctx);

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            solar_os_shell_io_write(io, " ");
        }
        solar_os_shell_io_write(io, argv[i]);
    }
    solar_os_shell_io_newline(io);
}

static void cmd_wait(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = shell_io(ctx);

    if (argc != 2) {
        if (argc < 2) {
            solar_os_shell_diag_missing(io, "wait", "seconds", "wait <seconds>");
        } else {
            solar_os_shell_diag_unexpected(io, "wait", argv[2], "wait <seconds>");
        }
        return;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long seconds = strtoul(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' ||
        seconds > SHELL_WAIT_MAX_SECONDS) {
        solar_os_shell_diag_invalid(io,
                                    "wait",
                                    "seconds",
                                    argv[1],
                                    "0 through 86400",
                                    "wait <seconds>",
                                    false);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS((uint32_t)seconds * 1000U));
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
        strlcpy(session->cwd,
                solar_os_storage_path_has_mount_prefix("/") ? "/" : solar_os_storage_mount_point(),
                sizeof(session->cwd));
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

esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx,
                                      const char *arg,
                                      char *path,
                                      size_t path_len)
{
    if (ctx == NULL || path == NULL || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return resolve_path(ctx, arg, path, path_len);
}

solar_os_shell_io_t *solar_os_shell_context_io(solar_os_context_t *ctx)
{
    return shell_io(ctx);
}

bool solar_os_shell_resolve_path_for_command(solar_os_context_t *ctx,
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

esp_err_t solar_os_shell_set_cwd(solar_os_context_t *ctx, const char *path)
{
    if (ctx == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_shell_session_t *session = shell_session(ctx);
    if (strlcpy(session->cwd, path, sizeof(session->cwd)) >= sizeof(session->cwd)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
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
        if (strcmp(root, "/") == 0 || strcmp(root, solar_os_storage_mount_point()) == 0) {
            strlcpy(display, "/", display_len);
        } else {
            snprintf(display, display_len, "%s/", root);
        }
    } else {
        if (strcmp(root, "/") == 0 || strcmp(root, solar_os_storage_mount_point()) == 0) {
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
    char status_message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];

    shell_session(ctx)->input_len = 0;
    shell_session(ctx)->input_cursor = 0;
    shell_session(ctx)->input_view_offset = 0;
    shell_session(ctx)->input[0] = '\0';
    shell_session(ctx)->history_index = -1;
    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->previous_key_was_tab = false;
    shell_session(ctx)->prompt_on_resume = false;
    shell_session(ctx)->clear_on_resume = false;

    int exit_code = 0;
    if (solar_os_context_take_exit_result(ctx, &exit_code)) {
        shell_session(ctx)->last_exit_code = exit_code;
    }

    if (shell_session(ctx)->exit_message_pending) {
        if (solar_os_shell_io_cursor_col(io) != 0) {
            solar_os_shell_io_newline(io);
        }
        solar_os_shell_io_writeln(io, shell_session(ctx)->exit_message);
        shell_session(ctx)->exit_message_pending = false;
        shell_session(ctx)->exit_message[0] = '\0';
    }

    if (solar_os_context_take_status_message(ctx,
                                             status_message,
                                             sizeof(status_message)) &&
        status_message[0] != '\0') {
        if (solar_os_shell_io_cursor_col(io) != 0) {
            solar_os_shell_io_newline(io);
        }
        solar_os_shell_io_writeln(io, status_message);
    }

    if (!shell_path_has_storage_prefix(shell_session(ctx)->cwd)) {
        shell_reset_cwd(shell_session(ctx));
    }
    shell_format_display_path(shell_session(ctx)->cwd, display_path, sizeof(display_path));
    solar_os_identity_format(identity, sizeof(identity));
    snprintf(prompt, sizeof(prompt), "%s:%s ", identity, display_path);
    solar_os_shell_io_set_cursor_visible(io, true);
    solar_os_shell_io_write_bold(io, prompt);
    shell_session(ctx)->input_row = solar_os_shell_io_cursor_row(io);
    shell_session(ctx)->input_col = solar_os_shell_io_cursor_col(io);
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

static bool shell_can_redraw_input(solar_os_context_t *ctx)
{
    return solar_os_shell_io_is_cursor_addressable(shell_io(ctx));
}

static void shell_dumb_backspace(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = shell_io(ctx);

    solar_os_shell_io_put_char(io, '\b');
    solar_os_shell_io_put_char(io, ' ');
    solar_os_shell_io_put_char(io, '\b');
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

    if (!shell_can_redraw_input(ctx)) {
        return;
    }

    shell_ensure_cursor_visible(ctx);
    size_t cursor_col = shell_session(ctx)->input_cursor - shell_session(ctx)->input_view_offset;
    if (cursor_col >= visible_cols) {
        cursor_col = visible_cols - 1;
    }
    const size_t remaining = shell_session(ctx)->input_len - shell_session(ctx)->input_view_offset;
    const size_t visible_len = remaining < visible_cols ? remaining : visible_cols;
    (void)solar_os_shell_io_redraw_line(io,
                                        shell_session(ctx)->input_row,
                                        shell_session(ctx)->input_col,
                                        shell_session(ctx)->input + shell_session(ctx)->input_view_offset,
                                        visible_len,
                                        cursor_col);
}

static void shell_replace_input(solar_os_context_t *ctx, const char *text)
{
    const size_t max_len = shell_max_input_len(ctx);
    size_t copy_len = 0;

    if (!shell_can_redraw_input(ctx)) {
        while (shell_session(ctx)->input_len > 0) {
            shell_dumb_backspace(ctx);
            shell_session(ctx)->input_len--;
        }
        shell_session(ctx)->input_cursor = 0;
        shell_session(ctx)->input_view_offset = 0;
        shell_session(ctx)->input[0] = '\0';
    }

    if (text != NULL) {
        copy_len = strnlen(text, max_len);
        memcpy(shell_session(ctx)->input, text, copy_len);
    }

    shell_session(ctx)->input[copy_len] = '\0';
    shell_session(ctx)->input_len = copy_len;
    shell_session(ctx)->input_cursor = copy_len;
    shell_session(ctx)->input_view_offset = 0;
    if (!shell_can_redraw_input(ctx)) {
        solar_os_shell_io_write(shell_io(ctx), shell_session(ctx)->input);
        return;
    }
    shell_render_input(ctx);
}

static void shell_move_cursor_left(solar_os_context_t *ctx)
{
    if (!shell_can_redraw_input(ctx)) {
        return;
    }
    if (shell_session(ctx)->input_cursor == 0) {
        return;
    }

    shell_session(ctx)->input_cursor--;
    shell_render_input(ctx);
}

static void shell_move_cursor_right(solar_os_context_t *ctx)
{
    if (!shell_can_redraw_input(ctx)) {
        return;
    }
    if (shell_session(ctx)->input_cursor >= shell_session(ctx)->input_len) {
        return;
    }

    shell_session(ctx)->input_cursor++;
    shell_render_input(ctx);
}

static void shell_move_cursor_home(solar_os_context_t *ctx)
{
    if (!shell_can_redraw_input(ctx)) {
        return;
    }
    if (shell_session(ctx)->input_cursor == 0) {
        return;
    }

    shell_session(ctx)->input_cursor = 0;
    shell_render_input(ctx);
}

static void shell_move_cursor_end(solar_os_context_t *ctx)
{
    if (!shell_can_redraw_input(ctx)) {
        return;
    }
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
    if (!shell_can_redraw_input(ctx)) {
        return;
    }
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
    if (!shell_can_redraw_input(ctx)) {
        return;
    }
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
    if (!shell_can_redraw_input(ctx)) {
        if (shell_session(ctx)->input_cursor != shell_session(ctx)->input_len) {
            return;
        }
        shell_session(ctx)->input[shell_session(ctx)->input_cursor++] = ch;
        shell_session(ctx)->input_len++;
        shell_session(ctx)->input[shell_session(ctx)->input_len] = '\0';
        solar_os_shell_io_put_char(shell_io(ctx), ch);
        return;
    }

    const bool append = shell_session(ctx)->input_cursor == shell_session(ctx)->input_len;
    const size_t previous_view_offset = shell_session(ctx)->input_view_offset;
    memmove(&shell_session(ctx)->input[shell_session(ctx)->input_cursor + 1],
            &shell_session(ctx)->input[shell_session(ctx)->input_cursor],
            shell_session(ctx)->input_len - shell_session(ctx)->input_cursor + 1);
    shell_session(ctx)->input[shell_session(ctx)->input_cursor++] = ch;
    shell_session(ctx)->input_len++;
    if (append) {
        shell_ensure_cursor_visible(ctx);
        if (shell_session(ctx)->input_view_offset == previous_view_offset) {
            (void)solar_os_shell_io_put_char(shell_io(ctx), ch);
            return;
        }
    }
    shell_render_input(ctx);
}

static void shell_paste_clipboard(solar_os_context_t *ctx)
{
    size_t clipboard_len = 0;
    const char *clipboard = solar_os_clipboard_data(&clipboard_len);
    if (clipboard == NULL || clipboard_len == 0) {
        return;
    }

    const bool can_redraw = shell_can_redraw_input(ctx);
    if (!can_redraw && shell_session(ctx)->input_cursor != shell_session(ctx)->input_len) {
        return;
    }

    const size_t paste_start = shell_session(ctx)->input_cursor;
    const size_t inserted = solar_os_shell_line_paste(
        shell_session(ctx)->input,
        sizeof(shell_session(ctx)->input),
        &shell_session(ctx)->input_len,
        &shell_session(ctx)->input_cursor,
        clipboard,
        clipboard_len);
    if (inserted == 0) {
        return;
    }

    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;
    if (!can_redraw) {
        (void)solar_os_shell_io_write_len(shell_io(ctx),
                                          &shell_session(ctx)->input[paste_start],
                                          inserted);
        return;
    }
    shell_render_input(ctx);
}

static void shell_backspace(solar_os_context_t *ctx)
{
    if (shell_session(ctx)->input_cursor == 0) {
        return;
    }
    if (!shell_can_redraw_input(ctx)) {
        if (shell_session(ctx)->input_cursor != shell_session(ctx)->input_len) {
            return;
        }
        shell_session(ctx)->input_cursor--;
        shell_session(ctx)->input_len--;
        shell_session(ctx)->input[shell_session(ctx)->input_len] = '\0';
        shell_dumb_backspace(ctx);
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
    if (path == NULL || path_len == 0) {
        return false;
    }

    if (leaf == NULL || leaf[0] == '\0') {
        return solar_os_storage_default_path(SHELL_STATE_DIR, path, path_len) == ESP_OK;
    }

    char dir[SHELL_PATH_MAX];
    if (solar_os_storage_default_path(SHELL_STATE_DIR, dir, sizeof(dir)) != ESP_OK) {
        return false;
    }
    return solar_os_storage_join_path(dir, leaf, path, path_len) == ESP_OK;
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

static void shell_update_common_prefix(char *common, size_t common_len, const char *name);

static void shell_note_completion_match(char *match,
                                        size_t match_len,
                                        size_t *match_count,
                                        const char *name)
{
    if (match == NULL || match_len == 0 || match_count == NULL || name == NULL) {
        return;
    }
    if (*match_count == 0) {
        strlcpy(match, name, match_len);
    } else {
        shell_update_common_prefix(match, match_len, name);
    }
    (*match_count)++;
}

typedef bool (*shell_alias_callback_t)(const char *name, int argc, char **argv, void *user);

static bool shell_alias_path(char *path, size_t path_len, const char *file)
{
    return file != NULL && solar_os_storage_is_mounted() &&
        shell_make_state_path(path, path_len, file);
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

    if (!shell_ensure_state_dir() ||
        !shell_alias_path(path, sizeof(path), SHELL_ALIAS_FILE)) {
        return;
    }

    FILE *file = fopen(path, "a");
    if (file != NULL) {
        fclose(file);
    }
}

static bool shell_for_each_alias_file(const char *path,
                                      shell_alias_callback_t callback,
                                      void *user,
                                      bool *stopped)
{
    char line[SHELL_INPUT_MAX + 1];

    if (path == NULL || callback == NULL || stopped == NULL) {
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
        const solar_os_shell_parse_result_t parsed =
            solar_os_shell_tokenize(alias_line, alias_argv, SHELL_ARG_MAX);
        if (parsed.error != SOLAR_OS_SHELL_PARSE_OK || parsed.argc < 2) {
            continue;
        }

        if (!callback(alias_argv[0], parsed.argc, alias_argv, user)) {
            *stopped = true;
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return true;
}

static bool shell_for_each_alias(shell_alias_callback_t callback, void *user)
{
    char path[SHELL_PATH_MAX];
    bool opened = false;
    bool stopped = false;

    if (callback == NULL ||
        !shell_alias_path(path, sizeof(path), SHELL_ALIAS_FILE)) {
        return false;
    }
    opened = shell_for_each_alias_file(path, callback, user, &stopped);
    if (stopped) {
        return opened;
    }
    if (!shell_alias_path(path,
                          sizeof(path),
                          SHELL_PLAYGROUND_ALIAS_FILE)) {
        return opened;
    }
    return shell_for_each_alias_file(path, callback, user, &stopped) || opened;
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

static bool SHELL_NOINLINE shell_try_alias(solar_os_context_t *ctx,
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

typedef struct {
    const char *input;
    int limit;
    int best_distance;
    bool ambiguous;
    char best[SHELL_INPUT_MAX];
} shell_alias_suggest_t;

static bool shell_alias_suggest_callback(const char *name, int argc, char **argv, void *user)
{
    shell_alias_suggest_t *state = (shell_alias_suggest_t *)user;
    (void)argc;
    (void)argv;

    const int distance = solar_os_shell_edit_distance(state->input, name, state->limit);
    if (distance < state->best_distance) {
        strlcpy(state->best, name, sizeof(state->best));
        state->best_distance = distance;
        state->ambiguous = false;
    } else if (distance == state->best_distance && distance <= state->limit &&
               state->best[0] != '\0' && strcmp(state->best, name) != 0) {
        state->ambiguous = true;
    }
    return true;
}

static const char *shell_alias_suggestion(const char *input, char *result, size_t result_len)
{
    shell_alias_suggest_t state = {
        .input = input,
        .limit = strlen(input) >= 5 ? 2 : 1,
        .best_distance = 3,
    };
    (void)shell_for_each_alias(shell_alias_suggest_callback, &state);
    if (state.best[0] == '\0' || state.best_distance > state.limit || state.ambiguous) {
        return NULL;
    }
    strlcpy(result, state.best, result_len);
    return result;
}

static bool shell_alias_complete_callback(const char *name, int argc, char **argv, void *user)
{
    shell_alias_complete_t *complete = (shell_alias_complete_t *)user;

    (void)argc;
    (void)argv;

    if (starts_with(name, complete->prefix)) {
        shell_note_completion_match(complete->match,
                                    sizeof(complete->match),
                                    &complete->count,
                                    name);
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
                             "%s or q exits\n",
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
                             "%s, Ctrl+C, or q exits\n",
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
        solar_os_shell_diag_missing(term, "watch", "command",
                                    "watch [-n seconds] <command> [args...]");
        return;
    }

    if (strcmp(argv[first_command_arg], "-n") == 0) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "watch -n", "seconds",
                                        "watch -n <seconds> <command> [args...]");
            return;
        }
        if (!shell_watch_parse_interval(argv[first_command_arg + 1], &interval_ms)) {
            solar_os_shell_diag_invalid(term, "watch", "interval", argv[first_command_arg + 1],
                                        "integer seconds from 1 to 86400",
                                        "watch [-n seconds] <command> [args...]", false);
            return;
        }
        if (argc < 4) {
            solar_os_shell_diag_missing(term, "watch", "command",
                                        "watch -n <seconds> <command> [args...]");
            return;
        }
        first_command_arg += 2;
    } else if (starts_with(argv[first_command_arg], "-n") && argv[first_command_arg][2] != '\0') {
        if (!shell_watch_parse_interval(&argv[first_command_arg][2], &interval_ms)) {
            solar_os_shell_diag_invalid(term, "watch", "interval", &argv[first_command_arg][2],
                                        "integer seconds from 1 to 86400",
                                        "watch [-n seconds] <command> [args...]", false);
            return;
        }
        first_command_arg++;
    } else if (argv[first_command_arg][0] == '-' && argv[first_command_arg][1] != '\0') {
        solar_os_shell_diag_invalid(term, "watch", "option", argv[first_command_arg], "-n",
                                    "watch [-n seconds] <command> [args...]", false);
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
           strcmp(command, "zip") == 0 ||
           strcmp(command, "unzip") == 0 ||
#if SOLAR_OS_PACKAGE_APP_APLAY
           strcmp(command, "aplay") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_ARECORD
           strcmp(command, "arecord") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
           strcmp(command, "recorder") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_EDIT
           strcmp(command, "edit") == 0 ||
           strcmp(command, "hexedit") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
           strcmp(command, "files") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_LESS
           strcmp(command, "less") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_READER
           strcmp(command, "reader") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_WRITER
           strcmp(command, "writer") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_SHEET
           strcmp(command, "sheet") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_PYTHON
           strcmp(command, "python") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
           strcmp(command, "lua") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_VIEW
           strcmp(command, "view") == 0 ||
#endif
#if SOLAR_OS_PACKAGE_APP_SCP
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
            !shell_builtin_command_exists(app->name) &&
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
    char match[SHELL_INPUT_MAX] = "";
    size_t match_count = 0;

    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        if (starts_with(shell_builtin_commands[i].name, shell_session(ctx)->input)) {
            shell_note_completion_match(match,
                                        sizeof(match),
                                        &match_count,
                                        shell_builtin_commands[i].name);
        }
    }
    for (size_t i = 0; i < solar_os_app_registry_count(); i++) {
        const solar_os_app_registry_entry_t *app = solar_os_app_registry_get(i);
        if (app != NULL && app->name != NULL &&
            !shell_builtin_command_exists(app->name) &&
            starts_with(app->name, shell_session(ctx)->input)) {
            shell_note_completion_match(match, sizeof(match), &match_count, app->name);
        }
    }
    shell_alias_complete_t alias_complete = {
        .prefix = shell_session(ctx)->input,
    };
    (void)shell_for_each_alias(shell_alias_complete_callback, &alias_complete);
    if (alias_complete.count > 0) {
        if (match_count == 0) {
            strlcpy(match, alias_complete.match, sizeof(match));
        } else {
            shell_update_common_prefix(match, sizeof(match), alias_complete.match);
        }
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

    if (strlen(match) > shell_session(ctx)->input_len) {
        shell_replace_input(ctx, match);
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

static shell_completion_parse_t *shell_alloc_completion_parse(void)
{
    return solar_os_memory_calloc(1,
                                  sizeof(shell_completion_parse_t),
                                  SOLAR_OS_MEMORY_TRANSIENT,
                                  "shell.complete");
}

static bool shell_completion_expand_alias(const shell_completion_parse_t *parse,
                                          size_t completed_count,
                                          char *expanded_line,
                                          size_t expanded_line_len,
                                          char **expanded_tokens,
                                          size_t *expanded_count)
{
    if (parse == NULL || parse->count == 0 || completed_count > parse->count ||
        expanded_line == NULL || expanded_line_len == 0 ||
        expanded_tokens == NULL || expanded_count == NULL) {
        return false;
    }

    char *user_argv[SHELL_ARG_MAX] = {0};
    for (size_t i = 0; i < completed_count; i++) {
        user_argv[i] = (char *)parse->tokens[i];
    }
    shell_alias_expand_t expand = {
        .name = parse->tokens[0],
        .user_argc = (int)completed_count,
        .user_argv = user_argv,
    };
    (void)shell_for_each_alias(shell_alias_expand_callback, &expand);
    if (!expand.found || expand.too_long) {
        return false;
    }

    strlcpy(expanded_line, expand.expanded, expanded_line_len);
    const solar_os_shell_parse_result_t result =
        solar_os_shell_tokenize(expanded_line, expanded_tokens, SHELL_ARG_MAX);
    if (result.error != SOLAR_OS_SHELL_PARSE_OK || result.argc <= 0) {
        return false;
    }

    *expanded_count = (size_t)result.argc;
    return true;
}

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

    if (parse == NULL) {
        return false;
    }

    memset(parse, 0, sizeof(*parse));
    return solar_os_shell_completion_parse(session->input,
                                           session->input_len,
                                           &parse->tokens[0][0],
                                           sizeof(parse->tokens[0]),
                                           SHELL_ARG_MAX,
                                           parse->starts,
                                           &parse->count,
                                           &parse->trailing_space);
}

typedef struct {
    char original[SHELL_INPUT_MAX];
    char token[SHELL_PATH_MAX];
    char dir_arg[SHELL_PATH_MAX];
    char dir_path[SHELL_PATH_MAX];
    char base_arg[SHELL_PATH_MAX];
    char match_name[SHELL_PATH_MAX];
    char common_prefix[SHELL_PATH_MAX];
    char completed_arg[SHELL_PATH_MAX];
    char encoded_arg[SHELL_INPUT_MAX];
    char completed_line[SHELL_INPUT_MAX];
} shell_path_completion_work_t;

static shell_path_completion_work_t *shell_alloc_path_completion_work(void)
{
    return solar_os_memory_calloc(1,
                                  sizeof(shell_path_completion_work_t),
                                  SOLAR_OS_MEMORY_TRANSIENT,
                                  "shell.path");
}

static void shell_update_common_prefix(char *common, size_t common_len, const char *name)
{
    if (common == NULL || common_len == 0 || name == NULL) {
        return;
    }
    const size_t length = solar_os_shell_completion_common_prefix(common, name);
    common[length < common_len ? length : common_len - 1U] = '\0';
}

static bool shell_path_entry_matches(const char *name, const char *prefix, bool prefix_has_wildcards)
{
    if (name == NULL || prefix == NULL) {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    if (prefix_has_wildcards) {
        return shell_wildcard_match(prefix, name);
    }
    return starts_with(name, prefix);
}

static void shell_print_path_matches(solar_os_context_t *ctx,
                                     const shell_path_completion_work_t *work,
                                     const char *prefix,
                                     bool prefix_has_wildcards,
                                     bool dirs_only)
{
    solar_os_shell_io_t *io = shell_io(ctx);
    DIR *dir = opendir(work->dir_path);
    if (dir == NULL) {
        return;
    }

    solar_os_shell_io_newline(io);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!shell_path_entry_matches(entry->d_name, prefix, prefix_has_wildcards)) {
            continue;
        }

        const bool entry_is_dir = shell_path_entry_is_dir(work->dir_path, entry->d_name);
        if (dirs_only && !entry_is_dir) {
            continue;
        }

        shell_print_path_match(io, work->dir_path, entry->d_name);
    }
    closedir(dir);

    shell_prompt(ctx);
    shell_replace_input(ctx, work->original);
}

static void shell_complete_path(solar_os_context_t *ctx,
                                size_t token_start,
                                const char *token,
                                bool dirs_only,
                                bool show_matches)
{
    if (token == NULL) {
        return;
    }

    shell_path_completion_work_t *work = shell_alloc_path_completion_work();
    bool match_is_dir = false;
    size_t match_count = 0;

    if (work == NULL) {
        return;
    }

    strlcpy(work->original, shell_session(ctx)->input, sizeof(work->original));
    if (strlcpy(work->token, token, sizeof(work->token)) >= sizeof(work->token)) {
        solar_os_memory_free(work);
        return;
    }

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
        solar_os_memory_free(work);
        return;
    }

    DIR *dir = opendir(work->dir_path);
    if (dir == NULL) {
        solar_os_memory_free(work);
        return;
    }

    const bool prefix_has_wildcards = shell_arg_has_wildcards(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!shell_path_entry_matches(entry->d_name, prefix, prefix_has_wildcards)) {
            continue;
        }

        const bool entry_is_dir = shell_path_entry_is_dir(work->dir_path, entry->d_name);
        if (dirs_only && !entry_is_dir) {
            continue;
        }

        match_count++;
        if (match_count == 1) {
            strlcpy(work->match_name, entry->d_name, sizeof(work->match_name));
            strlcpy(work->common_prefix, entry->d_name, sizeof(work->common_prefix));
            match_is_dir = entry_is_dir;
        } else {
            shell_update_common_prefix(work->common_prefix,
                                       sizeof(work->common_prefix),
                                       entry->d_name);
        }
    }

    closedir(dir);

    if (match_count == 0) {
        solar_os_memory_free(work);
        return;
    }

    const size_t prefix_len = strlen(prefix);
    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;

    if (match_count == 1) {
        const int completed_arg_len = snprintf(work->completed_arg,
                                               sizeof(work->completed_arg),
                                               "%s%s%s",
                                               work->base_arg,
                                               work->match_name,
                                               match_is_dir ? "/" : "");
        if (completed_arg_len < 0 ||
            (size_t)completed_arg_len >= sizeof(work->completed_arg)) {
            solar_os_memory_free(work);
            return;
        }
        if (!solar_os_shell_completion_encode_token(work->completed_arg,
                                                     work->encoded_arg,
                                                     sizeof(work->encoded_arg))) {
            solar_os_memory_free(work);
            return;
        }
        const int completed_line_len = snprintf(work->completed_line,
                                                sizeof(work->completed_line),
                                                "%.*s%s%s",
                                                (int)token_start,
                                                shell_session(ctx)->input,
                                                work->encoded_arg,
                                                match_is_dir ? "" : " ");
        if (completed_line_len < 0 ||
            (size_t)completed_line_len >= sizeof(work->completed_line)) {
            solar_os_memory_free(work);
            return;
        }
        shell_replace_input(ctx, work->completed_line);
        solar_os_memory_free(work);
        return;
    }

    if (!prefix_has_wildcards && strlen(work->common_prefix) > prefix_len) {
        const int completed_arg_len = snprintf(work->completed_arg,
                                               sizeof(work->completed_arg),
                                               "%s%s",
                                               work->base_arg,
                                               work->common_prefix);
        if (completed_arg_len < 0 ||
            (size_t)completed_arg_len >= sizeof(work->completed_arg)) {
            solar_os_memory_free(work);
            return;
        }
        if (!solar_os_shell_completion_encode_token(work->completed_arg,
                                                     work->encoded_arg,
                                                     sizeof(work->encoded_arg))) {
            solar_os_memory_free(work);
            return;
        }
        const int completed_line_len = snprintf(work->completed_line,
                                                sizeof(work->completed_line),
                                                "%.*s%s",
                                                (int)token_start,
                                                shell_session(ctx)->input,
                                                work->encoded_arg);
        if (completed_line_len < 0 ||
            (size_t)completed_line_len >= sizeof(work->completed_line)) {
            solar_os_memory_free(work);
            return;
        }
        shell_replace_input(ctx, work->completed_line);
        solar_os_memory_free(work);
        return;
    }

    if (show_matches || prefix_has_wildcards) {
        shell_print_path_matches(ctx, work, prefix, prefix_has_wildcards, dirs_only);
    }
    solar_os_memory_free(work);
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

    if (state->count == 0U) {
        strlcpy(state->match, value, sizeof(state->match));
    } else {
        const size_t common =
            solar_os_shell_completion_common_prefix(state->match, value);
        state->match[common] = '\0';
    }
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
        if (app != NULL && app->name != NULL &&
            !shell_builtin_command_exists(app->name)) {
            shell_completion_emit(state, app->name);
        }
    }
    (void)shell_for_each_alias(shell_completion_alias_emit_callback, state);
}

static void shell_completion_emit_apps(shell_completion_match_t *state)
{
    for (size_t i = 0; i < solar_os_app_registry_count(); i++) {
        const solar_os_app_registry_entry_t *app = solar_os_app_registry_get(i);
        if (app != NULL &&
            app->name != NULL &&
            (app->capabilities & SOLAR_OS_APP_CAP_DISPLAY) != 0) {
            shell_completion_emit(state, app->name);
        }
    }
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

static void shell_completion_emit_schedule_entries(
    shell_completion_match_t *state)
{
    const size_t count = solar_os_schedule_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_schedule_entry_t entry;
        if (solar_os_schedule_get(i, &entry)) {
            shell_completion_emit(state, entry.name);
        }
    }
}

static void shell_completion_emit_agent_conversations(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_APP_AGENT
    solar_os_agent_conversation_info_t *items =
        solar_os_memory_calloc(SOLAR_OS_AGENT_CONVERSATION_LIST_MAX,
                               sizeof(*items),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "shell.complete.agent");
    if (items == NULL) {
        return;
    }

    size_t count = 0;
    if (solar_os_agent_conversations_list(items,
                                          SOLAR_OS_AGENT_CONVERSATION_LIST_MAX,
                                          &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            shell_completion_emit(state, items[i].id);
        }
    }
    solar_os_memory_free(items);
#else
    (void)state;
#endif
}

static void shell_completion_emit_inbox_ids(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_APP_INBOX
    enum { COMPLETION_INBOX_MAX = 16 };
    solar_os_inbox_entry_t *entries =
        solar_os_memory_calloc(COMPLETION_INBOX_MAX,
                               sizeof(*entries),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "shell.complete.inbox");
    if (entries == NULL) {
        return;
    }

    size_t total = 0;
    const size_t count =
        solar_os_inbox_snapshot(entries, COMPLETION_INBOX_MAX, false, &total);
    for (size_t i = 0; i < count; i++) {
        char id[16];
        snprintf(id, sizeof(id), "%lu", (unsigned long)entries[i].id);
        shell_completion_emit(state, id);
    }
    solar_os_memory_free(entries);
#else
    (void)state;
#endif
}

#if SOLAR_OS_PACKAGE_APP_CHAT
static bool shell_completion_emit_message_id(
    const solar_os_messaging_message_t *message,
    void *user)
{
    shell_completion_match_t *state = user;
    char id[17];
    snprintf(id, sizeof(id), "%016" PRIx64, message->key);
    shell_completion_emit(state, id);
    return true;
}
#endif

static void shell_completion_emit_message_ids(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_APP_CHAT
    (void)solar_os_messaging_message_visit(0,
                                           0,
                                           shell_completion_emit_message_id,
                                           state,
                                           NULL);
#else
    (void)state;
#endif
}

static void shell_completion_emit_contact_ids(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_APP_CONTACTS
    solar_os_contact_t *contacts =
        solar_os_memory_calloc(SOLAR_OS_CONTACT_CAPACITY,
                               sizeof(*contacts),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "shell.complete.contacts");
    if (contacts == NULL) {
        return;
    }
    size_t total = 0U;
    const size_t count =
        solar_os_contacts_snapshot(contacts,
                                   SOLAR_OS_CONTACT_CAPACITY,
                                   false,
                                   SOLAR_OS_CONTACT_TRUST_DISCOVERED,
                                   &total);
    for (size_t index = 0U; index < count; index++) {
        char id[16];
        snprintf(id, sizeof(id), "%lu", (unsigned long)contacts[index].id);
        shell_completion_emit(state, id);
    }
    solar_os_memory_free(contacts);
#else
    (void)state;
#endif
}

static void shell_completion_emit_endpoint_ids(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_APP_CONTACTS
    solar_os_endpoint_t *endpoints =
        solar_os_memory_calloc(SOLAR_OS_ENDPOINT_CAPACITY,
                               sizeof(*endpoints),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "shell.complete.endpoints");
    if (endpoints == NULL) {
        return;
    }
    const size_t count =
        solar_os_contacts_endpoint_snapshot(SOLAR_OS_CONTACT_ID_NONE,
                                            endpoints,
                                            SOLAR_OS_ENDPOINT_CAPACITY);
    for (size_t index = 0U; index < count; index++) {
        char id[16];
        snprintf(id, sizeof(id), "%lu", (unsigned long)endpoints[index].id);
        shell_completion_emit(state, id);
    }
    solar_os_memory_free(endpoints);
#else
    (void)state;
#endif
}

static void shell_completion_emit_playground_apps(
    shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
    const size_t count = solar_os_playground_app_count();
    for (size_t i = 0U; i < count; i++) {
        char id[SOLAR_OS_PLAYGROUND_ID_MAX];
        if (solar_os_playground_get_installed_app_id(i, id, sizeof(id))) {
            shell_completion_emit(state, id);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_expansion_devices(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    const size_t count = solar_os_expansion_device_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_expansion_device_t device;
        if (solar_os_expansion_get_device(i, &device)) {
            shell_completion_emit(state, device.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_connectors(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    for (size_t i = 0; i < solar_os_connector_pin_count(); i++) {
        solar_os_connector_pin_info_t pin;
        if (!solar_os_connector_pin_get_info(i, &pin) || pin.connector == NULL) {
            continue;
        }
        bool seen = false;
        for (size_t previous = 0; previous < i; previous++) {
            solar_os_connector_pin_info_t candidate;
            if (solar_os_connector_pin_get_info(previous, &candidate) &&
                candidate.connector != NULL &&
                strcmp(candidate.connector, pin.connector) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            shell_completion_emit(state, pin.connector);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_manual_aliases(shell_completion_match_t *state,
                                                 const char *aliases,
                                                 const char *prefix)
{
    if (aliases == NULL) {
        return;
    }

    const size_t prefix_len = prefix != NULL ? strlen(prefix) : 0U;
    const char *alias = aliases;
    while (*alias != '\0') {
        const char *end = strchr(alias, '\n');
        const size_t alias_len = end != NULL ? (size_t)(end - alias) : strlen(alias);
        if (alias_len > 0U && prefix_len + alias_len < SHELL_INPUT_MAX) {
            char value[SHELL_INPUT_MAX];
            if (prefix_len > 0U) {
                memcpy(value, prefix, prefix_len);
            }
            memcpy(value + prefix_len, alias, alias_len);
            value[prefix_len + alias_len] = '\0';
            shell_completion_emit(state, value);
        }
        if (end == NULL) {
            break;
        }
        alias = end + 1;
    }
}

static void shell_completion_emit_manual_pages(shell_completion_match_t *state)
{
    for (size_t i = 0U; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page != NULL) {
            shell_completion_emit(state, page->id);
            shell_completion_emit_manual_aliases(state, page->aliases, NULL);
        }
    }
}

static void shell_completion_emit_manual_references(shell_completion_match_t *state)
{
    for (size_t i = 0U; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page != NULL) {
            char reference[SHELL_INPUT_MAX];
            snprintf(reference, sizeof(reference), "man:%s", page->id);
            shell_completion_emit(state, reference);
            shell_completion_emit_manual_aliases(state, page->aliases, "man:");
        }
    }
}

static void shell_completion_emit_session_id(shell_completion_match_t *state, uint8_t session_id)
{
    char value[8];

    snprintf(value, sizeof(value), "%u", (unsigned)session_id);
    shell_completion_emit(state, value);
}

static void shell_completion_emit_display_session_ids(shell_completion_match_t *state)
{
    const size_t count = solar_os_sessions_active_count();

    for (size_t i = 0; i < count; i++) {
        uint8_t session_id = 0;
        if (solar_os_sessions_get_active_id(i, &session_id)) {
            shell_completion_emit_session_id(state, session_id);
        }
    }
}

static void shell_completion_emit_session_ids(shell_completion_match_t *state)
{
    shell_completion_emit_display_session_ids(state);

    const size_t shell_count = solar_os_port_shell_session_count();
    for (size_t i = 0; i < shell_count; i++) {
        uint8_t session_id = 0;
        if (solar_os_port_shell_get_session_id(i, &session_id)) {
            shell_completion_emit_session_id(state, session_id);
        }
    }
    const size_t app_count = solar_os_port_shell_app_session_count();
    for (size_t i = 0; i < app_count; i++) {
        uint8_t session_id = 0;
        if (solar_os_port_shell_get_app_session_id(i, &session_id)) {
            shell_completion_emit_session_id(state, session_id);
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

static void shell_completion_emit_radios(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
    const size_t count = solar_os_radio_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_radio_info_t info;
        if (solar_os_radio_get(i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_links(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_LINK
    const size_t count = solar_os_link_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_link_status_t status;
        if (solar_os_link_get(i, &status)) {
            shell_completion_emit(state, status.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_link_streams(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_LINK
    const size_t count = solar_os_link_stream_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_link_stream_status_t status;
        if (solar_os_link_stream_get(i, &status)) {
            shell_completion_emit(state, status.port);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_meshcore_streams(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_JOB_MESHCORE
    const size_t count = solar_os_meshcore_stream_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_meshcore_stream_status_t status;
        if (solar_os_meshcore_stream_get(i, &status)) {
            shell_completion_emit(state, status.stream.port);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_radio_profiles(shell_completion_match_t *state,
                                                 bool user_only)
{
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
    solar_os_radio_profile_t profiles[SOLAR_OS_RADIO_PROFILE_MAX];
    size_t count = 0;
    if (solar_os_radio_profile_list(profiles,
                                    SOLAR_OS_RADIO_PROFILE_MAX,
                                    &count) != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (!user_only || !profiles[i].builtin) {
            shell_completion_emit(state, profiles[i].name);
        }
    }
#else
    (void)state;
    (void)user_only;
#endif
}

static void shell_completion_emit_ramfs_mounts(shell_completion_match_t *state)
{
    const size_t count = solar_os_ramfs_mount_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_ramfs_info_t info;
        if (solar_os_ramfs_get_info(i, &info)) {
            shell_completion_emit(state, info.mount_point);
        }
    }
}

static void shell_completion_emit_storage_mountables(shell_completion_match_t *state)
{
    const size_t count = solar_os_storage_block_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (solar_os_storage_get_block(i, &block) && block.mountable && !block.mounted) {
            shell_completion_emit(state, block.name);
        }
    }
}

static void shell_completion_emit_storage_blocks(shell_completion_match_t *state)
{
    const size_t count = solar_os_storage_block_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (solar_os_storage_get_block(i, &block)) {
            shell_completion_emit(state, block.name);
        }
    }
}

static void shell_completion_emit_storage_unmount_targets(shell_completion_match_t *state)
{
    const size_t count = solar_os_storage_block_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (!solar_os_storage_get_block(i, &block) || !block.mountable || !block.mounted) {
            continue;
        }
        shell_completion_emit(state, block.name);
        if (block.mount_point[0] != '\0') {
            shell_completion_emit(state, block.mount_point);
        }
    }
}

static void shell_completion_emit_display_targets(shell_completion_match_t *state)
{
    const size_t count = solar_os_display_target_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_display_target_t target;
        if (solar_os_display_get_target(i, &target)) {
            shell_completion_emit(state, target.name);
        }
    }
}

static void shell_completion_emit_input_sources(shell_completion_match_t *state,
                                                bool absolute_only)
{
    const size_t count = solar_os_input_source_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_input_source_info_t info;
        if (solar_os_input_source_get(i, &info) &&
            (!absolute_only ||
             (info.capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) != 0U)) {
            shell_completion_emit(state, info.name);
        }
    }
}

static bool shell_completion_display_mode_seen(char values[][32],
                                               size_t count,
                                               const char *value)
{
    if (value == NULL) {
        return true;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(values[i], value) == 0) {
            return true;
        }
    }
    return false;
}

static void shell_completion_emit_display_mode_values(shell_completion_match_t *state,
                                                      const char *values,
                                                      char emitted[][32],
                                                      size_t *emitted_count,
                                                      size_t emitted_max)
{
    if (values == NULL || emitted == NULL || emitted_count == NULL) {
        return;
    }

    const char *cursor = values;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        char value[32];
        size_t len = 0;
        while (cursor[len] != '\0' && cursor[len] != ' ' && len + 1 < sizeof(value)) {
            value[len] = cursor[len];
            len++;
        }
        value[len] = '\0';
        while (cursor[len] != '\0' && cursor[len] != ' ') {
            len++;
        }
        cursor += len;

        if (value[0] == '\0' ||
            shell_completion_display_mode_seen(emitted, *emitted_count, value)) {
            continue;
        }
        if (*emitted_count < emitted_max) {
            strlcpy(emitted[*emitted_count], value, sizeof(emitted[*emitted_count]));
            (*emitted_count)++;
        }
        shell_completion_emit(state, value);
    }
}

static void shell_completion_emit_display_modes(shell_completion_match_t *state,
                                                const char * const *tokens,
                                                size_t token_count)
{
    char emitted[16][32];
    size_t emitted_count = 0;

    if (token_count >= 3 && tokens[2] != NULL) {
        const char *values = NULL;
        if (solar_os_display_get_controller_mode(tokens[2], NULL, &values) == ESP_OK) {
            shell_completion_emit_display_mode_values(state,
                                                      values,
                                                      emitted,
                                                      &emitted_count,
                                                      SHELL_ARRAY_COUNT(emitted));
        }
        return;
    }

    const size_t count = solar_os_display_target_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_display_target_t target;
        const char *values = NULL;
        if (solar_os_display_get_target(i, &target) &&
            solar_os_display_get_controller_mode(target.name, NULL, &values) == ESP_OK) {
            shell_completion_emit_display_mode_values(state,
                                                      values,
                                                      emitted,
                                                      &emitted_count,
                                                      SHELL_ARRAY_COUNT(emitted));
        }
    }
}

static void shell_completion_emit_gpio_pins(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    char value[8];

    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info(i, &info) || !info.runtime_allowed) {
            continue;
        }
        snprintf(value, sizeof(value), "%d", info.pin);
        shell_completion_emit(state, value);
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_spi_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI);
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_i2c_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_I2C
    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C);
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_I2C, i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_onewire_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE);
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE, i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_ps2_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_PS2
    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_PS2);
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_PS2, i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_midi_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_UART
    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_MIDI);
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_MIDI, i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_uart_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_UART
    const size_t count = solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART);
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_UART, i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_com_arguments(shell_completion_match_t *state,
                                                const char * const *tokens,
                                                size_t token_count)
{
#if SOLAR_OS_PACKAGE_APP_COM
    bool port_seen = false;
    bool autobaud_seen = false;
    bool hex_seen = false;
    for (size_t i = 1; i < token_count; i++) {
        if (strcmp(tokens[i], "--autobaud") == 0) {
            autobaud_seen = true;
        } else if (strcmp(tokens[i], "--hex") == 0) {
            hex_seen = true;
        } else if (tokens[i][0] != '-') {
            port_seen = true;
        }
    }
    if (!autobaud_seen) {
        shell_completion_emit(state, com_options[0]);
    }
    if (!hex_seen) {
        shell_completion_emit(state, com_options[1]);
    }
    if (!port_seen) {
        shell_completion_emit_ports(state);
    }
#else
    (void)state;
    (void)tokens;
    (void)token_count;
#endif
}

static void shell_completion_emit_buses(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const size_t count = solar_os_bus_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_bus_info_t info;
        if (solar_os_bus_get(i, &info)) {
            shell_completion_emit(state, info.name);
        }
    }
#else
    (void)state;
#endif
}

static bool shell_completion_uart_named(const char * const *tokens, size_t token_count)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_UART
    return tokens != NULL && token_count >= 3 &&
        solar_os_bus_find(tokens[2], SOLAR_OS_BUS_PROTOCOL_UART, NULL);
#else
    (void)tokens;
    (void)token_count;
    return false;
#endif
}

static void shell_completion_emit_uart_values(shell_completion_match_t *state,
                                              const char * const *values,
                                              size_t count)
{
    for (size_t i = 0; i < count; i++) {
        shell_completion_emit(state, values[i]);
    }
}

static void shell_completion_emit_uart_arguments(shell_completion_match_t *state,
                                                 const char * const *tokens,
                                                 size_t token_count)
{
    if (tokens == NULL || token_count < 2) {
        return;
    }

    const char *operation = tokens[1];
    if (token_count == 2) {
        shell_completion_emit_uart_buses(state);
        if (strcmp(operation, "baud") == 0) {
            shell_completion_emit_uart_values(state,
                                              uart_baud_values,
                                              SHELL_ARRAY_COUNT(uart_baud_values));
        } else if (strcmp(operation, "mode") == 0) {
            shell_completion_emit_uart_values(state,
                                              uart_mode_values,
                                              SHELL_ARRAY_COUNT(uart_mode_values));
        } else if (strcmp(operation, "read") == 0) {
            shell_completion_emit_uart_values(state,
                                              uart_read_ms_values,
                                              SHELL_ARRAY_COUNT(uart_read_ms_values));
        }
        return;
    }

    if (token_count == 3 && shell_completion_uart_named(tokens, token_count)) {
        if (strcmp(operation, "baud") == 0) {
            shell_completion_emit_uart_values(state,
                                              uart_baud_values,
                                              SHELL_ARRAY_COUNT(uart_baud_values));
        } else if (strcmp(operation, "mode") == 0) {
            shell_completion_emit_uart_values(state,
                                              uart_mode_values,
                                              SHELL_ARRAY_COUNT(uart_mode_values));
        } else if (strcmp(operation, "read") == 0) {
            shell_completion_emit_uart_values(state,
                                              uart_read_ms_values,
                                              SHELL_ARRAY_COUNT(uart_read_ms_values));
        }
    }
}

static void shell_completion_emit_i2c_values(shell_completion_match_t *state,
                                             const char * const *values,
                                             size_t count)
{
    for (size_t i = 0; i < count; i++) {
        shell_completion_emit(state, values[i]);
    }
}

static bool shell_completion_i2c_named(const char * const *tokens, size_t token_count)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES && SOLAR_OS_PACKAGE_SERVICE_I2C
    return tokens != NULL && token_count >= 3 &&
        solar_os_bus_find(tokens[2], SOLAR_OS_BUS_PROTOCOL_I2C, NULL);
#else
    (void)tokens;
    (void)token_count;
    return false;
#endif
}

static void shell_completion_emit_i2c_arguments(shell_completion_match_t *state,
                                                const char * const *tokens,
                                                size_t token_count)
{
    if (tokens == NULL || token_count < 2) {
        return;
    }

    const char *operation = tokens[1];
    if (strcmp(operation, "speed") == 0) {
        if (token_count == 2) {
            shell_completion_emit_i2c_buses(state);
            shell_completion_emit_i2c_values(state,
                                              i2c_speed_values,
                                              SHELL_ARRAY_COUNT(i2c_speed_values));
        } else if (token_count == 3 &&
                   shell_completion_i2c_named(tokens, token_count)) {
            shell_completion_emit_i2c_values(state,
                                              i2c_speed_values,
                                              SHELL_ARRAY_COUNT(i2c_speed_values));
        }
        return;
    }
    if (strcmp(operation, "status") == 0 || strcmp(operation, "scan") == 0) {
        if (token_count == 2) {
            shell_completion_emit_i2c_buses(state);
        }
        return;
    }

    const bool named = shell_completion_i2c_named(tokens, token_count);
    if (strcmp(operation, "probe") == 0) {
        if (token_count == 2) {
            shell_completion_emit_i2c_buses(state);
            shell_completion_emit_i2c_values(state,
                                              i2c_addr_values,
                                              SHELL_ARRAY_COUNT(i2c_addr_values));
        } else if (token_count == 3 && named) {
            shell_completion_emit_i2c_values(state,
                                              i2c_addr_values,
                                              SHELL_ARRAY_COUNT(i2c_addr_values));
        }
        return;
    }

    if (strcmp(operation, "read") == 0 || strcmp(operation, "write") == 0) {
        if (token_count == 2) {
            shell_completion_emit_i2c_buses(state);
            shell_completion_emit_i2c_values(state,
                                              i2c_addr_values,
                                              SHELL_ARRAY_COUNT(i2c_addr_values));
        } else if (named && token_count == 3) {
            shell_completion_emit_i2c_values(state,
                                              i2c_addr_values,
                                              SHELL_ARRAY_COUNT(i2c_addr_values));
        } else if ((!named && token_count == 3) || (named && token_count == 4)) {
            shell_completion_emit_i2c_values(state,
                                              i2c_reg_values,
                                              SHELL_ARRAY_COUNT(i2c_reg_values));
        } else if (strcmp(operation, "read") == 0 &&
                   ((!named && token_count == 4) || (named && token_count == 5))) {
            shell_completion_emit_i2c_values(state,
                                              i2c_len_values,
                                              SHELL_ARRAY_COUNT(i2c_len_values));
        } else if (strcmp(operation, "write") == 0 &&
                   ((!named && token_count == 4) || (named && token_count == 5))) {
            shell_completion_emit_i2c_values(state,
                                              byte_values,
                                              SHELL_ARRAY_COUNT(byte_values));
        }
    }
}

static void shell_completion_emit_spi_cs(shell_completion_match_t *state,
                                         const char * const *tokens,
                                         size_t token_count)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_bus_info_t info;
    if (tokens == NULL || token_count < 3 ||
        !solar_os_bus_find(tokens[2], SOLAR_OS_BUS_PROTOCOL_SPI, &info)) {
        return;
    }

    for (size_t i = 0; i < info.config.spi.cs_count; i++) {
        char pin[8];
        shell_completion_emit(state, info.config.spi.cs[i].name);
        snprintf(pin, sizeof(pin), "%d", info.config.spi.cs[i].pin);
        shell_completion_emit(state, pin);
    }
#else
    (void)state;
    (void)tokens;
    (void)token_count;
#endif
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

static void shell_completion_emit_expansion_drivers(
    shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    const size_t count = solar_os_expansion_driver_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_expansion_driver_t driver;
        if (solar_os_expansion_get_driver(i, &driver) &&
            solar_os_expansion_driver_supported(driver.name)) {
            shell_completion_emit(state, driver.name);
        }
    }
#else
    (void)state;
#endif
}

static void shell_completion_emit_audio_outputs(shell_completion_match_t *state)
{
    shell_completion_emit(state, "auto");
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    const size_t count = solar_os_audio_device_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_audio_device_info_t device;
        if (solar_os_audio_device_get(i, &device) &&
            (device.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) != 0U &&
            device.playback_stream[0] != '\0') {
            shell_completion_emit(state, device.id);
        }
    }
#endif
}

#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
static void shell_completion_emit_controls(shell_completion_match_t *state)
{
    const size_t count = solar_os_control_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_control_info_t info;
        if (solar_os_control_get_info(i, &info)) {
            shell_completion_emit(state, info.config.name);
        }
    }
}

static void shell_completion_emit_parameters(shell_completion_match_t *state)
{
    const size_t count = solar_os_parameter_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_parameter_info_t info;
        if (solar_os_parameter_get_info(i, &info)) {
            shell_completion_emit(state, info.path);
        }
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_WIFI
static bool shell_completion_token_is_safe(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (isspace((unsigned char)*cursor)) {
            return false;
        }
    }
    return true;
}
#endif

static void shell_completion_emit_wifi_ssids(shell_completion_match_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;

    if (solar_os_wifi_known(profiles, SHELL_ARRAY_COUNT(profiles), &count) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count && i < SHELL_ARRAY_COUNT(profiles); i++) {
        if (shell_completion_token_is_safe(profiles[i].ssid)) {
            shell_completion_emit(state, profiles[i].ssid);
        }
    }
#else
    (void)state;
#endif
}

typedef enum {
    SHELL_DAQ_COMPLETION_SUBCOMMANDS,
    SHELL_DAQ_COMPLETION_OPTIONS,
    SHELL_DAQ_COMPLETION_RATE,
    SHELL_DAQ_COMPLETION_RATE_MS,
    SHELL_DAQ_COMPLETION_STREAMS_ALL,
    SHELL_DAQ_COMPLETION_STREAMS_BYTES,
    SHELL_DAQ_COMPLETION_STREAMS_CSV,
} shell_daq_completion_kind_t;

typedef struct {
    const char *positionals[SHELL_ARG_MAX];
    size_t positional_count;
    bool raw;
    bool first_pos_is_stream;
    bool stream_first_file_seen;
    solar_os_stream_type_t first_pos_type;
} shell_daq_completed_t;

static void shell_completion_init_state(solar_os_context_t *ctx,
                                        const char *prefix,
                                        bool print,
                                        shell_completion_match_t *state)
{
    memset(state, 0, sizeof(*state));
    state->ctx = ctx;
    state->io = shell_io(ctx);
    state->prefix = prefix;
    state->print = print;
}

#if SOLAR_OS_PACKAGE_APP_SSH || SOLAR_OS_PACKAGE_APP_SCP
typedef struct {
    shell_completion_match_t *matches;
    const char *authority_prefix;
    size_t authority_prefix_len;
} shell_ssh_host_completion_t;

static bool shell_completion_visit_ssh_host(const char *address,
                                            const char *alias,
                                            void *user)
{
    shell_ssh_host_completion_t *completion = (shell_ssh_host_completion_t *)user;
    char candidate[SHELL_INPUT_MAX];

    (void)address;
    const int written = snprintf(candidate,
                                 sizeof(candidate),
                                 "%.*s%s",
                                 (int)completion->authority_prefix_len,
                                 completion->authority_prefix,
                                 alias);
    if (written >= 0 && (size_t)written < sizeof(candidate)) {
        shell_completion_emit(completion->matches, candidate);
    }
    return true;
}

static bool shell_scp_host_position(const char * const *tokens, size_t token_count)
{
    size_t positional_count = 0U;

    for (size_t i = 1U; i < token_count; i++) {
        if (strcmp(tokens[i], "-P") == 0) {
            if (i + 1U >= token_count) {
                return false;
            }
            i++;
            continue;
        }
        positional_count++;
    }
    return positional_count < 2U;
}

static bool SHELL_NOINLINE shell_complete_ssh_host_argument(
    solar_os_context_t *ctx,
    const char *effective_command,
    const char * const *tokens,
    size_t token_count,
    const char *prefix,
    size_t token_start,
    bool show_matches)
{
    const bool ssh_command =
#if SOLAR_OS_PACKAGE_APP_SSH
        strcmp(effective_command, "ssh") == 0;
#else
        false;
#endif
    const bool scp_command =
#if SOLAR_OS_PACKAGE_APP_SCP
        strcmp(effective_command, "scp") == 0;
#else
        false;
#endif
    if ((!ssh_command && !scp_command) || prefix == NULL) {
        return false;
    }
    if ((ssh_command && token_count != 1U) ||
        (scp_command && !shell_scp_host_position(tokens, token_count))) {
        return false;
    }
    if (prefix[0] == '-' || strchr(prefix, ':') != NULL ||
        strchr(prefix, '/') != NULL || prefix[0] == '.') {
        return false;
    }

    const char *at = strchr(prefix, '@');
    if (at == prefix) {
        return false;
    }
    shell_completion_match_t matches;
    shell_completion_init_state(ctx, prefix, false, &matches);
    shell_ssh_host_completion_t completion = {
        .matches = &matches,
        .authority_prefix = prefix,
        .authority_prefix_len = at != NULL ? (size_t)(at - prefix) + 1U : 0U,
    };
    if (solar_os_ssh_hosts_visit(shell_completion_visit_ssh_host, &completion) != ESP_OK ||
        matches.count == 0U) {
        return false;
    }

    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;
    if (matches.count == 1U && !show_matches) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed,
                 sizeof(completed),
                 "%.*s%s%s",
                 (int)token_start,
                 shell_session(ctx)->input,
                 matches.match,
                 scp_command ? ":" : " ");
        shell_replace_input(ctx, completed);
        return true;
    }
    if (!show_matches && strlen(matches.match) > strlen(prefix)) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed,
                 sizeof(completed),
                 "%.*s%s",
                 (int)token_start,
                 shell_session(ctx)->input,
                 matches.match);
        shell_replace_input(ctx, completed);
        return true;
    }
    if (show_matches) {
        char original[SHELL_INPUT_MAX];
        strlcpy(original, shell_session(ctx)->input, sizeof(original));
        solar_os_shell_io_newline(shell_io(ctx));
        shell_completion_init_state(ctx, prefix, true, &matches);
        (void)solar_os_ssh_hosts_visit(shell_completion_visit_ssh_host, &completion);
        shell_prompt(ctx);
        shell_replace_input(ctx, original);
    }
    return true;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_OTA
static void shell_completion_emit_ota_flavors(shell_completion_match_t *state)
{
    const size_t count = solar_os_ota_available_flavor_count();
    for (size_t i = 0U; i < count; i++) {
        char flavor[SOLAR_OS_OTA_FLAVOR_MAX];
        if (solar_os_ota_get_available_flavor(i, flavor, sizeof(flavor))) {
            shell_completion_emit(state, flavor);
        }
    }
}

static void shell_completion_show_ota_message(solar_os_context_t *ctx,
                                              const char *message)
{
    char original[SHELL_INPUT_MAX];
    strlcpy(original, shell_session(ctx)->input, sizeof(original));
    solar_os_shell_io_newline(shell_io(ctx));
    solar_os_shell_io_writeln(shell_io(ctx), message);
    shell_prompt(ctx);
    shell_replace_input(ctx, original);
}

static bool shell_complete_ota_flavor_argument(
    solar_os_context_t *ctx,
    const char *effective_command,
    const shell_completion_parse_t *parse,
    size_t current_index,
    size_t token_start,
    bool show_matches)
{
    if (strcmp(effective_command, "ota") != 0 || current_index != 2U ||
        parse->count < 2U || strcmp(parse->tokens[1], "flavor") != 0) {
        return false;
    }

    if (!solar_os_ota_available_flavors_checked()) {
        if (show_matches) {
            shell_completion_show_ota_message(
                ctx, "ota flavor: run 'ota check' first");
        }
        return true;
    }

    const char *prefix = "";
    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

    shell_completion_match_t state;
    shell_completion_init_state(ctx, prefix, false, &state);
    shell_completion_emit_ota_flavors(&state);
    if (state.count == 0U) {
        if (show_matches) {
            shell_completion_show_ota_message(
                ctx, "ota flavor: no matching flavor for this board");
        }
        return true;
    }

    shell_session(ctx)->history_browsing = false;
    shell_session(ctx)->history_index = -1;
    if (state.count == 1U && !show_matches) {
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
        char original[SHELL_INPUT_MAX];
        strlcpy(original, shell_session(ctx)->input, sizeof(original));
        solar_os_shell_io_newline(shell_io(ctx));
        shell_completion_init_state(ctx, prefix, true, &state);
        shell_completion_emit_ota_flavors(&state);
        shell_prompt(ctx);
        shell_replace_input(ctx, original);
    }
    return true;
}
#endif

#if SOLAR_OS_PACKAGE_APP_FLASH
static bool shell_flash_artifact_seen_before(
    const solar_os_flash_catalog_t *catalog,
    size_t index,
    bool match_flavor,
    const char *board_id)
{
    const solar_os_flash_artifact_t *artifact = &catalog->artifacts[index];
    for (size_t i = 0; i < index; i++) {
        const solar_os_flash_artifact_t *earlier = &catalog->artifacts[i];
        if (!earlier->cached ||
            strcmp(earlier->board_id, artifact->board_id) != 0) {
            continue;
        }
        if (!match_flavor ||
            (strcmp(artifact->board_id, board_id) == 0 &&
             strcmp(earlier->flavor, artifact->flavor) == 0)) {
            return true;
        }
    }
    return false;
}

static void shell_completion_emit_flash_values(
    shell_completion_match_t *state,
    const solar_os_flash_catalog_t *catalog,
    const shell_completion_parse_t *parse,
    size_t current_index)
{
    if (current_index == 1U) {
        for (size_t i = 0; i < catalog->count; i++) {
            const solar_os_flash_artifact_t *artifact = &catalog->artifacts[i];
            if (artifact->cached &&
                !shell_flash_artifact_seen_before(catalog, i, false, NULL)) {
                shell_completion_emit(state, artifact->board_id);
            }
        }
        return;
    }
    if (current_index != 2U || parse->count < 2U) {
        return;
    }
    const char *board_id = parse->tokens[1];
    for (size_t i = 0; i < catalog->count; i++) {
        const solar_os_flash_artifact_t *artifact = &catalog->artifacts[i];
        if (artifact->cached &&
            strcmp(artifact->board_id, board_id) == 0 &&
            !shell_flash_artifact_seen_before(catalog, i, true, board_id)) {
            shell_completion_emit(state, artifact->flavor);
        }
    }
}

static bool shell_complete_flash_argument(
    solar_os_context_t *ctx,
    const char *effective_command,
    const shell_completion_parse_t *parse,
    size_t current_index,
    size_t token_start,
    bool show_matches)
{
    if (strcmp(effective_command, "flash") != 0 || current_index > 2U) {
        return false;
    }
    const char *prefix = "";
    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

    solar_os_flash_catalog_t *catalog = NULL;
    if (solar_os_flash_catalog_load(&catalog) != ESP_OK) {
        return true;
    }

    shell_completion_match_t state;
    shell_completion_init_state(ctx, prefix, false, &state);
    shell_completion_emit_flash_values(&state, catalog, parse, current_index);
    if (state.count == 1U && !show_matches) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed,
                 sizeof(completed),
                 "%.*s%s ",
                 (int)token_start,
                 shell_session(ctx)->input,
                 state.match);
        shell_replace_input(ctx, completed);
    } else if (show_matches && state.count > 0U) {
        char original[SHELL_INPUT_MAX];
        strlcpy(original, shell_session(ctx)->input, sizeof(original));
        solar_os_shell_io_newline(shell_io(ctx));
        shell_completion_init_state(ctx, prefix, true, &state);
        shell_completion_emit_flash_values(&state,
                                           catalog,
                                           parse,
                                           current_index);
        shell_prompt(ctx);
        shell_replace_input(ctx, original);
    }
    solar_os_flash_catalog_free(catalog);
    return true;
}
#endif

static bool shell_token_looks_like_path(const char *token)
{
    return token != NULL &&
        (token[0] == '/' ||
         token[0] == '.' ||
         strchr(token, '/') != NULL ||
         strchr(token, '*') != NULL ||
         strchr(token, '?') != NULL);
}

static bool shell_daq_option_takes_value(const char *token)
{
    return token != NULL &&
        (strcmp(token, "--rate") == 0 ||
         strcmp(token, "--rate-ms") == 0);
}

static bool shell_daq_stream_type_allowed(solar_os_stream_type_t type,
                                          shell_daq_completion_kind_t kind)
{
    switch (kind) {
    case SHELL_DAQ_COMPLETION_STREAMS_BYTES:
        return type == SOLAR_OS_STREAM_TYPE_BYTES ||
            type == SOLAR_OS_STREAM_TYPE_AUDIO;
    case SHELL_DAQ_COMPLETION_STREAMS_CSV:
        return type != SOLAR_OS_STREAM_TYPE_BYTES &&
            type != SOLAR_OS_STREAM_TYPE_AUDIO;
    case SHELL_DAQ_COMPLETION_STREAMS_ALL:
        return true;
    default:
        return false;
    }
}

static void shell_completion_emit_daq_streams(shell_completion_match_t *state,
                                              shell_daq_completion_kind_t kind)
{
    const size_t count = solar_os_stream_count();

    for (size_t i = 0; i < count; i++) {
        solar_os_stream_info_t info;
        if (solar_os_stream_get(i, &info) &&
            shell_daq_stream_type_allowed(info.type, kind)) {
            shell_completion_emit(state, info.id);
        }
    }
}

static void shell_completion_emit_daq_kind(shell_completion_match_t *state,
                                           shell_daq_completion_kind_t kind)
{
    switch (kind) {
    case SHELL_DAQ_COMPLETION_SUBCOMMANDS:
        for (size_t i = 0; i < SHELL_ARRAY_COUNT(daq_subcommands); i++) {
            shell_completion_emit(state, daq_subcommands[i]);
        }
        break;
    case SHELL_DAQ_COMPLETION_OPTIONS:
        for (size_t i = 0; i < SHELL_ARRAY_COUNT(daq_options); i++) {
            shell_completion_emit(state, daq_options[i]);
        }
        break;
    case SHELL_DAQ_COMPLETION_RATE:
        for (size_t i = 0; i < SHELL_ARRAY_COUNT(daq_rate_values); i++) {
            shell_completion_emit(state, daq_rate_values[i]);
        }
        break;
    case SHELL_DAQ_COMPLETION_RATE_MS:
        for (size_t i = 0; i < SHELL_ARRAY_COUNT(daq_rate_ms_values); i++) {
            shell_completion_emit(state, daq_rate_ms_values[i]);
        }
        break;
    case SHELL_DAQ_COMPLETION_STREAMS_ALL:
    case SHELL_DAQ_COMPLETION_STREAMS_BYTES:
    case SHELL_DAQ_COMPLETION_STREAMS_CSV:
        shell_completion_emit_daq_streams(state, kind);
        break;
    }
}

static bool shell_complete_daq_kind(solar_os_context_t *ctx,
                                    shell_daq_completion_kind_t kind,
                                    const char *prefix,
                                    size_t token_start,
                                    bool show_matches)
{
    shell_completion_match_t state;
    shell_completion_init_state(ctx, prefix, false, &state);
    shell_completion_emit_daq_kind(&state, kind);
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

    if (!show_matches && strlen(state.match) > strlen(prefix)) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed,
                 sizeof(completed),
                 "%.*s%s",
                 (int)token_start,
                 shell_session(ctx)->input,
                 state.match);
        shell_replace_input(ctx, completed);
        return true;
    }

    if (show_matches) {
        char original[SHELL_INPUT_MAX];
        strlcpy(original, shell_session(ctx)->input, sizeof(original));
        solar_os_shell_io_newline(shell_io(ctx));
        shell_completion_init_state(ctx, prefix, true, &state);
        shell_completion_emit_daq_kind(&state, kind);
        shell_prompt(ctx);
        shell_replace_input(ctx, original);
    }

    return true;
}

static bool shell_daq_kind_has_match(solar_os_context_t *ctx,
                                     shell_daq_completion_kind_t kind,
                                     const char *prefix)
{
    shell_completion_match_t state;
    shell_completion_init_state(ctx, prefix, false, &state);
    shell_completion_emit_daq_kind(&state, kind);
    return state.count > 0;
}

static void shell_daq_analyze_completed(const shell_completion_parse_t *parse,
                                        size_t current_index,
                                        shell_daq_completed_t *completed)
{
    bool skip_value = false;

    memset(completed, 0, sizeof(*completed));

    for (size_t i = 2; i < current_index && i < parse->count; i++) {
        const char *token = parse->tokens[i];

        if (skip_value) {
            skip_value = false;
            continue;
        }
        if (strcmp(token, "--raw") == 0) {
            completed->raw = true;
            continue;
        }
        if (shell_daq_option_takes_value(token)) {
            skip_value = true;
            continue;
        }
        if (token[0] == '-') {
            continue;
        }
        if (completed->positional_count < SHELL_ARRAY_COUNT(completed->positionals)) {
            completed->positionals[completed->positional_count++] = token;
        }
    }

    if (completed->positional_count > 0) {
        solar_os_stream_info_t info;
        if (solar_os_stream_get_info(completed->positionals[0], &info) == ESP_OK) {
            completed->first_pos_is_stream = true;
            completed->first_pos_type = info.type;
        }
    }
    if (completed->first_pos_is_stream) {
        for (size_t i = 1; i < completed->positional_count; i++) {
            solar_os_stream_info_t info;
            if (solar_os_stream_get_info(completed->positionals[i], &info) != ESP_OK) {
                completed->stream_first_file_seen = true;
                break;
            }
        }
    }
}

static bool shell_complete_daq_start(solar_os_context_t *ctx,
                                     const shell_completion_parse_t *parse,
                                     size_t current_index,
                                     size_t token_start,
                                     bool show_matches)
{
    const char *prefix = "";
    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

    if (current_index > 2 && shell_daq_option_takes_value(parse->tokens[current_index - 1])) {
        if (strcmp(parse->tokens[current_index - 1], "--rate") == 0) {
            return shell_complete_daq_kind(ctx,
                                           SHELL_DAQ_COMPLETION_RATE,
                                           prefix,
                                           token_start,
                                           show_matches);
        }
        return shell_complete_daq_kind(ctx,
                                       SHELL_DAQ_COMPLETION_RATE_MS,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    if (prefix[0] == '-') {
        return shell_complete_daq_kind(ctx,
                                       SHELL_DAQ_COMPLETION_OPTIONS,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    shell_daq_completed_t completed;
    shell_daq_analyze_completed(parse, current_index, &completed);

    if (completed.positional_count == 0) {
        if (shell_token_looks_like_path(prefix)) {
            shell_complete_path(ctx, token_start, prefix, false, show_matches);
            return true;
        }
        return shell_complete_daq_kind(ctx,
                                       completed.raw ?
                                       SHELL_DAQ_COMPLETION_STREAMS_BYTES :
                                       SHELL_DAQ_COMPLETION_STREAMS_ALL,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    if (!completed.first_pos_is_stream) {
        return shell_complete_daq_kind(ctx,
                                       completed.raw ?
                                       SHELL_DAQ_COMPLETION_STREAMS_BYTES :
                                       SHELL_DAQ_COMPLETION_STREAMS_ALL,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    if (completed.raw || completed.first_pos_type == SOLAR_OS_STREAM_TYPE_BYTES ||
        completed.first_pos_type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        if (completed.positional_count == 1) {
            shell_complete_path(ctx, token_start, prefix, false, show_matches);
            return true;
        }
        return shell_complete_daq_kind(ctx,
                                       SHELL_DAQ_COMPLETION_OPTIONS,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    if (completed.stream_first_file_seen) {
        return shell_complete_daq_kind(ctx,
                                       SHELL_DAQ_COMPLETION_OPTIONS,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    if (prefix[0] != '\0' &&
        !shell_token_looks_like_path(prefix) &&
        shell_daq_kind_has_match(ctx, SHELL_DAQ_COMPLETION_STREAMS_CSV, prefix)) {
        return shell_complete_daq_kind(ctx,
                                       SHELL_DAQ_COMPLETION_STREAMS_CSV,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    shell_complete_path(ctx, token_start, prefix, false, show_matches);
    return true;
}

static bool shell_complete_daq_argument(solar_os_context_t *ctx,
                                        const char *effective_command,
                                        const shell_completion_parse_t *parse,
                                        size_t current_index,
                                        size_t token_start,
                                        bool show_matches)
{
    const char *prefix = "";

    if (strcmp(effective_command, "daq") != 0) {
        return false;
    }
    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

    if (current_index == 1) {
        return shell_complete_daq_kind(ctx,
                                       SHELL_DAQ_COMPLETION_SUBCOMMANDS,
                                       prefix,
                                       token_start,
                                       show_matches);
    }

    if (strcmp(parse->tokens[1], "start") == 0) {
        return shell_complete_daq_start(ctx, parse, current_index, token_start, show_matches);
    }

    return true;
}

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
static const solar_os_expansion_binding_spec_t shell_manual_expansion_specs[] = {
    {.key = "i2c", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS},
    {.key = "spi", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS},
    {.key = "cs", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS},
    {.key = "uart", .kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT},
    {.key = "ps2", .kind = SOLAR_OS_EXPANSION_BINDING_PS2_BUS},
    {.key = "x", .kind = SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM, .role = "x"},
    {.key = "y", .kind = SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM, .role = "y"},
    {.key = "addr", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS},
    {.key = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "gpio"},
    {.key = "irq", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    {.key = "reset", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
    {.key = "dc", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc"},
    {.key = "busy", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy"},
    {.key = "min", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "min"},
    {.key = "center", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "center"},
    {.key = "max", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "max"},
    {.key = "deadzone", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "deadzone"},
};

static bool shell_expansion_find_driver(const char *name,
                                        solar_os_expansion_driver_t *driver)
{
    for (size_t i = 0; i < solar_os_expansion_driver_count(); i++) {
        if (solar_os_expansion_get_driver(i, driver) && strcmp(driver->name, name) == 0) {
            return true;
        }
    }
    return false;
}

static const char *shell_expansion_token_key(const char *token,
                                             char *key,
                                             size_t key_len)
{
    const char *eq = token != NULL ? strchr(token, '=') : NULL;
    if (eq == NULL || eq == token || (size_t)(eq - token) >= key_len) {
        return NULL;
    }
    memcpy(key, token, (size_t)(eq - token));
    key[eq - token] = '\0';
    if (strcmp(key, "ce") == 0) {
        strlcpy(key, "cs", key_len);
    } else if (strcmp(key, "rst") == 0) {
        strlcpy(key, "reset", key_len);
    }
    return key;
}

static bool shell_expansion_spec_used(const shell_completion_parse_t *parse,
                                      size_t current_index,
                                      const char *key)
{
    for (size_t i = 4; i < current_index && i < parse->count; i++) {
        char token_key[SOLAR_OS_EXPANSION_ROLE_MAX];
        if (shell_expansion_token_key(parse->tokens[i], token_key, sizeof(token_key)) != NULL &&
            strcmp(token_key, key) == 0) {
            return true;
        }
    }
    return false;
}

static const char *shell_expansion_selected_spi(const shell_completion_parse_t *parse,
                                                size_t current_index)
{
    for (size_t i = 4; i < current_index && i < parse->count; i++) {
        if (starts_with(parse->tokens[i], "spi=") && parse->tokens[i][4] != '\0') {
            return &parse->tokens[i][4];
        }
    }
    return NULL;
}

static void shell_completion_emit_expansion_gpio(shell_completion_match_t *state,
                                                 const char *key)
{
    char candidate[32];
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info(i, &info) || !info.available) {
            continue;
        }
        snprintf(candidate, sizeof(candidate), "%s=gpio%d", key, info.pin);
        shell_completion_emit(state, candidate);
    }
}

static void shell_completion_emit_expansion_spec(
    shell_completion_match_t *state,
    const solar_os_expansion_binding_spec_t *spec,
    const shell_completion_parse_t *parse,
    size_t current_index)
{
    char candidate[40];

    if (spec->allowed_value_count > 0) {
        for (size_t i = 0; i < spec->allowed_value_count; i++) {
            snprintf(candidate, sizeof(candidate), "%s=0x%02x", spec->key, spec->allowed_values[i]);
            shell_completion_emit(state, candidate);
        }
        return;
    }

    switch (spec->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        for (size_t i = 0; i < solar_os_expansion_i2c_bus_count(); i++) {
            solar_os_expansion_i2c_bus_t bus;
            if (solar_os_expansion_get_i2c_bus(i, &bus)) {
                snprintf(candidate, sizeof(candidate), "%s=%s", spec->key, bus.name);
                shell_completion_emit(state, candidate);
            }
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        for (size_t i = 0; i < solar_os_expansion_spi_bus_count(); i++) {
            solar_os_expansion_spi_bus_t bus;
            if (solar_os_expansion_get_spi_bus(i, &bus)) {
                snprintf(candidate, sizeof(candidate), "%s=%s", spec->key, bus.name);
                shell_completion_emit(state, candidate);
            }
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        for (size_t i = 0; i < solar_os_expansion_uart_port_count(); i++) {
            solar_os_expansion_uart_port_t port;
            if (solar_os_expansion_get_uart_port(i, &port)) {
                snprintf(candidate, sizeof(candidate), "%s=%s", spec->key, port.name);
                shell_completion_emit(state, candidate);
            }
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        for (size_t i = 0; i < solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_PS2); i++) {
            solar_os_bus_info_t bus;
            if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_PS2, i, &bus)) {
                snprintf(candidate, sizeof(candidate), "%s=%s", spec->key, bus.name);
                shell_completion_emit(state, candidate);
            }
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        for (size_t i = 0; i < solar_os_stream_count(); i++) {
            solar_os_stream_info_t info;
            if (solar_os_stream_get(i, &info) &&
                info.type == SOLAR_OS_STREAM_TYPE_SCALAR &&
                info.direction != SOLAR_OS_STREAM_DIRECTION_SINK) {
                snprintf(candidate, sizeof(candidate), "%s=%s", spec->key, info.id);
                shell_completion_emit(state, candidate);
            }
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS: {
        const char *selected = shell_expansion_selected_spi(parse, current_index);
        for (size_t i = 0; i < solar_os_expansion_spi_bus_count(); i++) {
            solar_os_expansion_spi_bus_t bus;
            if (!solar_os_expansion_get_spi_bus(i, &bus) ||
                (selected != NULL && strcmp(selected, bus.name) != 0)) {
                continue;
            }
            for (size_t cs = 0; cs < bus.cs_count; cs++) {
                snprintf(candidate, sizeof(candidate), "%s=gpio%d", spec->key, bus.cs[cs].pin);
                shell_completion_emit(state, candidate);
            }
        }
        break;
    }
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        shell_completion_emit_expansion_gpio(state, spec->key);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        shell_completion_emit(state, "addr=0x3c");
        shell_completion_emit(state, "addr=0x3d");
        break;
    default:
        break;
    }
}

static void shell_completion_emit_expansion_bindings(
    shell_completion_match_t *state,
    const solar_os_expansion_driver_t *driver,
    const shell_completion_parse_t *parse,
    size_t current_index)
{
    const solar_os_expansion_binding_spec_t *specs = driver->binding_specs;
    size_t spec_count = driver->binding_spec_count;
    if (driver->allow_unlisted_bindings) {
        specs = shell_manual_expansion_specs;
        spec_count = SHELL_ARRAY_COUNT(shell_manual_expansion_specs);
    }
    for (size_t i = 0; i < spec_count; i++) {
        if (!shell_expansion_spec_used(parse, current_index, specs[i].key)) {
            shell_completion_emit_expansion_spec(state, &specs[i], parse, current_index);
        }
    }
}

static bool shell_complete_expansion_argument(solar_os_context_t *ctx,
                                              const char *effective_command,
                                              const shell_completion_parse_t *parse,
                                              size_t current_index,
                                              size_t token_start,
                                              bool show_matches)
{
    if (strcmp(effective_command, "expansion") != 0 || current_index < 3 ||
        parse->count < 3 || strcmp(parse->tokens[1], "attach") != 0) {
        return false;
    }
    if (current_index == 3) {
        return true;
    }

    solar_os_expansion_driver_t driver;
    if (!shell_expansion_find_driver(parse->tokens[2], &driver)) {
        return true;
    }
    const char *prefix = "";
    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

    shell_completion_match_t state;
    shell_completion_init_state(ctx, prefix, false, &state);
    shell_completion_emit_expansion_bindings(&state, &driver, parse, current_index);
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
        char original[SHELL_INPUT_MAX];
        strlcpy(original, shell_session(ctx)->input, sizeof(original));
        solar_os_shell_io_newline(shell_io(ctx));
        shell_completion_init_state(ctx, prefix, true, &state);
        shell_completion_emit_expansion_bindings(&state, &driver, parse, current_index);
        shell_prompt(ctx);
        shell_replace_input(ctx, original);
    }
    return true;
}
#endif

static bool shell_completion_collect_matches(solar_os_context_t *ctx,
                                             const char * const *tokens,
                                             size_t token_count,
                                             const char *prefix,
                                             bool print,
                                             shell_completion_match_t *state)
{
    bool rule_seen = false;
    size_t best_wildcards = SHELL_ARG_MAX + 1U;

    if (state == NULL || tokens == NULL || token_count == 0U) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->ctx = ctx;
    state->io = shell_io(ctx);
    state->prefix = prefix;
    state->print = print;

    for (uint16_t rule_index = shell_completion_rule_first(tokens[0]);
         rule_index != SHELL_COMPLETION_RULE_NONE;
         rule_index = shell_completion_rule_next(rule_index, tokens[0])) {
        const shell_completion_rule_t *rule = &shell_completion_rules[rule_index];
        size_t wildcards = 0;

        if ((rule->flags & SHELL_COMPLETION_FLAG_PATH) != 0U ||
            !shell_completion_path_matches(rule, tokens, token_count, &wildcards)) {
            continue;
        }
        if (rule->required_prefix != NULL &&
            (prefix == NULL || !starts_with(prefix, rule->required_prefix))) {
            continue;
        }
        if (wildcards < best_wildcards) {
            best_wildcards = wildcards;
        }
    }

    for (uint16_t rule_index = shell_completion_rule_first(tokens[0]);
         rule_index != SHELL_COMPLETION_RULE_NONE;
         rule_index = shell_completion_rule_next(rule_index, tokens[0])) {
        const shell_completion_rule_t *rule = &shell_completion_rules[rule_index];
        size_t wildcards = 0;

        if ((rule->flags & SHELL_COMPLETION_FLAG_PATH) != 0U ||
            !shell_completion_path_matches(rule, tokens, token_count, &wildcards) ||
            wildcards != best_wildcards) {
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
        switch (rule->source) {
        case SHELL_COMPLETION_SOURCE_NONE:
            break;
        case SHELL_COMPLETION_SOURCE_COMMANDS:
            shell_completion_emit_commands(state);
            break;
        case SHELL_COMPLETION_SOURCE_APPS:
            shell_completion_emit_apps(state);
            break;
        case SHELL_COMPLETION_SOURCE_JOBS:
            shell_completion_emit_jobs(state);
            break;
        case SHELL_COMPLETION_SOURCE_MANUAL_PAGES:
            shell_completion_emit_manual_pages(state);
            break;
        case SHELL_COMPLETION_SOURCE_MANUAL_REFERENCES:
            shell_completion_emit_manual_references(state);
            break;
        case SHELL_COMPLETION_SOURCE_AGENT_CONVERSATIONS:
            shell_completion_emit_agent_conversations(state);
            break;
        case SHELL_COMPLETION_SOURCE_INBOX_IDS:
            shell_completion_emit_inbox_ids(state);
            break;
        case SHELL_COMPLETION_SOURCE_MESSAGE_IDS:
            shell_completion_emit_message_ids(state);
            break;
        case SHELL_COMPLETION_SOURCE_CONTACT_IDS:
            shell_completion_emit_contact_ids(state);
            break;
        case SHELL_COMPLETION_SOURCE_ENDPOINT_IDS:
            shell_completion_emit_endpoint_ids(state);
            break;
        case SHELL_COMPLETION_SOURCE_PLAYGROUND_APPS:
            shell_completion_emit_playground_apps(state);
            break;
        case SHELL_COMPLETION_SOURCE_AUDIO_OUTPUTS:
            shell_completion_emit_audio_outputs(state);
            break;
        case SHELL_COMPLETION_SOURCE_EXPANSION_DRIVERS:
            shell_completion_emit_expansion_drivers(state);
            break;
        case SHELL_COMPLETION_SOURCE_EXPANSION_DEVICES:
            shell_completion_emit_expansion_devices(state);
            break;
        case SHELL_COMPLETION_SOURCE_CONNECTORS:
            shell_completion_emit_connectors(state);
            break;
        case SHELL_COMPLETION_SOURCE_DISPLAY_SESSION_IDS:
            shell_completion_emit_display_session_ids(state);
            break;
        case SHELL_COMPLETION_SOURCE_SESSION_IDS:
            shell_completion_emit_session_ids(state);
            break;
        case SHELL_COMPLETION_SOURCE_PORTS:
            shell_completion_emit_ports(state);
            break;
        case SHELL_COMPLETION_SOURCE_RADIOS:
            shell_completion_emit_radios(state);
            break;
        case SHELL_COMPLETION_SOURCE_LINKS:
            shell_completion_emit_links(state);
            break;
        case SHELL_COMPLETION_SOURCE_LINK_STREAMS:
            shell_completion_emit_link_streams(state);
            break;
        case SHELL_COMPLETION_SOURCE_MESHCORE_STREAMS:
            shell_completion_emit_meshcore_streams(state);
            break;
        case SHELL_COMPLETION_SOURCE_RADIO_PROFILES:
            shell_completion_emit_radio_profiles(state, false);
            break;
        case SHELL_COMPLETION_SOURCE_USER_RADIO_PROFILES:
            shell_completion_emit_radio_profiles(state, true);
            break;
        case SHELL_COMPLETION_SOURCE_RAMFS_MOUNTS:
            shell_completion_emit_ramfs_mounts(state);
            break;
        case SHELL_COMPLETION_SOURCE_STORAGE_MOUNTABLES:
            shell_completion_emit_storage_mountables(state);
            break;
        case SHELL_COMPLETION_SOURCE_STORAGE_BLOCKS:
            shell_completion_emit_storage_blocks(state);
            break;
        case SHELL_COMPLETION_SOURCE_STORAGE_UNMOUNT_TARGETS:
            shell_completion_emit_storage_unmount_targets(state);
            break;
        case SHELL_COMPLETION_SOURCE_DISPLAY_TARGETS:
            shell_completion_emit_display_targets(state);
            break;
        case SHELL_COMPLETION_SOURCE_DISPLAY_MODES:
            shell_completion_emit_display_modes(state, tokens, token_count);
            break;
        case SHELL_COMPLETION_SOURCE_INPUT_SOURCES:
            shell_completion_emit_input_sources(
                state,
                (rule->flags & SHELL_COMPLETION_FLAG_ABSOLUTE_POINTERS) != 0U);
            break;
        case SHELL_COMPLETION_SOURCE_GPIO_PINS:
            shell_completion_emit_gpio_pins(state);
            break;
        case SHELL_COMPLETION_SOURCE_I2C_ARGUMENTS:
            shell_completion_emit_i2c_arguments(state, tokens, token_count);
            break;
        case SHELL_COMPLETION_SOURCE_ONEWIRE_BUSES:
            shell_completion_emit_onewire_buses(state);
            break;
        case SHELL_COMPLETION_SOURCE_PS2_BUSES:
            shell_completion_emit_ps2_buses(state);
            break;
        case SHELL_COMPLETION_SOURCE_MIDI_BUSES:
            shell_completion_emit_midi_buses(state);
            break;
        case SHELL_COMPLETION_SOURCE_SPI_BUSES:
            shell_completion_emit_spi_buses(state);
            break;
        case SHELL_COMPLETION_SOURCE_UART_BUSES:
            shell_completion_emit_uart_buses(state);
            break;
        case SHELL_COMPLETION_SOURCE_COM_ARGUMENTS:
            shell_completion_emit_com_arguments(state, tokens, token_count);
            break;
        case SHELL_COMPLETION_SOURCE_UART_ARGUMENTS:
            shell_completion_emit_uart_arguments(state, tokens, token_count);
            break;
        case SHELL_COMPLETION_SOURCE_BUSES:
            shell_completion_emit_buses(state);
            break;
        case SHELL_COMPLETION_SOURCE_SPI_CS:
            shell_completion_emit_spi_cs(state, tokens, token_count);
            break;
        case SHELL_COMPLETION_SOURCE_STREAMS:
            shell_completion_emit_streams(
                state,
                (rule->flags & SHELL_COMPLETION_FLAG_SCALAR_STREAMS) != 0U);
            break;
        case SHELL_COMPLETION_SOURCE_SCHEDULE_ENTRIES:
            shell_completion_emit_schedule_entries(state);
            break;
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
        case SHELL_COMPLETION_SOURCE_CONTROLS:
            shell_completion_emit_controls(state);
            break;
        case SHELL_COMPLETION_SOURCE_PARAMETERS:
            shell_completion_emit_parameters(state);
            break;
#else
        case SHELL_COMPLETION_SOURCE_CONTROLS:
        case SHELL_COMPLETION_SOURCE_PARAMETERS:
            break;
#endif
        case SHELL_COMPLETION_SOURCE_WIFI_SSIDS:
            shell_completion_emit_wifi_ssids(state);
            break;
        }
    }

    return rule_seen;
}

static const shell_completion_rule_t *shell_completion_find_path_rule(const char * const *tokens,
                                                                      size_t token_count)
{
    const shell_completion_rule_t *best = NULL;
    size_t best_wildcards = SHELL_ARG_MAX + 1;

    if (tokens == NULL || token_count == 0U) {
        return NULL;
    }

    for (uint16_t rule_index = shell_completion_rule_first(tokens[0]);
         rule_index != SHELL_COMPLETION_RULE_NONE;
         rule_index = shell_completion_rule_next(rule_index, tokens[0])) {
        const shell_completion_rule_t *rule = &shell_completion_rules[rule_index];
        size_t wildcards = 0;

        if ((rule->flags & SHELL_COMPLETION_FLAG_PATH) == 0U ||
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
    char *alias_tokens[SHELL_ARG_MAX] = {0};
    const char *prefix = "";
    size_t completed_count = current_index;
    shell_completion_match_t state;

    if (parse == NULL || parse->count == 0 || current_index >= SHELL_ARG_MAX) {
        return false;
    }

    for (size_t i = 0; i < completed_count; i++) {
        completed_tokens[i] = parse->tokens[i];
    }

    size_t alias_count = 0;
    if (shell_completion_expand_alias(parse,
                                      completed_count,
                                      effective_command,
                                      sizeof(effective_command),
                                      alias_tokens,
                                      &alias_count)) {
        completed_count = alias_count;
        for (size_t i = 0; i < completed_count; i++) {
            completed_tokens[i] = alias_tokens[i];
        }
    } else if (!shell_alias_lookup_target_command(parse->tokens[0],
                                                  effective_command,
                                                  sizeof(effective_command))) {
        strlcpy(effective_command, parse->tokens[0], sizeof(effective_command));
    }
    if (alias_count == 0) {
        completed_tokens[0] = effective_command;
    }

    if (!parse->trailing_space && current_index < parse->count) {
        prefix = parse->tokens[current_index];
    }

#if SOLAR_OS_PACKAGE_APP_SSH || SOLAR_OS_PACKAGE_APP_SCP
    if (shell_complete_ssh_host_argument(ctx,
                                         effective_command,
                                         completed_tokens,
                                         completed_count,
                                         prefix,
                                         token_start,
                                         show_matches)) {
        return true;
    }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_OTA
    if (shell_complete_ota_flavor_argument(ctx,
                                           effective_command,
                                           parse,
                                           current_index,
                                           token_start,
                                           show_matches)) {
        return true;
    }
#endif
    if (shell_complete_daq_argument(ctx,
                                    effective_command,
                                    parse,
                                    current_index,
                                    token_start,
                                    show_matches)) {
        return true;
    }
#if SOLAR_OS_PACKAGE_APP_FLASH
    if (shell_complete_flash_argument(ctx,
                                      effective_command,
                                      parse,
                                      current_index,
                                      token_start,
                                      show_matches)) {
        return true;
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (shell_complete_expansion_argument(ctx,
                                          effective_command,
                                          parse,
                                          current_index,
                                          token_start,
                                          show_matches)) {
        return true;
    }
#endif

    const shell_completion_rule_t *path_rule =
        shell_completion_find_path_rule(completed_tokens, completed_count);

    const bool rule_seen = shell_completion_collect_matches(ctx,
                                                            completed_tokens,
                                                            completed_count,
                                                            prefix,
                                                            false,
                                                            &state);
    if (!rule_seen) {
        if (path_rule != NULL) {
            shell_complete_path(
                ctx,
                token_start,
                prefix,
                (path_rule->flags & SHELL_COMPLETION_FLAG_DIRS_ONLY) != 0U,
                show_matches);
            return true;
        }
        return false;
    }
    if (state.count == 0) {
        if (path_rule != NULL) {
            shell_complete_path(
                ctx,
                token_start,
                prefix,
                (path_rule->flags & SHELL_COMPLETION_FLAG_DIRS_ONLY) != 0U,
                show_matches);
            return true;
        }
        return false;
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

    if (!show_matches && strlen(state.match) > strlen(prefix)) {
        char completed[SHELL_INPUT_MAX];
        snprintf(completed,
                 sizeof(completed),
                 "%.*s%s",
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

    shell_completion_parse_t *parse = shell_alloc_completion_parse();
    if (parse == NULL) {
        return;
    }
    if (!shell_completion_parse_input(ctx, parse) || parse->count == 0) {
        solar_os_memory_free(parse);
        if (show_matches && shell_session(ctx)->input_len == 0) {
            shell_print_builtin_command_matches(ctx, NULL);
        }
        return;
    }

    const size_t current_index = parse->trailing_space ? parse->count : parse->count - 1;
    if (current_index == 0 && !parse->trailing_space) {
        solar_os_memory_free(parse);
        shell_complete_builtin_command(ctx, show_matches);
        return;
    }

    const char *command = parse->tokens[0];
    char effective_command[SHELL_INPUT_MAX];
    if (!shell_alias_lookup_target_command(command, effective_command, sizeof(effective_command))) {
        strlcpy(effective_command, command, sizeof(effective_command));
    }

    const size_t token_start =
        parse->trailing_space ? shell_session(ctx)->input_len : parse->starts[current_index];
    if (shell_complete_argument(ctx, parse, current_index, token_start, show_matches)) {
        solar_os_memory_free(parse);
        return;
    }

    if (!shell_is_path_command(effective_command)) {
        solar_os_memory_free(parse);
        return;
    }
    if (strcmp(effective_command, "scp") == 0 &&
        memchr(&shell_session(ctx)->input[token_start], ':', shell_session(ctx)->input_len - token_start) != NULL) {
        solar_os_memory_free(parse);
        return;
    }

    shell_complete_path(ctx,
                        token_start,
                        parse->trailing_space ? "" : parse->tokens[current_index],
                        shell_path_completion_dirs_only(effective_command),
                        show_matches);
    solar_os_memory_free(parse);
}

static void shell_script_discard_rest_of_line(FILE *file)
{
    int ch = 0;

    do {
        ch = fgetc(file);
    } while (ch != EOF && ch != '\n');
}

bool solar_os_shell_run_script(solar_os_context_t *ctx,
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
            continue;
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
            continue;
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

esp_err_t solar_os_shell_run_background_script(const char *path)
{
    if (path == NULL || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *probe = fopen(path, "r");
    if (probe == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    fclose(probe);

    solar_os_shell_session_t *session = solar_os_shell_session_create();
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_context_t ctx;
    solar_os_context_init(&ctx, NULL, NULL);
    solar_os_shell_io_t *io = solar_os_shell_session_io(session);
    solar_os_shell_io_init_terminal(io, NULL);
    esp_err_t err = solar_os_shell_session_start(&ctx,
                                                 session,
                                                 io,
                                                 false,
                                                 false);
    if (err == ESP_OK) {
        session->watch_executing = true;
        (void)solar_os_shell_run_script(&ctx, path, path, false);
        session->watch_executing = false;
    }
    solar_os_shell_session_destroy(session);
    return err;
}

static void cmd_sh(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char path[SHELL_PATH_MAX];

    if (argc != 2) {
        if (argc < 2) {
            solar_os_shell_diag_missing(term, "sh", "file", "sh <file>");
        } else {
            solar_os_shell_diag_unexpected(term, "sh", argv[2], "sh <file>");
        }
        return;
    }
    if (!solar_os_shell_resolve_path_for_command(ctx, term, "sh", argv[1], path, sizeof(path))) {
        return;
    }

    solar_os_shell_run_script(ctx, path, argv[1], true);
}

static void cmd_reboot(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "reboot", argv[1], "reboot");
        return;
    }
    solar_os_shell_io_writeln(term, "rebooting");
    solar_os_shell_io_flush(term);
    vTaskDelay(pdMS_TO_TICKS(100));
    solar_os_context_reboot(ctx, "restarting");
}

static void cmd_exit(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = terminal(ctx);

    if (argc != 1) {
        solar_os_shell_diag_unexpected(io, "exit", argv[1], "exit");
        return;
    }
    if (solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT) {
        solar_os_shell_io_writeln(io, "exit: cannot close the display shell");
        return;
    }
    if (solar_os_sessions_shell_count() <= 1U) {
        solar_os_shell_io_writeln(io, "exit: cannot close the last shell");
        return;
    }

    solar_os_context_finish(ctx, 0, NULL);
    shell_session(ctx)->builtin_suppressed_prompt = true;
}

static bool parse_session_id(const char *text, uint8_t *session_id)
{
    if (text == NULL || text[0] == '\0' || session_id == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT8_MAX) {
        return false;
    }

    *session_id = (uint8_t)value;
    return true;
}

static bool parse_port_shell_size(const char *text, uint16_t *cols, uint16_t *rows)
{
    if (text == NULL || cols == NULL || rows == NULL) {
        return false;
    }

    const char *sep = strchr(text, 'x');
    if (sep == NULL) {
        sep = strchr(text, 'X');
    }
    if (sep == NULL || sep == text || sep[1] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed_cols = strtoul(text, &end, 10);
    if (errno != 0 || end != sep) {
        return false;
    }

    errno = 0;
    const unsigned long parsed_rows = strtoul(sep + 1, &end, 10);
    if (errno != 0 || end == sep + 1 || *end != '\0') {
        return false;
    }
    if (parsed_cols < SHELL_PORT_TERM_MIN_COLS ||
        parsed_rows < SHELL_PORT_TERM_MIN_ROWS ||
        parsed_cols > SHELL_PORT_TERM_MAX_COLS ||
        parsed_rows > SHELL_PORT_TERM_MAX_ROWS) {
        return false;
    }

    *cols = (uint16_t)parsed_cols;
    *rows = (uint16_t)parsed_rows;
    return true;
}

static bool parse_port_shell_options(int argc,
                                     char **argv,
                                     int first,
                                     solar_os_port_shell_options_t *options)
{
    if (options == NULL) {
        return false;
    }

    *options = (solar_os_port_shell_options_t){
        .terminal_profile = SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO,
        .charset = SOLAR_OS_SHELL_CHARSET_UTF8,
        .cols = 0,
        .rows = 0,
    };

    for (int i = first; i < argc; i++) {
        const char *term_arg = NULL;
        const char *charset_arg = NULL;
        const char *size_arg = NULL;

        if (strcmp(argv[i], "--term") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            term_arg = argv[++i];
        } else if (strncmp(argv[i], "--term=", 7) == 0) {
            term_arg = argv[i] + 7;
        } else if (strcmp(argv[i], "--charset") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            charset_arg = argv[++i];
        } else if (strncmp(argv[i], "--charset=", 10) == 0) {
            charset_arg = argv[i] + 10;
        } else if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            size_arg = argv[++i];
        } else if (strncmp(argv[i], "--size=", 7) == 0) {
            size_arg = argv[i] + 7;
        } else {
            return false;
        }

        if (term_arg != NULL &&
            !solar_os_shell_parse_terminal_profile(term_arg, &options->terminal_profile)) {
            return false;
        }
        if (charset_arg != NULL &&
            !solar_os_shell_parse_charset(charset_arg, &options->charset)) {
            return false;
        }
        if (size_arg != NULL && !parse_port_shell_size(size_arg, &options->cols, &options->rows)) {
            return false;
        }
    }

    return true;
}

static void cmd_sessions(solar_os_context_t *ctx, int argc, char **argv)
{
    if (argc != 1) {
        solar_os_shell_diag_unexpected(terminal(ctx), "sessions", argv[1], "sessions");
        return;
    }

    const esp_err_t err = solar_os_context_print_session_list(ctx);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(terminal(ctx),
                                 "sessions: unavailable: %s\n",
                                 solar_os_shell_error_text(err));
    }
}

static void session_print_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  session list");
    solar_os_shell_io_writeln(io, "  session create shell <port> [--term auto|vt100|ansi|dumb] [--charset utf8|ascii] [--size COLSxROWS]");
    solar_os_shell_io_writeln(io, "  session create shell <display-target>");
    solar_os_shell_io_writeln(io, "  session create <app> <display-target> [args...]");
    solar_os_shell_io_writeln(io, "  session fg [session-id]");
    solar_os_shell_io_writeln(io, "  session switch [session-id]");
    solar_os_shell_io_writeln(io, "  session close <session-id>");
    solar_os_shell_io_writeln(io, "  session send <session-id> <command> [args...]");
    solar_os_shell_io_writeln(io, "  session focus [display-target]");
}

static void session_request_fg(solar_os_context_t *ctx, uint8_t session_id)
{
    if (solar_os_port_shell_is_app_session_id(session_id)) {
        const bool local =
            solar_os_port_shell_context_owns_app_session(ctx, session_id);
        const esp_err_t err =
            solar_os_port_shell_foreground_app_session(ctx, session_id);
        if (err == ESP_OK) {
            if (local) {
                shell_session(ctx)->builtin_suppressed_prompt = true;
            } else {
                solar_os_shell_io_printf(terminal(ctx),
                                         "foregrounded session %u on its port\n",
                                         (unsigned)session_id);
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(terminal(ctx),
                                     "fg: no such port app session: %u\n",
                                     (unsigned)session_id);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(
                terminal(ctx),
                "fg: the active port application cannot be suspended");
        } else {
            solar_os_shell_io_printf(terminal(ctx),
                                     "fg: failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }
    if (solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        solar_os_shell_io_writeln(terminal(ctx),
                                  "fg: display sessions are only available on the display shell");
        return;
    }

    shell_session(ctx)->builtin_suppressed_prompt = true;
    shell_session(ctx)->prompt_on_resume = true;
    solar_os_context_request_session_fg(ctx, session_id);
}

static void session_request_close(solar_os_context_t *ctx, uint8_t session_id)
{
    const bool closes_calling_port =
        solar_os_port_shell_context_owns_session(ctx, session_id);
    const esp_err_t err =
        solar_os_sessions_close_any(session_id, terminal(ctx));
    if (closes_calling_port && err == ESP_OK) {
        shell_session(ctx)->builtin_suppressed_prompt = true;
    }
}

static void session_create_display_shell(solar_os_context_t *ctx, const char *target_name)
{
    solar_os_shell_io_t *caller_io = terminal(ctx);
    const bool caller_is_port =
        solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT;
    char busy_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    uint8_t session_id = 0;

    const esp_err_t err =
        solar_os_sessions_create_detached_display_shell(target_name,
                                                        &session_id,
                                                        busy_owner,
                                                        sizeof(busy_owner));
    if (err == ESP_OK) {
        if (caller_is_port) {
            solar_os_shell_io_printf(caller_io,
                                     "session %u created: shell on %s\n",
                                     (unsigned)session_id,
                                     target_name);
        } else {
            shell_session(ctx)->builtin_suppressed_prompt = true;
        }
        return;
    }

    if (err == ESP_ERR_INVALID_STATE && busy_owner[0] != '\0') {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s owned by %s\n",
                                 target_name,
                                 busy_owner);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: display target not found: %s\n",
                                 target_name);
    } else {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s\n",
                                 solar_os_shell_error_text(err));
    }
}

static void session_create_display_app(solar_os_context_t *ctx,
                                       int argc,
                                       char **argv)
{
    solar_os_shell_io_t *caller_io = terminal(ctx);
    const solar_os_app_registry_entry_t *app =
        solar_os_app_registry_find(argv[2]);
    if (app == NULL || app->app == NULL) {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: app not found: %s\n",
                                 argv[2]);
        return;
    }
    if ((app->capabilities & SOLAR_OS_APP_CAP_DISPLAY) == 0) {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s is not a display app\n",
                                 app->name);
        return;
    }

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(argv[3], &target)) {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: display target not found: %s\n",
                                 argv[3]);
        return;
    }

    const int app_argc = argc - 3;
    if (app_argc > SOLAR_OS_APP_ARG_MAX) {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: too many app arguments (max %u)\n",
                                 (unsigned)(SOLAR_OS_APP_ARG_MAX - 1));
        return;
    }

    char *app_argv[SOLAR_OS_APP_ARG_MAX] = {0};
    app_argv[0] = argv[2];
    for (int i = 4; i < argc; i++) {
        app_argv[i - 3] = argv[i];
    }
    char *launch_argv[SOLAR_OS_APP_ARG_MAX] = {0};
    shell_app_launch_storage_t launch_storage = {0};
    if (!shell_prepare_app_launch_args(ctx,
                                       app,
                                       app_argc,
                                       app_argv,
                                       launch_argv,
                                       &launch_storage)) {
        return;
    }

    const bool caller_is_port =
        solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT;
    if (!caller_is_port &&
        solar_os_sessions_context_uses_display(ctx, target.name)) {
        const esp_err_t request_err =
            solar_os_context_request_launch_ex(ctx,
                                               app->app,
                                               app_argc,
                                               launch_argv,
                                               SOLAR_OS_LAUNCH_CHILD_RETURN);
        if (request_err == ESP_OK) {
            shell_session(ctx)->builtin_suppressed_prompt = true;
            shell_session(ctx)->prompt_on_resume = true;
            return;
        }
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s\n",
                                 solar_os_shell_error_text(request_err));
        return;
    }

    char busy_owner[SOLAR_OS_APP_OWNER_MAX];
    uint8_t session_id = 0;
    const esp_err_t err =
        solar_os_sessions_create_display_app(app->app,
                                             target.name,
                                             app_argc,
                                             launch_argv,
                                             &session_id,
                                             busy_owner,
                                             sizeof(busy_owner));
    if (err == ESP_OK) {
        if (caller_is_port) {
            solar_os_shell_io_printf(caller_io,
                                     "session %u created: %s on %s\n",
                                     (unsigned)session_id,
                                     app->name,
                                     target.name);
        } else if (solar_os_sessions_foreground_app() == app->app) {
            shell_session(ctx)->builtin_suppressed_prompt = true;
        } else {
            solar_os_shell_io_printf(caller_io,
                                     "session %u created: %s on %s\n",
                                     (unsigned)session_id,
                                     app->name,
                                     target.name);
        }
        return;
    }

    if (err == ESP_ERR_INVALID_STATE && busy_owner[0] != '\0') {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s already running on %s\n",
                                 app->name,
                                 busy_owner);
    } else if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s is not ready\n",
                                 target.name);
    } else {
        solar_os_shell_io_printf(caller_io,
                                 "session create failed: %s\n",
                                 solar_os_shell_error_text(err));
    }
}

static void cmd_session(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = terminal(ctx);

    if (argc < 2) {
        solar_os_shell_diag_missing(io, "session", "subcommand",
                                    "session <list|create|fg|switch|close|send|focus> ...");
        return;
    }

    if (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "ls") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(io, "session list", argv[2], "session list");
            return;
        }
        const esp_err_t err = solar_os_context_print_session_list(ctx);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "session list: unavailable: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "create") == 0) {
        solar_os_port_shell_options_t options;
        if (argc < 4) {
            solar_os_shell_diag_problem(io, "session create", "missing app, port, or display target",
                                        "session create <shell|app> <port|display-target> [options|args...]",
                                        NULL);
            return;
        }

        if (strcmp(argv[2], "shell") != 0) {
            session_create_display_app(ctx, argc, argv);
            return;
        }

        solar_os_display_target_t display_target;
        if (solar_os_display_find_target(argv[3], &display_target)) {
            if (argc != 4) {
                solar_os_shell_diag_unexpected(io, "session create shell", argv[4],
                                               "session create shell <display-target>");
                return;
            }
            session_create_display_shell(ctx, argv[3]);
            return;
        }

        if (!parse_port_shell_options(argc, argv, 4, &options)) {
            solar_os_shell_diag_problem(
                io, "session create shell", "invalid terminal option or value",
                "session create shell <port> [--term auto|vt100|ansi|dumb] [--charset utf8|ascii] [--size COLSxROWS]",
                NULL);
            return;
        }

        uint8_t session_id = 0;
        const esp_err_t err =
            solar_os_port_shell_start_with_options(ctx, argv[3], &options, false, &session_id);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "session %u created: shell on %s term=%s charset=%s\n",
                                     (unsigned)session_id,
                                     argv[3],
                                     solar_os_shell_terminal_profile_name(options.terminal_profile),
                                     solar_os_shell_charset_name(options.charset));
        } else {
            solar_os_shell_io_printf(io,
                                     "session create failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "focus") == 0) {
        if (argc == 2) {
            char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
            if (solar_os_sessions_input_focus(target_name,
                                              sizeof(target_name))) {
                solar_os_shell_io_printf(io,
                                         "session input focus: %s\n",
                                         target_name);
            } else {
                solar_os_shell_io_writeln(io, "session input focus: none");
            }
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(io, "session focus", argv[3],
                                           "session focus [display-target]");
            return;
        }

        const esp_err_t err = solar_os_sessions_focus_display(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "session input focus: %s\n",
                                     argv[2]);
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(io,
                                     "session focus failed: display target not found: %s\n",
                                     argv[2]);
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_printf(io,
                                     "session focus failed: no active session on %s\n",
                                     argv[2]);
        } else {
            solar_os_shell_io_printf(io,
                                     "session focus failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "fg") == 0 || strcmp(argv[1], "foreground") == 0 ||
        strcmp(argv[1], "switch") == 0) {
        uint8_t session_id = 0;
        if (argc == 2 &&
            solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            const esp_err_t err =
                solar_os_port_shell_foreground_last_app(ctx, &session_id);
            if (err == ESP_OK) {
                shell_session(ctx)->builtin_suppressed_prompt = true;
                return;
            }
            solar_os_shell_io_writeln(
                io,
                err == ESP_ERR_NOT_FOUND ?
                    "fg: no suspended application" :
                    "fg: unavailable");
            return;
        }
        if (argc != 3 || !parse_session_id(argv[2], &session_id)) {
            if (argc != 3) {
                solar_os_shell_diag_problem(io, "session fg", "expected one session ID",
                                            "session fg [session-id]", NULL);
            } else {
                solar_os_shell_diag_invalid(io, "session fg", "session ID", argv[2],
                                            "integer from 0 to 255", "session fg [session-id]", false);
            }
            return;
        }
        if (solar_os_port_shell_is_session_id(session_id)) {
            solar_os_shell_io_writeln(io, "session fg: port shells are already attached to their port");
            return;
        }
        session_request_fg(ctx, session_id);
        return;
    }

    if (strcmp(argv[1], "close") == 0) {
        uint8_t session_id = 0;
        if (argc != 3 || !parse_session_id(argv[2], &session_id)) {
            if (argc < 3) {
                solar_os_shell_diag_missing(io, "session close", "session ID",
                                            "session close <session-id>");
            } else if (argc > 3) {
                solar_os_shell_diag_unexpected(io, "session close", argv[3],
                                               "session close <session-id>");
            } else {
                solar_os_shell_diag_invalid(io, "session close", "session ID", argv[2],
                                            "integer from 0 to 255", "session close <session-id>", false);
            }
            return;
        }
        session_request_close(ctx, session_id);
        return;
    }

    if (strcmp(argv[1], "send") == 0) {
        uint8_t session_id = 0;
        if (argc < 4 || !parse_session_id(argv[2], &session_id)) {
            if (argc < 3) {
                solar_os_shell_diag_missing(io, "session send", "session ID",
                                            "session send <session-id> <command> [args...]");
            } else if (!parse_session_id(argv[2], &session_id)) {
                solar_os_shell_diag_invalid(io, "session send", "session ID", argv[2],
                                            "integer from 0 to 255",
                                            "session send <session-id> <command> [args...]", false);
            } else {
                solar_os_shell_diag_missing(io, "session send", "command",
                                            "session send <session-id> <command> [args...]");
            }
            return;
        }

        char command[SHELL_INPUT_MAX] = "";
        for (int i = 3; i < argc; i++) {
            if (!shell_append_token(command, sizeof(command), argv[i])) {
                solar_os_shell_diag_problem(io, "session send",
                                            "command is empty or too long",
                                            "session send <session-id> <command> [args...]", NULL);
                return;
            }
        }

        const esp_err_t err =
            solar_os_sessions_send_command(session_id, command, io);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io, "sent command to session %u\n",
                                     (unsigned)session_id);
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(io, "session send: no such session: %u\n",
                                     (unsigned)session_id);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_printf(io,
                                     "session send: session %u is not a display shell\n",
                                     (unsigned)session_id);
        } else if (err == ESP_ERR_NOT_ALLOWED) {
            solar_os_shell_io_writeln(io,
                                      "session send: cannot send to the calling shell");
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_printf(
                io,
                "session send: session %u is suspended or not at an empty prompt\n",
                (unsigned)session_id);
        } else {
            solar_os_shell_io_printf(io, "session send failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "background") == 0 || strcmp(argv[1], "bg") == 0) {
        solar_os_shell_io_writeln(
            io,
            "session background: use Ctrl+Z on a port shell or Alt+Tab on a display");
        return;
    }

    static const char * const session_subcommands[] = {
        "list", "create", "fg", "foreground", "switch", "close", "send", "focus", "background", "bg",
    };
    solar_os_shell_diag_subcommand(io, "session", argc, argv,
                                   "session <list|create|fg|switch|close|send|focus> ...",
                                   session_subcommands,
                                   sizeof(session_subcommands) / sizeof(session_subcommands[0]));
}

static void cmd_fg(solar_os_context_t *ctx, int argc, char **argv)
{
    uint8_t session_id = 0;

    if (argc == 1 &&
        solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        const esp_err_t err =
            solar_os_port_shell_foreground_last_app(ctx, &session_id);
        if (err == ESP_OK) {
            shell_session(ctx)->builtin_suppressed_prompt = true;
        } else {
            solar_os_shell_io_writeln(
                terminal(ctx),
                err == ESP_ERR_NOT_FOUND ?
                    "fg: no suspended application" :
                    "fg: unavailable");
        }
        return;
    }
    if (argc != 2 || !parse_session_id(argv[1], &session_id)) {
        if (argc < 2) {
            solar_os_shell_diag_missing(terminal(ctx), "fg", "session ID", "fg [session-id]");
        } else if (argc > 2) {
            solar_os_shell_diag_unexpected(terminal(ctx), "fg", argv[2], "fg [session-id]");
        } else {
            solar_os_shell_diag_invalid(terminal(ctx), "fg", "session ID", argv[1],
                                        "integer from 0 to 255", "fg [session-id]", false);
        }
        return;
    }

    if (solar_os_port_shell_is_session_id(session_id)) {
        solar_os_shell_io_writeln(terminal(ctx), "fg: port shells are already attached to their port");
        return;
    }
    session_request_fg(ctx, session_id);
}

static void cmd_close(solar_os_context_t *ctx, int argc, char **argv)
{
    uint8_t session_id = 0;

    if (argc != 2 || !parse_session_id(argv[1], &session_id)) {
        if (argc < 2) {
            solar_os_shell_diag_missing(terminal(ctx), "close", "session ID", "close <session-id>");
        } else if (argc > 2) {
            solar_os_shell_diag_unexpected(terminal(ctx), "close", argv[2], "close <session-id>");
        } else {
            solar_os_shell_diag_invalid(terminal(ctx), "close", "session ID", argv[1],
                                        "integer from 0 to 255", "close <session-id>", false);
        }
        return;
    }

    session_request_close(ctx, session_id);
}

solar_os_shell_session_t *solar_os_shell_session_create(void)
{
    return solar_os_memory_calloc(1,
                                  sizeof(solar_os_shell_session_t),
                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                  "shell.session");
}

void solar_os_shell_session_destroy(solar_os_shell_session_t *session)
{
    if (session != NULL && session != &shell_display_session) {
        solar_os_memory_free(session);
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

void solar_os_shell_session_set_exit_result(solar_os_shell_session_t *session,
                                            int exit_code,
                                            const char *message)
{
    if (session == NULL) {
        return;
    }
    session->last_exit_code = exit_code;
    if (message == NULL || message[0] == '\0') {
        session->exit_message_pending = false;
        session->exit_message[0] = '\0';
        return;
    }
    strlcpy(session->exit_message, message, sizeof(session->exit_message));
    session->exit_message_pending = true;
}

int solar_os_shell_session_last_exit_code(
    const solar_os_shell_session_t *session)
{
    return session != NULL ? session->last_exit_code : 0;
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
        if (!solar_os_shell_resolve_path_for_command(ctx,
                                                     terminal(ctx),
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

static bool shell_prepare_app_launch_args(
    solar_os_context_t *ctx,
    const solar_os_app_registry_entry_t *app,
    int argc,
    char **argv,
    char **launch_argv,
    shell_app_launch_storage_t *storage)
{
    if (ctx == NULL ||
        app == NULL ||
        app->name == NULL ||
        argc < 1 ||
        argv == NULL ||
        launch_argv == NULL ||
        storage == NULL) {
        return false;
    }

    if (argc < app->min_argc) {
        solar_os_shell_diag_problem(terminal(ctx), app->name,
                                    "not enough arguments", app->usage, NULL);
        return false;
    }
    if (app->max_argc != 0 && argc > app->max_argc) {
        solar_os_shell_diag_unexpected(terminal(ctx), app->name,
                                       argv[app->max_argc], app->usage);
        return false;
    }

    for (int i = 0; i < argc; i++) {
        launch_argv[i] = argv[i];
    }

    const int path_arg = solar_os_shell_launch_path_arg(app->name,
                                                        argc,
                                                        argv);
    if (path_arg >= 0) {
        if (!solar_os_shell_resolve_path_for_command(ctx,
                                                     terminal(ctx),
                                                     app->name,
                                                     argv[path_arg],
                                                     storage->path,
                                                     sizeof(storage->path))) {
            return false;
        }
        launch_argv[path_arg] = storage->path;
    } else if (strcmp(app->name, "scp") == 0) {
        return shell_prepare_scp_launch_args(ctx,
                                             argc,
                                             argv,
                                             launch_argv,
                                             storage->scp_paths);
    }

    return true;
}

#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
static bool SHELL_NOINLINE shell_launch_playground_script(
    solar_os_context_t *ctx,
    int argc,
    char **argv);

static bool shell_launch_playground_script(
    solar_os_context_t *ctx,
    int argc,
    char **argv)
{
    solar_os_shell_io_t *io = terminal(ctx);
    if (argc < 3) {
        solar_os_shell_diag_missing(io, "playground run", "app ID",
                                    "playground run <APP-ID> [ARG...]");
        return true;
    }
    const int runtime_argc = argc - 1;
    if (runtime_argc > SOLAR_OS_APP_ARG_MAX) {
        solar_os_shell_io_printf(
            io,
            "playground: too many script arguments (maximum %u)\n",
            (unsigned)(SOLAR_OS_APP_ARG_MAX - 2));
        return true;
    }
    if (shell_session(ctx)->watch_executing) {
        solar_os_shell_io_writeln(
            io, "watch: cannot launch foreground app: playground run");
        return true;
    }
    solar_os_playground_app_info_t script;
    if (!solar_os_playground_find_installed_app(argv[2], &script)) {
        solar_os_shell_io_printf(
            io, "playground: application is not installed: %s\n", argv[2]);
        return true;
    }
    char path[SOLAR_OS_APP_ARG_LEN];
    const esp_err_t path_err =
        solar_os_playground_entry_path(&script, path, sizeof(path));
    if (path_err != ESP_OK) {
        solar_os_shell_io_printf(
            io, "playground: cannot run %s: not installed\n", argv[2]);
        return true;
    }

    const char *runtime_name =
        solar_os_playground_runtime_name(script.runtime);
    const solar_os_app_registry_entry_t *runtime =
        solar_os_app_registry_find(runtime_name);
    if (runtime == NULL || runtime->app == NULL) {
        solar_os_shell_io_printf(
            io, "playground: runtime is unavailable: %s\n", runtime_name);
        return true;
    }
    if (solar_os_shell_io_kind(shell_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_PORT &&
        (runtime->capabilities & SOLAR_OS_APP_CAP_PORT) == 0) {
        solar_os_shell_io_printf(
            io, "%s: display-only app; use the display shell\n", runtime_name);
        return true;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    if (solar_os_app_registry_owner(runtime->app, owner, sizeof(owner))) {
        solar_os_shell_io_printf(io,
                                 "%s: already running on %s\n",
                                 runtime_name,
                                 owner[0] != '\0' ? owner : "another session");
        return true;
    }

    char *launch_argv[SOLAR_OS_APP_ARG_MAX] = {
        (char *)runtime_name,
        path,
    };
    for (int i = 3; i < argc; i++) {
        launch_argv[i - 1] = argv[i];
    }
    const esp_err_t err =
        solar_os_context_request_launch(ctx,
                                        runtime->app,
                                        runtime_argc,
                                        launch_argv);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "playground: launch failed: %s\n",
                                 solar_os_shell_error_text(err));
        return true;
    }
    shell_session(ctx)->prompt_on_resume = true;
    shell_session(ctx)->clear_on_resume =
        (runtime->app->flags & SOLAR_OS_APP_FLAG_RESUMABLE) == 0;
    return false;
}
#endif

static bool SHELL_NOINLINE shell_launch_registered_app(
    solar_os_context_t *ctx,
    const solar_os_app_registry_entry_t *app,
    int argc,
    char **argv,
    const char *source,
    size_t line_number)
{
    solar_os_shell_io_t *io = terminal(ctx);
    char *launch_argv[SHELL_ARG_MAX];
    shell_app_launch_storage_t launch_storage = {0};

    if (shell_session(ctx)->watch_executing) {
        solar_os_shell_io_printf(io,
                                 "watch: cannot launch foreground app: %s\n",
                                 app->name);
        return true;
    }
    if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT &&
        (app->capabilities & SOLAR_OS_APP_CAP_PORT) == 0) {
        solar_os_shell_io_printf(io,
                                 "%s: display-only app; use the display shell\n",
                                 app->name);
        return true;
    }

    char owner[SOLAR_OS_APP_OWNER_MAX];
    if (solar_os_app_registry_owner(app->app, owner, sizeof(owner))) {
        solar_os_shell_io_printf(io,
                                 "%s: already running on %s\n",
                                 app->name,
                                 owner[0] != '\0' ? owner : "another session");
        return true;
    }

    solar_os_shell_diag_set_source(io, source, line_number);
    if (!shell_prepare_app_launch_args(ctx,
                                       app,
                                       argc,
                                       argv,
                                       launch_argv,
                                       &launch_storage)) {
        solar_os_shell_diag_set_source(io, NULL, 0);
        return true;
    }
    solar_os_shell_diag_set_source(io, NULL, 0);

    const esp_err_t err =
        solar_os_context_request_launch(ctx, app->app, argc, launch_argv);
    if (err == ESP_OK) {
        shell_session(ctx)->prompt_on_resume = true;
        shell_session(ctx)->clear_on_resume =
            (app->app->flags & SOLAR_OS_APP_FLAG_RESUMABLE) == 0;
        return false;
    }

    solar_os_shell_diag_set_source(io, source, line_number);
    if (err == ESP_ERR_INVALID_ARG && app->usage != NULL) {
        solar_os_shell_diag_problem(io,
                                    app->name,
                                    "invalid launch arguments",
                                    app->usage,
                                    NULL);
    } else {
        solar_os_shell_diag_esp(io,
                                app->name,
                                err,
                                "application could not start",
                                NULL);
    }
    solar_os_shell_diag_set_source(io, NULL, 0);
    return true;
}

static void SHELL_NOINLINE shell_report_unknown_command(
    solar_os_shell_io_t *io,
    const char *command,
    const char *source,
    size_t line_number)
{
    const char *candidate_names[128];
    size_t candidate_count = 0;
    for (size_t i = 0; i < shell_builtin_command_count &&
                       candidate_count < SHELL_ARRAY_COUNT(candidate_names); i++) {
        candidate_names[candidate_count++] = shell_builtin_commands[i].name;
    }
    for (size_t i = 0; i < solar_os_app_registry_count() &&
                       candidate_count < SHELL_ARRAY_COUNT(candidate_names); i++) {
        const solar_os_app_registry_entry_t *candidate = solar_os_app_registry_get(i);
        if (candidate != NULL && !shell_builtin_command_exists(candidate->name)) {
            candidate_names[candidate_count++] = candidate->name;
        }
    }
    const char *suggestion =
        solar_os_shell_suggest(command, candidate_names, candidate_count);
    char alias_suggestion[SHELL_INPUT_MAX];
    if (suggestion == NULL) {
        suggestion = shell_alias_suggestion(command,
                                            alias_suggestion,
                                            sizeof(alias_suggestion));
    }
    solar_os_shell_diag_set_source(io, source, line_number);
    solar_os_shell_diag_unknown(io, "shell", "command", command, suggestion, NULL);
    solar_os_shell_diag_set_source(io, NULL, 0);
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

    solar_os_shell_io_t *io = terminal(ctx);
    if (line == NULL || strlen(line) >= sizeof(command)) {
        if (source != NULL) {
            solar_os_shell_io_printf(io, "%s:%u: ", source, (unsigned)line_number);
        }
        solar_os_shell_diag_problem(io,
                                    "shell",
                                    "command line is too long; maximum is 191 characters",
                                    NULL,
                                    NULL);
        return true;
    }

    strlcpy(command, line, sizeof(command));
    const solar_os_shell_parse_result_t parsed =
        solar_os_shell_tokenize(command, argv, SHELL_ARG_MAX);
    if (parsed.error != SOLAR_OS_SHELL_PARSE_OK) {
        char problem[128];
        if (parsed.error == SOLAR_OS_SHELL_PARSE_TOO_MANY_ARGUMENTS) {
            snprintf(problem, sizeof(problem), "too many arguments; maximum is %u",
                     (unsigned)SHELL_ARG_MAX);
        } else if (parsed.error == SOLAR_OS_SHELL_PARSE_UNSUPPORTED_OPERATOR) {
            snprintf(problem,
                     sizeof(problem),
                     "operator '%s' is not supported; run commands separately",
                     parsed.operator_text);
        } else {
            snprintf(problem,
                     sizeof(problem),
                     "%s at column %u",
                     solar_os_shell_parse_error_text(parsed.error),
                     (unsigned)(parsed.column + 1U));
        }
        if (source != NULL) {
            solar_os_shell_io_printf(io, "%s:%u: ", source, (unsigned)line_number);
        }
        solar_os_shell_diag_problem(io, "shell", problem, NULL, NULL);
        return true;
    }
    const int argc = parsed.argc;
    if (argc == 0) {
        return true;
    }

    if (add_history) {
        shell_history_add(ctx, line);
    }

    for (size_t i = 0; i < shell_builtin_command_count; i++) {
        if (strcmp(argv[0], shell_builtin_commands[i].name) == 0) {
            solar_os_shell_diag_set_source(io, source, line_number);
            shell_builtin_commands[i].handler(ctx, argc, argv);
            solar_os_shell_diag_set_source(io, NULL, 0);
            const bool should_prompt = !shell_session(ctx)->builtin_suppressed_prompt;
            return should_prompt;
        }
    }

    if (solar_os_shell_path_is_script(argv[0])) {
        char *script_argv[SHELL_ARG_MAX + 1] = {0};
        script_argv[0] = "sh";
        for (int i = 0; i < argc; i++) {
            script_argv[i + 1] = argv[i];
        }
        solar_os_shell_diag_set_source(io, source, line_number);
        cmd_sh(ctx, argc + 1, script_argv);
        solar_os_shell_diag_set_source(io, NULL, 0);
        return !shell_session(ctx)->builtin_suppressed_prompt;
    }

#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
    if (argc >= 2 &&
        strcmp(argv[0], "playground") == 0 &&
        strcmp(argv[1], "run") == 0) {
        return shell_launch_playground_script(ctx, argc, argv);
    }
#endif

    const solar_os_app_registry_entry_t *app = solar_os_app_registry_find(argv[0]);
    if (app != NULL) {
        return shell_launch_registered_app(ctx,
                                           app,
                                           argc,
                                           argv,
                                           source,
                                           line_number);
    }

    bool alias_matched = false;
    const bool alias_should_prompt =
        shell_try_alias(ctx, argc, argv, source, line_number, &alias_matched);
    if (alias_matched) {
        return alias_should_prompt;
    }

    shell_report_unknown_command(io, argv[0], source, line_number);
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
    case 0x16U:
        shell_paste_clipboard(ctx);
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

    if (shell_startup_attempted) {
        return true;
    }
    shell_startup_attempted = true;

    const solar_os_shell_startup_source_t source = solar_os_shell_startup_source();
    const bool source_mounted = source == SOLAR_OS_SHELL_STARTUP_SD ?
        solar_os_storage_sd_is_mounted() :
        solar_os_storage_flash_is_mounted();
    if (!source_mounted || solar_os_shell_startup_path(path, sizeof(path)) != ESP_OK) {
        return true;
    }

    return solar_os_shell_run_script(ctx, path, path, false);
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
    session->prompt_on_resume = false;
    session->clear_on_resume = false;
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

esp_err_t solar_os_shell_session_submit_command(solar_os_context_t *ctx,
                                                solar_os_shell_session_t *session,
                                                const char *command)
{
    if (ctx == NULL || session == NULL || command == NULL || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(command) >= sizeof(session->input)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (session->input_len != 0 || session->watch_active ||
        session->log_follow_active || session->watch_executing) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_context_set_shell_session(ctx, session);
    solar_os_context_set_shell_io(ctx, &session->io);
    for (const char *p = command; *p != '\0'; p++) {
        shell_handle_char(ctx, *p);
    }
    shell_handle_char(ctx, '\r');
    return ESP_OK;
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

void solar_os_shell_session_prepare_foreground_launch(solar_os_context_t *ctx,
                                                      bool clear_on_resume)
{
    if (ctx == NULL || shell_session(ctx) == NULL) {
        return;
    }

    shell_session(ctx)->builtin_suppressed_prompt = true;
    shell_session(ctx)->prompt_on_resume = true;
    shell_session(ctx)->clear_on_resume = clear_on_resume;
}

static esp_err_t shell_start(solar_os_context_t *ctx)
{
    const bool preserve_terminal = solar_os_context_take_terminal_preserve(ctx);
    solar_os_shell_session_t *session = shell_session(ctx);
    solar_os_shell_io_t *io = solar_os_shell_session_io(session);

    solar_os_shell_io_init_terminal(io, solar_os_context_terminal(ctx));
    return solar_os_shell_session_start(ctx, session, io, preserve_terminal, true);
}

static bool shell_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    return solar_os_shell_session_event(ctx, shell_session(ctx), event);
}

static void shell_resume(solar_os_context_t *ctx)
{
    solar_os_shell_session_t *session = shell_session(ctx);
    solar_os_shell_io_t *io = solar_os_shell_session_io(session);
    solar_os_context_set_shell_session(ctx, session);
    solar_os_context_set_shell_io(ctx, io);
    const bool preserve_terminal = solar_os_context_take_terminal_preserve(ctx);
    if (session->clear_on_resume && !preserve_terminal) {
        solar_os_shell_io_clear(io);
    } else if (preserve_terminal &&
               solar_os_shell_io_cursor_col(io) != 0) {
        solar_os_shell_io_newline(io);
    }
    if (session->prompt_on_resume) {
        shell_prompt(ctx);
    }
}

static void shell_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;

    if (buffer != NULL && buffer_len > 0) {
        strlcpy(buffer, "shell", buffer_len);
    }
}

static const solar_os_app_t shell_app = {
    .name = "shell",
    .summary = "SolarOS command shell",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = shell_start,
    .resume = shell_resume,
    .stop = NULL,
    .event = shell_event,
    .title = shell_title,
};

const solar_os_app_t *solar_os_shell_app(void)
{
    return &shell_app;
}
