#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"

static const char * const display_commands[] = {"list", "test", "mode"};
#if SOLAR_OS_PACKAGE_SERVICE_OTA
static const char * const ota_commands[] = {"status", "check", "upgrade", "url", "flavor", "boot"};
#endif
static const char * const job_commands[] = {"status", "start", "stop"};
static const char * const setterm_commands[] = {
    "--display", "orientation", "font", "textsize", "palette", "foreground", "background", "statusbar",
    "brightness", "backlight",
    "profile", "charset", "keyboard", "keymap", "powerkey", "key", "keyrate", "typerate",
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    "ble",
#endif
    "repeat", "timezone", "startup", "otaurl", "ota",
};
static const char * const stream_commands[] = {"list", "status"};
static const char * const log_commands[] = {"status", "show", "follow", "clear", "level", "sink"};
static const char * const port_commands[] = {"list", "status"};
static const char * const xfer_commands[] = {"protocols", "send", "recv"};
static const char * const xfer_options[] = {
    "--raw", "--zmodem", "--kermit", "--protocol", "-d", "--delay-ms",
    "--idle-ms", "--append", "--replace",
};
#if SOLAR_OS_PACKAGE_SERVICE_SSH
static const char * const sshkey_commands[] = {"status", "gen", "pub", "rm"};
#endif
#include "solar_os_shell_io.h"
#include "solar_os_shell_tui_apps.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_app_registry.h"
#include "solar_os_board_caps.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble_keyboard.h"
#endif
#include "solar_os_config.h"
#include "solar_os_display.h"
#include "solar_os_fonts.h"
#include "solar_os_identity.h"
#include "solar_os_input.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#if SOLAR_OS_PACKAGE_SERVICE_OTA
#include "solar_os_ota.h"
#endif
#include "solar_os_port.h"
#include "solar_os_port_shell.h"
#include "solar_os_power.h"
#include "solar_os_sessions.h"
#include "solar_os_shell.h"
#if SOLAR_OS_PACKAGE_SERVICE_SSH
#include "solar_os_ssh_keys.h"
#endif
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"
#include "solar_os_keys.h"
#include "solar_os_terminal.h"
#include "solar_os_time.h"
#include "solar_os_transfer.h"
#if SOLAR_OS_PACKAGE_SERVICE_OTA
#include "solar_os_wifi.h"
#endif

#define XFER_DELAY_MAX_MS 60000U
#define XFER_IDLE_MAX_MS 86400000U
#define PORT_SHELL_TERM_MIN_COLS 20U
#define PORT_SHELL_TERM_MIN_ROWS 8U
#define PORT_SHELL_TERM_MAX_COLS 300U
#define PORT_SHELL_TERM_MAX_ROWS 120U
#define PORT_LIST_MAX SOLAR_OS_PORT_MAX
#define LOG_SHOW_DEFAULT 40
#define OTA_PROGRESS_BAR_WIDTH 24
#define OTA_PROGRESS_STEP_BYTES (64U * 1024U)
#define OTA_UPGRADE_TASK_STACK 16384
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(OTA_UPGRADE_TASK_STACK);
#define OTA_UPGRADE_WAIT_MS 100U

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static solar_os_terminal_t *display_terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_display_terminal(ctx);
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
    if (parsed_cols < PORT_SHELL_TERM_MIN_COLS ||
        parsed_rows < PORT_SHELL_TERM_MIN_ROWS ||
        parsed_cols > PORT_SHELL_TERM_MAX_COLS ||
        parsed_rows > PORT_SHELL_TERM_MAX_ROWS) {
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

#if SOLAR_OS_PACKAGE_SERVICE_OTA
static void ota_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  ota status");
    solar_os_shell_io_writeln(term, "  ota check");
    solar_os_shell_io_writeln(term, "  ota upgrade");
    solar_os_shell_io_writeln(term, "  ota url [url]");
    solar_os_shell_io_writeln(term, "  ota flavor [flavor]");
    solar_os_shell_io_writeln(term, "  ota boot 0|1");
}
#endif

static void display_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  display [list]");
    solar_os_shell_io_writeln(term, "  display test <target>");
    solar_os_shell_io_writeln(term, "  display mode <target> [mode]");
}

static void display_print_targets(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_display_target_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "no display targets");
        return;
    }

    solar_os_shell_io_writeln(term, "TARGET   SOURCE    DRIVER     SIZE      ROLE     READY BRIGHT OWNER");
    for (size_t i = 0; i < count; i++) {
        solar_os_display_target_t target;
        if (!solar_os_display_get_target(i, &target)) {
            continue;
        }

        char size[16];
        snprintf(size,
                 sizeof(size),
                 "%ux%u",
                 (unsigned)target.width,
                 (unsigned)target.height);
        solar_os_shell_io_printf(term,
                                 "%-8s %-9s %-10s %-9s %-8s %-5s %-6s %s\n",
                                 target.name,
                                 target.source,
                                 target.driver,
                                 size,
                                 target.role[0] != '\0' ? target.role : "-",
                                 target.ready ? "yes" : "no",
                                 target.brightness_supported ? "yes" : "no",
                                 target.owner[0] != '\0' ? target.owner : "-");
    }
}

static void display_draw_test_pattern(u8g2_t *u8g2, const char *name)
{
    const u8g2_uint_t width = u8g2_GetDisplayWidth(u8g2);
    const u8g2_uint_t height = u8g2_GetDisplayHeight(u8g2);

    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawFrame(u8g2, 0, 0, width, height);
    if (width > 1 && height > 1) {
        u8g2_DrawVLine(u8g2, width / 4, 1, height - 2);
        u8g2_DrawVLine(u8g2, (width * 3) / 4, 1, height - 2);
    }
    for (u8g2_uint_t y = 6; y + 6 < height; y += 8) {
        u8g2_DrawHLine(u8g2, 2, y, width > 4 ? width - 4 : width);
    }

    u8g2_SetFont(u8g2, u8g2_font_solar_os_default_r_12_tf);
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFontPosBaseline(u8g2);
    u8g2_DrawBox(u8g2, 3, 14, width > 6 ? width - 6 : width, 20);
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawStr(u8g2, 6, 24, "SolarOS");
    u8g2_DrawStr(u8g2, 6, 32, name != NULL ? name : "display");
    u8g2_SetDrawColor(u8g2, 1);
    solar_os_display_present(u8g2, SOLAR_OS_DISPLAY_PRESENT_TEXT);
}

static void display_cmd_test(solar_os_shell_io_t *term, int argc, char **argv)
{
    static const char owner[] = "shell:display-test";

    if (argc != 3) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "display test", "<target>",
                                        "display test <target>");
        } else {
            solar_os_shell_diag_unexpected(term, "display test", argv[3],
                                           "display test <target>");
        }
        return;
    }

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(argv[2], &target)) {
        solar_os_shell_io_printf(term, "display test: %s not found\n", argv[2]);
        return;
    }
    if (!target.ready || target.u8g2 == NULL) {
        solar_os_shell_io_printf(term, "display test: %s is not drawable\n", argv[2]);
        return;
    }

    char busy_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    const esp_err_t claim_err =
        solar_os_display_claim(target.name, owner, busy_owner, sizeof(busy_owner));
    if (claim_err == ESP_ERR_INVALID_STATE && busy_owner[0] != '\0') {
        solar_os_shell_io_printf(term,
                                 "display test: %s owned by %s\n",
                                 target.name,
                                 busy_owner);
        return;
    }
    if (claim_err != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "display test: %s claim failed: %s\n",
                                 target.name,
                                 solar_os_shell_error_text(claim_err));
        return;
    }

    display_draw_test_pattern(target.u8g2, target.name);
    (void)solar_os_display_release(target.name, owner);
    solar_os_shell_io_printf(term, "display test: drew %s\n", target.name);
}

static void display_cmd_mode(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "display mode", "<target>",
                                        "display mode <target> [mode]");
        } else {
            solar_os_shell_diag_unexpected(term, "display mode", argv[4],
                                           "display mode <target> [mode]");
        }
        return;
    }

    const char *current = NULL;
    const char *values = NULL;
    esp_err_t err = solar_os_display_get_controller_mode(argv[2], &current, &values);
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "display mode: %s not found\n", argv[2]);
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_printf(term, "display mode: %s unsupported\n", argv[2]);
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "display mode: %s unavailable: %s\n",
                                 argv[2],
                                 solar_os_shell_error_text(err));
        return;
    }

    if (argc == 3) {
        solar_os_shell_io_printf(term, "mode: %s\n", current != NULL ? current : "unknown");
        solar_os_shell_io_printf(term, "values: %s\n", values != NULL ? values : "-");
        return;
    }

    err = solar_os_display_set_controller_mode(argv[2], argv[3]);
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "display mode: unknown mode %s\n", argv[3]);
        solar_os_shell_io_printf(term, "values: %s\n", values != NULL ? values : "-");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "display mode: %s failed: %s\n",
                                 argv[3],
                                 solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term, "display mode: %s -> %s\n", argv[2], argv[3]);
}

void solar_os_shell_cmd_display(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        display_print_targets(term);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        display_cmd_test(term, argc, argv);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "mode") == 0) {
        display_cmd_mode(term, argc, argv);
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "display",
                                   argc,
                                   argv,
                                   "display list|test|mode",
                                   display_commands,
                                   sizeof(display_commands) / sizeof(display_commands[0]));
}

#if SOLAR_OS_PACKAGE_SERVICE_OTA
static void ota_print_partition(solar_os_shell_io_t *term,
                                const char *role,
                                const solar_os_ota_partition_t *partition)
{
    if (partition == NULL || !partition->valid) {
        solar_os_shell_io_printf(term, "%s: unavailable\n", role);
        return;
    }

    char size[16];
    format_bytes(partition->size, size, sizeof(size));

    solar_os_shell_io_printf(term,
                             "%s: %s",
                             role,
                             partition->label[0] != '\0' ? partition->label : "?");
    if (partition->slot >= 0) {
        solar_os_shell_io_printf(term, " (slot %d)", partition->slot);
    }
    solar_os_shell_io_printf(term,
                             " addr 0x%06" PRIx32 " size %s state %s",
                             partition->address,
                             size,
                             partition->state[0] != '\0' ? partition->state : "unknown");
    if (partition->version[0] != '\0') {
        solar_os_shell_io_printf(term, " version %s", partition->version);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void ota_print_status(solar_os_shell_io_t *term)
{
    solar_os_ota_status_t status;
    const esp_err_t err = solar_os_ota_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ota: status failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term, "SolarOS: %s\n", SOLAR_OS_VERSION);
    solar_os_shell_io_printf(term, "Compiled flavor: %s\n", status.compiled_flavor);
    solar_os_shell_io_printf(term, "OTA flavor: %s\n", status.target_flavor);
    solar_os_shell_io_printf(term, "URL: %s\n", status.url);
    solar_os_shell_io_printf(term,
                             "OTA partitions: %u\n",
                             (unsigned)status.ota_partition_count);
    ota_print_partition(term, "Running", &status.running);
    ota_print_partition(term, "Boot", &status.boot);
    ota_print_partition(term, "Next update", &status.next_update);
}

static bool ota_wifi_ready(solar_os_shell_io_t *term)
{
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (wifi.started && wifi.connected && wifi.has_ip) {
        return true;
    }

    solar_os_shell_io_writeln(term, "ota: wifi not connected");
    return false;
}

static void ota_print_check_result(solar_os_shell_io_t *term,
                                   const solar_os_ota_check_result_t *result)
{
    if (result == NULL) {
        return;
    }

    solar_os_shell_io_printf(term,
                             "current: %s %s\n",
                             result->current_version,
                             result->compiled_flavor);
    solar_os_shell_io_printf(term,
                             "available: %s %s\n",
                             result->available_version,
                             result->target_flavor);
    solar_os_shell_io_printf(term, "board: %s\n", result->board_id);
    solar_os_shell_io_printf(term, "index URL: %s\n", result->index_url);
    solar_os_shell_io_printf(term,
                             "index signature: %s\n",
                             result->index_signature_verified ? "verified" : "not verified");
    if (result->index_sig_url[0] != '\0') {
        solar_os_shell_io_printf(term, "index sig URL: %s\n", result->index_sig_url);
    }
    solar_os_shell_io_printf(term, "manifest URL: %s\n", result->manifest_url);
    solar_os_shell_io_printf(term, "firmware URL: %s\n", result->firmware_url);
    if (result->image_size_known) {
        char size[16];
        format_bytes(result->image_size, size, sizeof(size));
        solar_os_shell_io_printf(term, "image size: %s\n", size);
    }
    if (result->image_sha256[0] != '\0') {
        solar_os_shell_io_printf(term, "sha256: %s\n", result->image_sha256);
    }
    solar_os_shell_io_printf(term,
                             "update: %s\n",
                             result->update_available ? "available" : "not needed");
}

static bool ota_print_available_flavors(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_ota_available_flavor_count();
    bool printed = false;

    for (size_t i = 0U; i < count; i++) {
        char flavor[SOLAR_OS_OTA_FLAVOR_MAX];
        if (!solar_os_ota_get_available_flavor(i, flavor, sizeof(flavor))) {
            continue;
        }
        if (!printed) {
            solar_os_shell_io_write(term,
                                    "available flavors from latest version: ");
        } else {
            solar_os_shell_io_write(term, ", ");
        }
        solar_os_shell_io_write(term, flavor);
        printed = true;
    }

    if (printed) {
        solar_os_shell_io_put_char(term, '\n');
    }
    return printed;
}

typedef struct {
    solar_os_shell_io_t *term;
    size_t row;
    bool row_valid;
    solar_os_ota_progress_stage_t last_stage;
    uint8_t last_percent;
    uint32_t next_bytes;
    bool last_known;
} ota_shell_progress_t;

typedef struct {
    ota_shell_progress_t progress;
    esp_err_t result;
    volatile bool done;
} ota_upgrade_worker_t;

static const char *ota_stage_name(solar_os_ota_progress_stage_t stage)
{
    switch (stage) {
    case SOLAR_OS_OTA_PROGRESS_CONNECTING:
        return "connecting";
    case SOLAR_OS_OTA_PROGRESS_IMAGE:
        return "image";
    case SOLAR_OS_OTA_PROGRESS_WRITING:
        return "writing";
    case SOLAR_OS_OTA_PROGRESS_VERIFYING:
        return "verifying";
    case SOLAR_OS_OTA_PROGRESS_DONE:
        return "done";
    default:
        return "ota";
    }
}

static void ota_render_progress_bar(solar_os_shell_io_t *term,
                                    uint8_t percent,
                                    uint32_t read,
                                    uint32_t total,
                                    bool total_known)
{
    char read_text[16];
    char total_text[16];
    const uint8_t filled = (uint8_t)((percent * OTA_PROGRESS_BAR_WIDTH) / 100U);

    solar_os_shell_io_put_char(term, '[');
    for (uint8_t i = 0; i < OTA_PROGRESS_BAR_WIDTH; i++) {
        solar_os_shell_io_put_char(term, i < filled ? '#' : '-');
    }
    solar_os_shell_io_printf(term, "] %3u%% ", (unsigned)percent);

    format_bytes(read, read_text, sizeof(read_text));
    if (total_known) {
        format_bytes(total, total_text, sizeof(total_text));
        solar_os_shell_io_printf(term, "%s/%s", read_text, total_text);
    } else {
        solar_os_shell_io_printf(term, "%s", read_text);
    }
}

static void ota_shell_progress_cb(const solar_os_ota_progress_t *progress, void *user)
{
    ota_shell_progress_t *state = (ota_shell_progress_t *)user;
    if (progress == NULL || state == NULL || state->term == NULL) {
        return;
    }

    uint8_t percent = 0;
    if (progress->image_size_known && progress->image_size > 0) {
        uint32_t calculated =
            (uint32_t)(((uint64_t)progress->bytes_read * 100ULL) / progress->image_size);
        if (calculated > 100U) {
            calculated = 100U;
        }
        percent = (uint8_t)calculated;
    }
    if (progress->stage == SOLAR_OS_OTA_PROGRESS_DONE) {
        percent = 100;
    }

    const bool stage_changed = progress->stage != state->last_stage;
    const bool percent_changed = progress->image_size_known &&
        (!state->last_known || percent != state->last_percent);
    const bool bytes_changed = !progress->image_size_known &&
        progress->bytes_read >= state->next_bytes;
    if (!stage_changed && !percent_changed && !bytes_changed) {
        return;
    }

    if (!state->row_valid) {
        state->row = solar_os_shell_io_cursor_row(state->term);
        state->row_valid = true;
    }

    solar_os_shell_io_set_cursor(state->term, state->row, 0);
    solar_os_shell_io_clear_line_from(state->term, state->row, 0);
    solar_os_shell_io_printf(state->term, "ota: %-10s ", ota_stage_name(progress->stage));
    ota_render_progress_bar(state->term,
                            percent,
                            progress->bytes_read,
                            progress->image_size,
                            progress->image_size_known);
    if (progress->stage == SOLAR_OS_OTA_PROGRESS_IMAGE && progress->version[0] != '\0') {
        solar_os_shell_io_printf(state->term, " v%s", progress->version);
    }
    solar_os_shell_io_flush(state->term);

    state->last_stage = progress->stage;
    state->last_percent = percent;
    state->last_known = progress->image_size_known;
    if (!progress->image_size_known) {
        state->next_bytes = progress->bytes_read + OTA_PROGRESS_STEP_BYTES;
    }
}

static void ota_upgrade_task(void *arg)
{
    ota_upgrade_worker_t *worker = (ota_upgrade_worker_t *)arg;
    if (worker != NULL) {
        worker->result = solar_os_ota_upgrade(ota_shell_progress_cb, &worker->progress);
        SOLAR_OS_LOGI("solar_os_shell",
                      "OTA upgrade task stopped stack_min_free=%u bytes",
                      (unsigned)uxTaskGetStackHighWaterMark(NULL));
        worker->done = true;
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t ota_run_upgrade_worker(ota_shell_progress_t *progress)
{
    if (progress == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_upgrade_worker_t worker = {
        .progress = *progress,
        .result = ESP_FAIL,
        .done = false,
    };

    TaskHandle_t task = NULL;
    if (solar_os_task_create_pinned_internal(ota_upgrade_task,
                                             "ota_upgrade",
                                             OTA_UPGRADE_TASK_STACK,
                                             &worker,
                                             tskIDLE_PRIORITY + 2,
                                             &task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        SOLAR_OS_LOGW("solar_os_shell",
                      "OTA upgrade task create failed stack=%u internal_free=%u "
                      "internal_largest=%u",
                      (unsigned)OTA_UPGRADE_TASK_STACK,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(OTA_UPGRADE_WAIT_MS);
    if (wait_ticks == 0) {
        wait_ticks = 1;
    }
    while (!worker.done) {
        vTaskDelay(wait_ticks);
    }

    *progress = worker.progress;
    return worker.result;
}

static bool ota_parse_slot(const char *text, uint8_t *slot)
{
    if (text == NULL || slot == NULL) {
        return false;
    }

    size_t parsed = 0;
    if (!parse_size_arg(text, 0, 1, &parsed)) {
        return false;
    }

    *slot = (uint8_t)parsed;
    return true;
}

void solar_os_shell_cmd_ota(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        solar_os_shell_diag_missing(term, "ota", "<subcommand>",
                                    "ota status|check|upgrade|url|flavor|boot");
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "ota status", argv[2], "ota status");
            return;
        }
        ota_print_status(term);
        return;
    }

    if (strcmp(argv[1], "check") == 0) {
        solar_os_ota_check_result_t result;

        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "ota check", argv[2], "ota check");
            return;
        }
        if (!ota_wifi_ready(term)) {
            return;
        }

        memset(&result, 0, sizeof(result));
        result.status_code = -1;
        solar_os_shell_io_writeln(term, "ota: checking");
        solar_os_shell_io_flush(term);
        const esp_err_t err = solar_os_ota_check(&result);
        if (err == ESP_OK) {
            ota_print_check_result(term, &result);
        } else if (err == ESP_ERR_NOT_FOUND &&
                   solar_os_ota_available_flavors_checked() &&
                   solar_os_ota_available_flavor_count() > 0U) {
            solar_os_shell_io_printf(term,
                                     "ota: no release for %s/%s\n",
                                     result.board_id,
                                     result.target_flavor);
            ota_print_available_flavors(term);
        } else {
            solar_os_shell_io_printf(term,
                                     "ota: check failed: %s",
                                     solar_os_shell_error_text(err));
            if (result.status_code > 0) {
                solar_os_shell_io_printf(term, " HTTP %d", result.status_code);
            }
            solar_os_shell_io_put_char(term, '\n');
        }
        return;
    }

    if (strcmp(argv[1], "upgrade") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "ota upgrade", argv[2], "ota upgrade");
            return;
        }
        if (!ota_wifi_ready(term)) {
            return;
        }

        solar_os_shell_io_writeln(term, "ota: resolving artifact");
        ota_shell_progress_t progress = {
            .term = term,
            .last_stage = SOLAR_OS_OTA_PROGRESS_CONNECTING,
            .last_percent = 255,
        };
        const esp_err_t err = ota_run_upgrade_worker(&progress);
        if (progress.row_valid) {
            const size_t rows = solar_os_shell_io_rows(term);
            solar_os_shell_io_set_cursor(term,
                                         progress.row + 1 < rows ? progress.row + 1 : progress.row,
                                         0);
        }
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "ota: upgrade complete; rebooting");
            solar_os_shell_io_flush(term);
            vTaskDelay(pdMS_TO_TICKS(200));
            solar_os_context_reboot(ctx, "installing update");
        } else {
            solar_os_shell_io_printf(term,
                                     "ota: upgrade failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "url") == 0) {
        if (argc == 2) {
            char url[SOLAR_OS_OTA_URL_MAX];
            solar_os_ota_get_url(url, sizeof(url));
            solar_os_shell_io_printf(term, "ota url: %s\n", url);
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "ota url", argv[3], "ota url [url]");
            return;
        }

        const esp_err_t err = solar_os_ota_set_url(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "ota url: %s\n", argv[2]);
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_printf(term, "ota url: invalid URL: %s\n", argv[2]);
        } else {
            solar_os_shell_io_printf(term, "ota url: save failed: %s\n", solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "flavor") == 0) {
        if (argc == 2) {
            solar_os_ota_status_t status;
            char index_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
            const esp_err_t err = solar_os_ota_get_status(&status);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term,
                                         "ota flavor: status failed: %s\n",
                                         solar_os_shell_error_text(err));
                return;
            }
            solar_os_shell_io_printf(term, "compiled: %s\n", status.compiled_flavor);
            solar_os_shell_io_printf(term, "target: %s\n", status.target_flavor);
            if (solar_os_ota_get_index_url(index_url, sizeof(index_url)) == ESP_OK) {
                solar_os_shell_io_printf(term, "index: %s\n", index_url);
            }
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "ota flavor", argv[3],
                                           "ota flavor [flavor]");
            return;
        }

        const esp_err_t err = solar_os_ota_set_flavor(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "ota flavor: %s\n", argv[2]);
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_printf(term, "ota flavor: invalid value: %s\n", argv[2]);
            solar_os_shell_io_writeln(term, "values: letters, numbers, dot, underscore, dash");
        } else {
            solar_os_shell_io_printf(term,
                                     "ota flavor: save failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "boot") == 0) {
        uint8_t slot = 0;
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "ota boot", "<slot>", "ota boot 0|1");
            return;
        }
        if (argc > 3) {
            solar_os_shell_diag_unexpected(term, "ota boot", argv[3], "ota boot 0|1");
            return;
        }
        if (!ota_parse_slot(argv[2], &slot)) {
            solar_os_shell_diag_invalid(term, "ota boot", "slot", argv[2], "0 or 1",
                                        "ota boot 0|1", false);
            return;
        }

        const esp_err_t err = solar_os_ota_set_boot_slot(slot);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "ota: boot slot set to ota_%u; rebooting\n", slot);
            solar_os_shell_io_flush(term);
            vTaskDelay(pdMS_TO_TICKS(100));
            solar_os_context_reboot(ctx, "switching slot");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "ota: slot not found: %u\n", slot);
        } else if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            solar_os_shell_io_printf(term, "ota: slot ota_%u has no valid image\n", slot);
        } else {
            solar_os_shell_io_printf(term,
                                     "ota: boot slot failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "ota",
                                   argc,
                                   argv,
                                   "ota status|check|upgrade|url|flavor|boot",
                                   ota_commands,
                                   sizeof(ota_commands) / sizeof(ota_commands[0]));
}
#endif

void solar_os_shell_cmd_apps(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "apps", argv[1], "apps");
        return;
    }

    for (size_t i = 0; i < solar_os_app_registry_count(); i++) {
        const solar_os_app_registry_entry_t *app = solar_os_app_registry_get(i);
        if (app == NULL || app->name == NULL) {
            continue;
        }
        solar_os_shell_io_write_bold(term, app->name);
        solar_os_shell_io_printf(term, " - %s\n", app->summary != NULL ? app->summary : "application");
    }
}

void solar_os_shell_cmd_identity(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        char user[SOLAR_OS_IDENTITY_USER_MAX];
        char hostname[SOLAR_OS_IDENTITY_HOSTNAME_MAX];
        solar_os_identity_get_user(user, sizeof(user));
        solar_os_identity_get_hostname(hostname, sizeof(hostname));
        solar_os_shell_io_printf(term, "User: %s\n", user);
        solar_os_shell_io_printf(term, "Hostname: %s\n", hostname);
        return;
    }

    if (argc == 3 && strcmp(argv[1], "user") == 0) {
        const esp_err_t err = solar_os_identity_set_user(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "identity user set: %s\n", argv[2]);
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(
                term,
                "identity: use 1-31 letters, numbers, '.', '-' or '_'");
        } else {
            solar_os_shell_io_printf(term,
                                     "identity user failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    if (argc == 3 && strcmp(argv[1], "hostname") == 0) {
        const esp_err_t err = solar_os_identity_set_hostname(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(
                term,
                "identity hostname set: %s; reboot to update Wi-Fi\n",
                argv[2]);
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(
                term,
                "identity: use 1-31 letters, numbers, '.', '-' or '_'");
        } else {
            solar_os_shell_io_printf(term,
                                     "identity hostname failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  identity [status]");
    solar_os_shell_io_writeln(term, "  identity user <name>");
    solar_os_shell_io_writeln(term, "  identity hostname <name>");
}

static void job_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  jobs");
    solar_os_shell_io_writeln(term, "  job status [name]");
    solar_os_shell_io_writeln(term, "  job start <name> [args...]");
    solar_os_shell_io_writeln(term, "  job stop <name>");
}

static void job_print_resource(solar_os_shell_io_t *term,
                               const solar_os_job_resource_t *resource)
{
    if (term == NULL || resource == NULL || resource->type == SOLAR_OS_JOB_RESOURCE_NONE) {
        return;
    }

    solar_os_shell_io_printf(term,
                             "  - %-6s %s%s%s\n",
                             solar_os_job_resource_type_name(resource->type),
                             resource->name,
                             resource->detail[0] != '\0' ? " " : "",
                             resource->detail);
}

static void job_print_status(solar_os_shell_io_t *term,
                             const solar_os_job_status_t *status,
                             bool detail)
{
    if (status == NULL) {
        return;
    }

    char stack_bytes[12];
    if (status->worker_stack_bytes > 0) {
        snprintf(stack_bytes,
                 sizeof(stack_bytes),
                 "%u",
                 (unsigned)status->worker_stack_bytes);
    } else {
        strlcpy(stack_bytes, "-", sizeof(stack_bytes));
    }
    const char *format = "%-12s %-8s %5s %-11s %4s %5u %3u\n";
    if (status->state == SOLAR_OS_JOB_RUNNING) {
        solar_os_shell_io_printf_bold(term,
                                      format,
                                      status->name != NULL ? status->name : "?",
                                      solar_os_job_state_name(status->state),
                                      stack_bytes,
                                      solar_os_job_kind_name(status->kind),
                                      status->has_event ? "tick" : "-",
                                      (unsigned)status->tick_count,
                                      (unsigned)status->resource_count);
    } else {
        solar_os_shell_io_printf(term,
                                 format,
                                 status->name != NULL ? status->name : "?",
                                 solar_os_job_state_name(status->state),
                                 stack_bytes,
                                 solar_os_job_kind_name(status->kind),
                                 status->has_event ? "tick" : "-",
                                 (unsigned)status->tick_count,
                                 (unsigned)status->resource_count);
    }
    if (status->state == SOLAR_OS_JOB_FAILED && status->last_error != ESP_OK) {
        char error_detail[SOLAR_OS_JOB_ERROR_DETAIL_MAX];
        solar_os_shell_io_printf(term,
                                 "  last error: %s\n",
                                 solar_os_jobs_get_error_detail(status->name,
                                                                error_detail,
                                                                sizeof(error_detail)) ?
                                     error_detail :
                                     solar_os_shell_error_text(status->last_error));
    }
    if (!detail) {
        return;
    }

    solar_os_shell_io_printf(term,
                             "  summary: %s\n",
                             status->summary != NULL ? status->summary : "job");
    solar_os_shell_io_printf(term,
                             "  owner: %s\n",
                             status->owner[0] != '\0' ? status->owner : "-");
    solar_os_job_inspection_t inspection;
    if (status->name != NULL &&
        solar_os_jobs_inspect_by_name(status->name, &inspection)) {
        solar_os_shell_io_printf(
            term,
            "  start admission: %s (%s)\n",
            solar_os_job_start_disposition_name(inspection.disposition),
            solar_os_job_start_reason_name(inspection.reason));
    }
    if (status->worker_stack_bytes > 0) {
        solar_os_shell_io_printf(term,
                                 "  worker stack: %u bytes (%s)\n",
                                 (unsigned)status->worker_stack_bytes,
                                 status->worker_stack_external ? "external" : "internal");
    } else {
        solar_os_shell_io_writeln(term, "  worker stack: none declared");
    }
    if (status->state == SOLAR_OS_JOB_WAITING) {
        solar_os_shell_io_writeln(
            term,
            "  waiting for worker stack memory; launch retries automatically");
    }
    if (status->has_event) {
        solar_os_shell_io_printf(term,
                                 "  tick: %" PRIu32 "/%" PRIu32 "ms n=%" PRIu32
                                 " us=%" PRIu32 "/%" PRIu32 " miss=%" PRIu32 "\n",
                                 status->tick_stats.interval_ms,
                                 status->tick_stats.deadline_ms,
                                 status->tick_stats.dispatch_count,
                                 status->tick_stats.last_duration_us,
                                 status->tick_stats.max_duration_us,
                                 status->tick_stats.deadline_miss_count);
    }
    if (status->resource_count == 0) {
        solar_os_shell_io_writeln(term, "  resources: none");
        return;
    }

    solar_os_shell_io_writeln(term, "  resources:");
    for (size_t i = 0; i < status->resource_count; i++) {
        job_print_resource(term, &status->resources[i]);
    }
}

static bool job_start_port_arg(int argc,
                               char **argv,
                               const char **port_name,
                               uint32_t *required_caps)
{
    if (argc < 4 || argv == NULL || argv[2] == NULL || argv[3] == NULL ||
        port_name == NULL || required_caps == NULL) {
        return false;
    }

    if (strcmp(argv[2], "log") == 0 && strcmp(argv[3], "file") != 0) {
        *port_name = argv[3];
        *required_caps = SOLAR_OS_PORT_CAP_WRITE;
        return true;
    }

    if (strcmp(argv[2], "bridge") == 0) {
        *port_name = argv[3];
        *required_caps = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE;
        return true;
    }

    return false;
}

static bool job_print_single_port_error(solar_os_shell_io_t *term,
                                        const char *port_name,
                                        uint32_t required_caps)
{
    solar_os_port_info_t info;
    const esp_err_t port_err = solar_os_port_get_info(port_name, &info);
    if (port_err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "job start failed: port not found: %s\n", port_name);
        return true;
    }
    if (port_err != ESP_OK) {
        return false;
    }

    if (info.claimed) {
        solar_os_job_status_t owner_job;
        const bool owner_is_job = info.owner[0] != '\0' &&
            solar_os_jobs_get_by_owner(info.owner, &owner_job);
        if (owner_is_job) {
            solar_os_shell_io_printf(term,
                                     "job start failed: job %s owns %s\n",
                                     owner_job.name != NULL ? owner_job.name : "?",
                                     info.name);
        } else {
            solar_os_shell_io_printf(term,
                                     "job start failed: %s owns %s\n",
                                     info.owner[0] != '\0' ? info.owner : "another owner",
                                     info.name);
        }
        return true;
    }

    if ((info.capabilities & required_caps) != required_caps) {
        char have[4];
        char need[4];
        solar_os_shell_io_printf(term,
                                 "job start failed: %s has %s, needs %s\n",
                                 info.name,
                                 solar_os_port_capabilities_text(info.capabilities,
                                                                 have,
                                                                 sizeof(have)),
                                 solar_os_port_capabilities_text(required_caps,
                                                                 need,
                                                                 sizeof(need)));
        return true;
    }

    return false;
}

static bool job_print_start_port_error(solar_os_shell_io_t *term,
                                       int argc,
                                       char **argv,
                                       esp_err_t err)
{
    if (err != ESP_ERR_INVALID_STATE &&
        err != ESP_ERR_NOT_FOUND &&
        err != ESP_ERR_NOT_SUPPORTED) {
        return false;
    }

    const char *port_name = NULL;
    uint32_t required_caps = 0;
    if (!job_start_port_arg(argc, argv, &port_name, &required_caps)) {
        return false;
    }

    if (job_print_single_port_error(term, port_name, required_caps)) {
        return true;
    }
    if (strcmp(argv[2], "bridge") == 0 && argc >= 5 && argv[4] != NULL) {
        return job_print_single_port_error(term,
                                           argv[4],
                                           SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE);
    }

    return false;
}

void solar_os_shell_cmd_jobs(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc != 1) {
        solar_os_shell_diag_unexpected(term, "jobs", argv[1], "jobs");
        return;
    }

    const size_t count = solar_os_jobs_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "no jobs registered");
        return;
    }

    solar_os_shell_io_writeln(term, "NAME         STATE    STACK KIND        EVT  TICKS RES");
    for (size_t i = 0; i < count; i++) {
        solar_os_job_status_t status;
        if (solar_os_jobs_get(i, &status)) {
            job_print_status(term, &status, false);
        }
    }
}

void solar_os_shell_cmd_job(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        solar_os_shell_diag_missing(term, "job", "<subcommand>",
                                    "job status|start|stop");
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (argc == 2) {
            solar_os_shell_cmd_jobs(ctx, 1, argv);
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "job status", argv[3],
                                           "job status [name]");
            return;
        }

        solar_os_job_status_t status;
        if (!solar_os_jobs_get_by_name(argv[2], &status)) {
            solar_os_shell_io_printf(term, "job: not found: %s\n", argv[2]);
            return;
        }
        solar_os_shell_io_writeln(term, "NAME         STATE    STACK KIND        EVT  TICKS RES");
        job_print_status(term, &status, true);
        if (status.detail != NULL) {
            status.detail(ctx);
        }
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term, "job start", "<name>",
                                        "job start <name> [args...]");
            return;
        }

        if (strcmp(argv[2], "shell") == 0) {
            solar_os_display_target_t display_target;
            if (argc == 4 && solar_os_display_find_target(argv[3], &display_target)) {
                char busy_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
                uint8_t session_id = 0;
                const esp_err_t err =
                    solar_os_sessions_create_detached_display_shell(
                        argv[3],
                        &session_id,
                        busy_owner,
                        sizeof(busy_owner));
                if (err == ESP_OK) {
                    solar_os_shell_io_printf(term,
                                             "job shell moved to sessions; session %u created: shell on %s\n",
                                             (unsigned)session_id,
                                             argv[3]);
                } else if (err == ESP_ERR_INVALID_STATE && busy_owner[0] != '\0') {
                    solar_os_shell_io_printf(term,
                                             "session create failed: %s owned by %s\n",
                                             argv[3],
                                             busy_owner);
                } else {
                    solar_os_shell_io_printf(term,
                                             "session create failed: %s\n",
                                             solar_os_shell_error_text(err));
                }
                return;
            }

            solar_os_port_shell_options_t options;
            if (argc < 4 || !parse_port_shell_options(argc, argv, 4, &options)) {
                solar_os_shell_diag_problem(
                    term, "job start shell", argc < 4 ? "missing argument <port>" :
                                                        "invalid port shell option",
                    "job start shell <port> [--term auto|vt100|ansi|dumb] [--charset utf8|ascii] [--size COLSxROWS]",
                    "job start shell is retained for compatibility; 'session create shell' is preferred");
                return;
            }
            uint8_t session_id = 0;
            const esp_err_t err =
                solar_os_port_shell_start_with_options(ctx, argv[3], &options, false, &session_id);
            if (err == ESP_OK) {
                solar_os_shell_io_printf(term,
                                         "job shell moved to sessions; session %u created: shell on %s term=%s charset=%s\n",
                                         (unsigned)session_id,
                                         argv[3],
                                         solar_os_shell_terminal_profile_name(options.terminal_profile),
                                         solar_os_shell_charset_name(options.charset));
            } else {
                solar_os_shell_io_printf(term,
                                         "session create failed: %s\n",
                                         solar_os_shell_error_text(err));
            }
            return;
        }

        const esp_err_t err = solar_os_jobs_start(ctx, argv[2], argc - 2, &argv[2]);
        if (err == ESP_OK) {
            solar_os_job_status_t status;
            if (solar_os_jobs_get_by_name(argv[2], &status) &&
                status.state == SOLAR_OS_JOB_WAITING) {
                solar_os_shell_io_printf(
                    term,
                    "job waiting: %s (needs %u-byte worker stack; retries automatically)\n",
                    argv[2],
                    (unsigned)status.worker_stack_bytes);
            } else {
                solar_os_shell_io_printf(term, "job started: %s\n", argv[2]);
            }
        } else if (job_print_start_port_error(term, argc, argv, err)) {
            return;
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_job_status_t status;
            if (!solar_os_jobs_get_by_name(argv[2], &status)) {
                solar_os_shell_io_printf(term, "job: not found: %s\n", argv[2]);
            } else {
                solar_os_shell_io_printf(term,
                                         "job start failed: not found\n");
            }
        } else {
            char error_detail[SOLAR_OS_JOB_ERROR_DETAIL_MAX];
            if (solar_os_jobs_get_error_detail(argv[2],
                                               error_detail,
                                               sizeof(error_detail))) {
                solar_os_shell_io_printf(term,
                                         "job start failed: %s\n",
                                         error_detail);
            } else {
                solar_os_shell_io_printf(term,
                                         "job start failed: %s\n",
                                         solar_os_shell_error_text(err));
            }
        }
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc != 3) {
            if (argc < 3) {
                solar_os_shell_diag_missing(term, "job stop", "<name>", "job stop <name>");
            } else {
                solar_os_shell_diag_unexpected(term, "job stop", argv[3], "job stop <name>");
            }
            return;
        }

        if (strcmp(argv[2], "shell") == 0) {
            solar_os_shell_io_writeln(term, "job shell moved to sessions; use sessions and close <session-id>");
            return;
        }

        const esp_err_t err = solar_os_jobs_stop(ctx, argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "job stopped: %s\n", argv[2]);
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "job: not found: %s\n", argv[2]);
        } else {
            solar_os_shell_io_printf(term,
                                     "job stop failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "job",
                                   argc,
                                   argv,
                                   "job status|start|stop",
                                   job_commands,
                                   sizeof(job_commands) / sizeof(job_commands[0]));
}

static void setterm_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  setterm");
    solar_os_shell_io_writeln(term, "  setterm --display <target> [orientation|font|textsize|palette|statusbar] [value]");
    solar_os_shell_io_writeln(term, "  setterm orientation [0|90|180|270]");
    solar_os_shell_io_writeln(term, "  setterm font [mono|compact]");
    solar_os_shell_io_writeln(term, "  setterm textsize [10|12|14|16|18|20]");
    solar_os_shell_io_writeln(term, "  setterm palette [normal|inverted]");
    solar_os_shell_io_writeln(term, "  setterm foreground [#RRGGBB]");
    solar_os_shell_io_writeln(term, "  setterm background [#RRGGBB]");
    solar_os_shell_io_writeln(term, "  setterm statusbar [show|hide]");
    solar_os_shell_io_writeln(term, "  setterm brightness [0..100]");
    solar_os_shell_io_writeln(term, "  setterm profile [vt100|ansi|dumb]");
    solar_os_shell_io_writeln(term, "  setterm charset [utf8|ascii]");
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    solar_os_shell_io_writeln(term, "  setterm keyboard [us|de]");
    solar_os_shell_io_writeln(term, "  setterm ble [default|on|off]");
#endif
    solar_os_shell_io_writeln(term, "  setterm powerkey [sleep|suspend]");
    solar_os_shell_io_writeln(term, "  setterm keyrate [off|1..60 [delay-ms]]");
    solar_os_shell_io_writeln(term, "  setterm timezone [UTC|UTC+/-offset|Europe/Berlin|POSIX-TZ]");
    solar_os_shell_io_writeln(term, "  setterm startup [flash|sd]");
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    solar_os_shell_io_writeln(term, "  setterm otaurl [url]");
#endif
}

static void setterm_print_save_result(solar_os_shell_io_t *term,
                                      const char *setting,
                                      const char *value,
                                      esp_err_t err)
{
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "%s: %s\n", setting, value);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_printf(term, "%s: invalid value: %s\n", setting, value);
    } else {
        solar_os_shell_io_printf(term,
                                 "%s: applied but save failed: %s\n",
                                 setting,
                                 solar_os_shell_error_text(err));
    }
}

static void setterm_print_keyrate(solar_os_shell_io_t *term)
{
    uint16_t keyrate = 0;
    uint16_t keydelay_ms = 0;

    solar_os_input_get_repeat(&keyrate, &keydelay_ms);
    if (keyrate == 0) {
        solar_os_shell_io_writeln(term, "keyrate: off");
        return;
    }

    solar_os_shell_io_printf(term,
                             "keyrate: %u cps delay %u ms\n",
                             (unsigned)keyrate,
                             (unsigned)keydelay_ms);
}

static void setterm_print_keyrate_result(solar_os_shell_io_t *term, esp_err_t err)
{
    if (err == ESP_OK) {
        setterm_print_keyrate(term);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "keyrate: invalid value");
    } else {
        solar_os_shell_io_printf(term,
                                 "keyrate: applied but save failed: %s\n",
                                 solar_os_shell_error_text(err));
    }
}

static void setterm_print_color(solar_os_shell_io_t *term,
                                const char *setting,
                                uint32_t rgb888)
{
    solar_os_shell_io_printf(term, "%s: #%06" PRIx32 "\n", setting, rgb888);
}

static void setterm_print_color_result(solar_os_shell_io_t *term,
                                       const char *setting,
                                       uint32_t rgb888,
                                       esp_err_t err)
{
    if (err == ESP_OK) {
        setterm_print_color(term, setting, rgb888);
    } else {
        setterm_print_save_result(term, setting, "color", err);
    }
}

static bool setterm_display_setting(const char *setting)
{
    return setting != NULL &&
        (strcmp(setting, "orientation") == 0 ||
         strcmp(setting, "font") == 0 ||
         strcmp(setting, "textsize") == 0 ||
         strcmp(setting, "palette") == 0 ||
         strcmp(setting, "statusbar") == 0);
}

static void setterm_print_display_profile(
    solar_os_shell_io_t *term,
    const char *target_name,
    const solar_os_terminal_profile_t *profile)
{
    solar_os_shell_io_printf(term, "display: %s\n", target_name);
    solar_os_shell_io_printf(term, "orientation: %u\n",
                             (unsigned)profile->orientation_degrees);
    solar_os_shell_io_printf(term, "font: %s\n",
                             solar_os_terminal_font_name(profile->font));
    solar_os_shell_io_printf(
        term, "textsize: %s\n",
        solar_os_terminal_text_size_name(profile->text_size));
    solar_os_shell_io_printf(term, "palette: %s\n",
                             profile->palette_inverted ? "inverted" : "normal");
    solar_os_shell_io_printf(term, "statusbar: %s\n",
                             profile->status_bar_visible ? "show" : "hide");
}

static void setterm_print_display_result(solar_os_shell_io_t *term,
                                         const char *target_name,
                                         const char *setting,
                                         const char *value,
                                         esp_err_t err)
{
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "%s %s: %s\n", target_name, setting, value);
    } else {
        solar_os_shell_io_printf(term,
                                 "setterm: %s update failed: %s\n",
                                 target_name,
                                 solar_os_shell_error_text(err));
    }
}

void solar_os_shell_cmd_setterm(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    solar_os_terminal_t *display = display_terminal(ctx);
    const char *display_target = NULL;
    solar_os_terminal_profile_t display_profile = {0};

    if (argc >= 2 && strcmp(argv[1], "--display") == 0) {
        if (argc < 3) {
            solar_os_shell_diag_missing(term,
                                        "setterm --display",
                                        "display target",
                                        "setterm --display <target> [orientation|font|textsize|palette|statusbar] [value]");
            return;
        }
        display_target = argv[2];
        const esp_err_t profile_err =
            solar_os_display_get_terminal_profile(display_target, &display_profile);
        if (profile_err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term,
                                     "setterm: display target not found: %s\n",
                                     display_target);
            return;
        }
        if (profile_err != ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "setterm: display unavailable: %s\n",
                                     solar_os_shell_error_text(profile_err));
            return;
        }
        argc -= 2;
        argv += 2;
        if (argc == 1) {
            setterm_print_display_profile(term, display_target, &display_profile);
            return;
        }
        if (!setterm_display_setting(argv[1])) {
            solar_os_shell_io_printf(
                term,
                "setterm: %s is not a display terminal setting\n",
                argv[1]);
            solar_os_shell_io_writeln(
                term,
                "values: orientation font textsize palette statusbar");
            return;
        }
    }

    if (argc == 1) {
        const esp_err_t err = solar_os_shell_launch_setterm_tui(ctx);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "setterm: launch failed: %s\n", solar_os_shell_error_text(err));
        } else {
            solar_os_shell_session_prepare_foreground_launch(ctx, true);
        }
        return;
    }

    if (strcmp(argv[1], "orientation") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(term,
                                     "orientation: %u\n",
                                     (unsigned)(display_target != NULL ?
                                         display_profile.orientation_degrees :
                                         solar_os_terminal_orientation(display)));
            solar_os_shell_io_writeln(term, "values: 0 90 180 270");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm orientation", argv[3],
                                           "setterm orientation [0|90|180|270]");
            return;
        }

        size_t degrees = 0;
        if (!parse_size_arg(argv[2], 0, 270, &degrees) ||
            !(degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270)) {
            solar_os_shell_diag_invalid(term, "setterm orientation", "orientation", argv[2],
                                        "0, 90, 180, or 270 degrees",
                                        "setterm orientation [0|90|180|270]", false);
            return;
        }

        const esp_err_t err =
            display_target != NULL ?
                (display_profile.orientation_degrees = (uint16_t)degrees,
                 solar_os_sessions_set_display_terminal_profile(display_target,
                                                                &display_profile)) :
                solar_os_sessions_set_terminal_orientation(display, (uint16_t)degrees);
        if (display_target != NULL) {
            setterm_print_display_result(term, display_target, "orientation", argv[2], err);
        } else {
            setterm_print_save_result(term, "orientation", argv[2], err);
        }
        return;
    }

    if (strcmp(argv[1], "font") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(term,
                                     "font: %s\n",
                                     solar_os_terminal_font_name(
                                         display_target != NULL ?
                                             display_profile.font :
                                             solar_os_terminal_font(display)));
            solar_os_shell_io_writeln(term, "values: mono compact");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm font", argv[3],
                                           "setterm font [mono|compact]");
            return;
        }

        solar_os_terminal_font_t font;
        if (!solar_os_terminal_parse_font(argv[2], &font)) {
            solar_os_shell_diag_invalid(term, "setterm font", "font", argv[2],
                                        "mono or compact", "setterm font [mono|compact]", false);
            return;
        }

        const esp_err_t err = display_target != NULL ?
            (display_profile.font = font,
             solar_os_sessions_set_display_terminal_profile(display_target,
                                                            &display_profile)) :
            solar_os_sessions_set_terminal_font(display, font);
        if (display_target != NULL) {
            setterm_print_display_result(term, display_target, "font", argv[2], err);
        } else {
            setterm_print_save_result(term, "font", argv[2], err);
        }
        return;
    }

    if (strcmp(argv[1], "textsize") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(
                term,
                "textsize: %s\n",
                solar_os_terminal_text_size_name(
                    display_target != NULL ?
                        display_profile.text_size :
                        solar_os_terminal_text_size(display)));
            solar_os_shell_io_writeln(term, "values: 10 12 14 16 18 20");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm textsize", argv[3],
                                           "setterm textsize [10|12|14|16|18|20]");
            return;
        }

        solar_os_terminal_text_size_t text_size;
        if (!solar_os_terminal_parse_text_size(argv[2], &text_size)) {
            solar_os_shell_diag_invalid(term, "setterm textsize", "size", argv[2],
                                        "10, 12, 14, 16, 18, or 20",
                                        "setterm textsize [10|12|14|16|18|20]", false);
            return;
        }

        const esp_err_t err = display_target != NULL ?
            (display_profile.text_size = text_size,
             solar_os_sessions_set_display_terminal_profile(display_target,
                                                            &display_profile)) :
            solar_os_sessions_set_terminal_text_size(display, text_size);
        if (display_target != NULL) {
            setterm_print_display_result(term, display_target, "textsize", argv[2], err);
        } else {
            setterm_print_save_result(term, "textsize", argv[2], err);
        }
        return;
    }

    if (strcmp(argv[1], "palette") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(
                term,
                "palette: %s\n",
                (display_target != NULL ?
                     display_profile.palette_inverted :
                 display != NULL ?
                     solar_os_terminal_palette_inverted(display) :
                     solar_os_terminal_palette_preference_inverted()) ?
                    "inverted" :
                    "normal");
            solar_os_shell_io_writeln(term, "values: normal inverted");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm palette", argv[3],
                                           "setterm palette [normal|inverted]");
            return;
        }
        if (strcmp(argv[2], "normal") != 0 && strcmp(argv[2], "inverted") != 0) {
            solar_os_shell_diag_invalid(term, "setterm palette", "palette", argv[2],
                                        "normal or inverted",
                                        "setterm palette [normal|inverted]", false);
            return;
        }

        const bool inverted = strcmp(argv[2], "inverted") == 0;
        const esp_err_t err = display_target != NULL ?
            (display_profile.palette_inverted = inverted,
             solar_os_sessions_set_display_terminal_profile(display_target,
                                                            &display_profile)) :
            solar_os_sessions_set_terminal_palette_inverted(display, inverted);
        if (display_target != NULL) {
            setterm_print_display_result(term, display_target, "palette", argv[2], err);
        } else {
            setterm_print_save_result(term, "palette", argv[2], err);
        }
        return;
    }

    if (strcmp(argv[1], "foreground") == 0 || strcmp(argv[1], "background") == 0) {
        const bool foreground = strcmp(argv[1], "foreground") == 0;
        const char *setting = foreground ? "foreground" : "background";
        if (argc == 2) {
            uint32_t foreground_rgb888 = 0;
            uint32_t background_rgb888 = 0;
            (void)solar_os_display_get_colors(&foreground_rgb888, &background_rgb888);
            setterm_print_color(term,
                                setting,
                                foreground ? foreground_rgb888 : background_rgb888);
            return;
        }
        if (argc != 3) {
            char command[32];
            char usage[48];
            snprintf(command, sizeof(command), "setterm %s", setting);
            snprintf(usage, sizeof(usage), "setterm %s [#RRGGBB]", setting);
            solar_os_shell_diag_unexpected(term, command, argv[3], usage);
            return;
        }

        uint32_t rgb888 = 0;
        if (!solar_os_shell_parse_rgb888(argv[2], &rgb888)) {
            char command[32];
            char usage[48];
            snprintf(command, sizeof(command), "setterm %s", setting);
            snprintf(usage, sizeof(usage), "%s [#RRGGBB]", command);
            solar_os_shell_diag_invalid(term,
                                        command,
                                        "color",
                                        argv[2],
                                        "#RRGGBB, RRGGBB, or 0xRRGGBB",
                                        usage,
                                        false);
            return;
        }

        const esp_err_t err = foreground ?
            solar_os_display_set_foreground_color(rgb888) :
            solar_os_display_set_background_color(rgb888);
        setterm_print_color_result(term, setting, rgb888, err);
        return;
    }

    if (strcmp(argv[1], "statusbar") == 0) {
        if (argc == 2) {
            const bool visible = display_target != NULL ?
                display_profile.status_bar_visible :
                (display != NULL ?
                    solar_os_terminal_status_bar_visible(display) :
                    solar_os_terminal_status_bar_preference_visible());
            solar_os_shell_io_printf(term, "statusbar: %s\n", visible ? "show" : "hide");
            solar_os_shell_io_writeln(term, "values: show hide");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm statusbar", argv[3],
                                           "setterm statusbar [show|hide]");
            return;
        }
        if (strcmp(argv[2], "show") != 0 && strcmp(argv[2], "hide") != 0) {
            solar_os_shell_diag_invalid(term, "setterm statusbar", "statusbar", argv[2],
                                        "show or hide",
                                        "setterm statusbar [show|hide]", false);
            return;
        }

        const bool visible = strcmp(argv[2], "show") == 0;
        const esp_err_t err = display_target != NULL ?
            (display_profile.status_bar_visible = visible,
             solar_os_sessions_set_display_terminal_profile(display_target,
                                                            &display_profile)) :
            solar_os_sessions_set_terminal_status_bar_visible(display, visible);
        if (display_target != NULL) {
            setterm_print_display_result(term, display_target, "statusbar", argv[2], err);
        } else {
            setterm_print_save_result(term, "statusbar", argv[2], err);
        }
        return;
    }

    if (strcmp(argv[1], "brightness") == 0 || strcmp(argv[1], "backlight") == 0) {
        if (argc == 2) {
            uint8_t percent = 0;
            const esp_err_t err = solar_os_display_get_brightness(&percent);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                solar_os_shell_io_writeln(term, "brightness: unsupported");
                return;
            }
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term,
                                         "brightness: unavailable: %s\n",
                                         solar_os_shell_error_text(err));
                return;
            }
            solar_os_shell_io_printf(term, "brightness: %u\n", (unsigned)percent);
            solar_os_shell_io_writeln(term, "values: 0..100");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm brightness", argv[3],
                                           "setterm brightness [0..100]");
            return;
        }

        size_t percent = 0;
        if (!parse_size_arg(argv[2], 0, 100, &percent)) {
            solar_os_shell_diag_invalid(term, "setterm brightness", "percent", argv[2],
                                        "an integer from 0 to 100",
                                        "setterm brightness [0..100]", false);
            return;
        }

        const esp_err_t err = solar_os_display_set_brightness((uint8_t)percent);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "brightness: unsupported");
            return;
        }
        setterm_print_save_result(term, "brightness", argv[2], err);
        return;
    }

    if (strcmp(argv[1], "profile") == 0) {
        if (solar_os_shell_io_kind(term) != SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term,
                                      "profile: set profile on port shell to vt100, ansi, or dumb");
            return;
        }

        if (argc == 2) {
            solar_os_shell_io_printf(
                term,
                "profile: %s\n",
                solar_os_shell_terminal_profile_name(solar_os_shell_io_terminal_profile(term)));
            solar_os_shell_io_writeln(term, "values: vt100 ansi dumb");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm profile", argv[3],
                                           "setterm profile [vt100|ansi|dumb]");
            return;
        }

        solar_os_shell_terminal_profile_t profile;
        if (!solar_os_shell_parse_terminal_profile(argv[2], &profile) ||
            profile == SOLAR_OS_SHELL_TERMINAL_PROFILE_AUTO) {
            solar_os_shell_diag_invalid(term, "setterm profile", "profile", argv[2],
                                        "vt100, ansi, or dumb",
                                        "setterm profile [vt100|ansi|dumb]", false);
            return;
        }

        solar_os_shell_io_set_terminal_profile(term, profile);
        solar_os_shell_io_printf(term,
                                 "profile: %s\n",
                                 solar_os_shell_terminal_profile_name(profile));
        return;
    }

    if (strcmp(argv[1], "charset") == 0) {
        if (solar_os_shell_io_kind(term) != SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term,
                                      "charset: set character set on a port shell to utf8 or ascii");
            return;
        }

        if (argc == 2) {
            solar_os_shell_io_printf(
                term,
                "charset: %s\n",
                solar_os_shell_charset_name(solar_os_shell_io_charset(term)));
            solar_os_shell_io_writeln(term, "values: utf8 ascii");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm charset", argv[3],
                                           "setterm charset [utf8|ascii]");
            return;
        }

        solar_os_shell_charset_t charset;
        if (!solar_os_shell_parse_charset(argv[2], &charset)) {
            solar_os_shell_diag_invalid(term, "setterm charset", "charset", argv[2],
                                        "utf8 or ascii", "setterm charset [utf8|ascii]", false);
            return;
        }

        solar_os_shell_io_set_charset(term, charset);
        solar_os_shell_io_printf(term,
                                 "charset: %s\n",
                                 solar_os_shell_charset_name(charset));
        return;
    }

    if (strcmp(argv[1], "keyboard") == 0 || strcmp(argv[1], "keymap") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(term,
                                     "keyboard: %s\n",
                                     solar_os_input_keyboard_layout_name(
                                         solar_os_input_keyboard_layout()));
            solar_os_shell_io_writeln(term, "values: us de");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm keyboard", argv[3],
                                           "setterm keyboard [us|de]");
            return;
        }

        solar_os_input_keyboard_layout_t layout;
        if (!solar_os_input_parse_keyboard_layout(argv[2], &layout)) {
            solar_os_shell_diag_invalid(term, "setterm keyboard", "layout", argv[2],
                                        "us or de", "setterm keyboard [us|de]", false);
            return;
        }

        const esp_err_t err = solar_os_input_set_keyboard_layout(layout);
        setterm_print_save_result(term, "keyboard", argv[2], err);
        return;
    }

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    if (strcmp(argv[1], "ble") == 0) {
        if (argc == 2) {
            const solar_os_ble_keyboard_boot_setting_t setting =
                solar_os_ble_keyboard_boot_setting();
            solar_os_shell_io_printf(
                term,
                "ble: %s (next boot %s, board default %s)\n",
                solar_os_ble_keyboard_boot_setting_name(setting),
                solar_os_ble_keyboard_enabled_for_next_boot() ? "on" : "off",
                solar_os_ble_keyboard_board_default_enabled() ? "on" : "off");
            solar_os_shell_io_printf(
                term,
                "current boot: %s\n",
                solar_os_ble_keyboard_enabled_for_current_boot() ? "on" : "off");
            solar_os_shell_io_writeln(term, "values: default on off");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term,
                                           "setterm ble",
                                           argv[3],
                                           "setterm ble [default|on|off]");
            return;
        }

        solar_os_ble_keyboard_boot_setting_t setting;
        if (!solar_os_ble_keyboard_parse_boot_setting(argv[2], &setting)) {
            solar_os_shell_diag_invalid(term,
                                        "setterm ble",
                                        "preference",
                                        argv[2],
                                        "default, on, or off",
                                        "setterm ble [default|on|off]",
                                        false);
            return;
        }
        const esp_err_t err = solar_os_ble_keyboard_set_boot_setting(setting);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "ble: save failed: %s\n",
                                     solar_os_shell_error_text(err));
            return;
        }
        solar_os_shell_io_printf(
            term,
            "ble: %s; next boot %s\n",
            solar_os_ble_keyboard_boot_setting_name(setting),
            solar_os_ble_keyboard_enabled_for_next_boot() ? "on" : "off");
        solar_os_shell_io_writeln(term, "The current boot is unchanged; reboot to apply.");
        return;
    }
#endif

    if (strcmp(argv[1], "powerkey") == 0 || strcmp(argv[1], "key") == 0) {
        if (argc == 2) {
            solar_os_power_status_t status;
            solar_os_power_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "powerkey: %s\n",
                                     solar_os_power_key_action_name(status.key_action));
            solar_os_shell_io_writeln(term, "values: sleep suspend");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm powerkey", argv[3],
                                           "setterm powerkey [sleep|suspend]");
            return;
        }

        solar_os_power_key_action_t action;
        if (!solar_os_power_parse_key_action(argv[2], &action) ||
            (action != SOLAR_OS_POWER_KEY_ACTION_SLEEP &&
             action != SOLAR_OS_POWER_KEY_ACTION_SUSPEND)) {
            solar_os_shell_diag_invalid(term,
                                        "setterm powerkey",
                                        "action",
                                        argv[2],
                                        "sleep or suspend",
                                        "setterm powerkey [sleep|suspend]",
                                        false);
            return;
        }

        const esp_err_t err = solar_os_power_set_key_action(action);
        setterm_print_save_result(term, "powerkey", argv[2], err);
        return;
    }

    if (strcmp(argv[1], "keyrate") == 0 ||
        strcmp(argv[1], "typerate") == 0 ||
        strcmp(argv[1], "repeat") == 0) {
        if (argc == 2) {
            setterm_print_keyrate(term);
            solar_os_shell_io_printf(term,
                                     "values: off or %u..%u [delay %u..%u ms]\n",
                                     (unsigned)SOLAR_OS_INPUT_REPEAT_RATE_MIN,
                                     (unsigned)SOLAR_OS_INPUT_REPEAT_RATE_MAX,
                                     (unsigned)SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS,
                                     (unsigned)SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS);
            return;
        }
        if (argc < 3 || argc > 4) {
            solar_os_shell_diag_unexpected(term, "setterm keyrate", argv[4],
                                           "setterm keyrate [off|1..60 [delay-ms]]");
            return;
        }

        uint16_t current_delay_ms = 0;
        solar_os_input_get_repeat(NULL, &current_delay_ms);

        if (strcmp(argv[2], "off") == 0) {
            if (argc != 3) {
                solar_os_shell_diag_unexpected(term, "setterm keyrate off", argv[3],
                                               "setterm keyrate off");
                return;
            }

            const esp_err_t err = solar_os_input_set_repeat(0, current_delay_ms);
            setterm_print_keyrate_result(term, err);
            return;
        }

        size_t rate = 0;
        if (!parse_size_arg(argv[2],
                            SOLAR_OS_INPUT_REPEAT_RATE_MIN,
                            SOLAR_OS_INPUT_REPEAT_RATE_MAX,
                            &rate)) {
            solar_os_shell_diag_invalid(term, "setterm keyrate", "rate", argv[2],
                                        "off or an integer from 1 to 60",
                                        "setterm keyrate [off|1..60 [delay-ms]]", false);
            return;
        }

        size_t delay_ms = current_delay_ms;
        if (argc == 4 &&
            !parse_size_arg(argv[3],
                            SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS,
                            SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS,
                            &delay_ms)) {
            solar_os_shell_diag_invalid(term, "setterm keyrate", "delay-ms", argv[3],
                                        "an integer from 100 to 2000",
                                        "setterm keyrate [off|1..60 [delay-ms]]", false);
            return;
        }

        const esp_err_t err =
            solar_os_input_set_repeat((uint16_t)rate, (uint16_t)delay_ms);
        setterm_print_keyrate_result(term, err);
        return;
    }
    if (strcmp(argv[1], "timezone") == 0) {
        char timezone[SOLAR_OS_TIMEZONE_NAME_MAX];
        char posix[SOLAR_OS_TIMEZONE_POSIX_MAX];

        if (argc == 2) {
            solar_os_time_get_timezone(timezone, sizeof(timezone), posix, sizeof(posix));
            solar_os_shell_io_printf(term, "timezone: %s\n", timezone);
            if (strcmp(timezone, posix) != 0) {
                solar_os_shell_io_printf(term, "posix: %s\n", posix);
            }
            solar_os_shell_io_writeln(term, "values: UTC UTC+/-offset Europe/Berlin or POSIX TZ");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(
                term, "setterm timezone", argv[3],
                "setterm timezone [UTC|UTC+/-offset|Europe/Berlin|POSIX-TZ]");
            return;
        }

        const esp_err_t err = solar_os_time_set_timezone(argv[2]);
        if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_diag_invalid(
                term, "setterm timezone", "timezone", argv[2],
                "UTC, UTC+/-offset, Europe/Berlin, or a POSIX timezone",
                "setterm timezone [UTC|UTC+/-offset|Europe/Berlin|POSIX-TZ]", false);
            return;
        }

        solar_os_time_get_timezone(timezone, sizeof(timezone), NULL, 0);
        setterm_print_save_result(term, "timezone", timezone, err);
        return;
    }

    if (strcmp(argv[1], "startup") == 0) {
        if (argc == 2) {
            char path[SOLAR_OS_STORAGE_PATH_MAX];
            const solar_os_shell_startup_source_t source = solar_os_shell_startup_source();
            solar_os_shell_io_printf(term,
                                     "startup: %s\n",
                                     solar_os_shell_startup_source_name(source));
            if (solar_os_shell_startup_path(path, sizeof(path)) == ESP_OK) {
                solar_os_shell_io_printf(term, "path: %s\n", path);
            }
            solar_os_shell_io_writeln(
                term,
                solar_os_board_has(SOLAR_OS_BOARD_CAP_SD) ?
                    "values: flash sd" :
                    "values: flash (SD is not supported on this board)");
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term,
                                           "setterm startup",
                                           argv[3],
                                           "setterm startup [flash|sd]");
            return;
        }

        solar_os_shell_startup_source_t source;
        if (!solar_os_shell_parse_startup_source(argv[2], &source)) {
            solar_os_shell_diag_invalid(term,
                                        "setterm startup",
                                        "source",
                                        argv[2],
                                        "flash or sd",
                                        "setterm startup [flash|sd]",
                                        false);
            return;
        }

        const esp_err_t err = solar_os_shell_set_startup_source(source);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "startup: SD is not supported on this board");
            return;
        }
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "startup: save failed: %s\n",
                                     solar_os_shell_error_text(err));
            return;
        }

        char path[SOLAR_OS_STORAGE_PATH_MAX];
        solar_os_shell_io_printf(term, "startup: %s\n", argv[2]);
        if (solar_os_shell_startup_path(path, sizeof(path)) == ESP_OK) {
            solar_os_shell_io_printf(term, "path: %s\n", path);
        }
        solar_os_shell_io_writeln(term, "The new startup source applies on the next boot.");
        return;
    }

#if SOLAR_OS_PACKAGE_SERVICE_OTA
    if (strcmp(argv[1], "otaurl") == 0 || strcmp(argv[1], "ota") == 0) {
        if (argc == 2) {
            char url[SOLAR_OS_OTA_URL_MAX];
            char target_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
            char index_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
            solar_os_ota_get_url(url, sizeof(url));
            solar_os_ota_get_flavor(target_flavor, sizeof(target_flavor));
            solar_os_shell_io_printf(term, "otaurl: %s\n", url);
            solar_os_shell_io_printf(term, "compiled flavor: %s\n", SOLAR_OS_FLAVOR_NAME);
            solar_os_shell_io_printf(term, "ota flavor: %s\n", target_flavor);
            if (solar_os_ota_get_index_url(index_url, sizeof(index_url)) == ESP_OK) {
                solar_os_shell_io_printf(term, "index: %s\n", index_url);
            }
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "setterm otaurl", argv[3],
                                           "setterm otaurl [url]");
            return;
        }

        const esp_err_t err = solar_os_ota_set_url(argv[2]);
        if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_printf(term, "otaurl: invalid value: %s\n", argv[2]);
            return;
        }
        setterm_print_save_result(term, "otaurl", argv[2], err);
        return;
    }
#endif

    solar_os_shell_diag_subcommand(term,
                                   "setterm",
                                   argc,
                                   argv,
                                   "setterm orientation|font|textsize|palette|foreground|background|statusbar|brightness|backlight|profile|charset|keyboard|ble|powerkey|keyrate|timezone|startup|otaurl",
                                   setterm_commands,
                                   sizeof(setterm_commands) / sizeof(setterm_commands[0]));
}

static void stream_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  stream list");
    solar_os_shell_io_writeln(term, "  stream status <id>");
}

static void stream_print_list(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_stream_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "streams: none");
        return;
    }

    solar_os_shell_io_writeln(term,
                              "ID                TYPE    DIR     PROVIDER     DEVICE       OWNER");
    for (size_t i = 0; i < count; i++) {
        solar_os_stream_info_t info;
        if (!solar_os_stream_get(i, &info)) {
            continue;
        }
        solar_os_shell_io_printf(term,
                                 "%-17s %-7s %-7s %-12s %-12s %s\n",
                                 info.id,
                                 solar_os_stream_type_name(info.type),
                                 solar_os_stream_direction_name(info.direction),
                                 info.provider[0] != '\0' ? info.provider : "-",
                                 info.device[0] != '\0' ? info.device : "-",
                                 info.owner[0] != '\0' ? info.owner : "-");
    }
}

void solar_os_shell_cmd_stream(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        stream_print_list(term);
        return;
    }

    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_stream_info_t info;
        const esp_err_t err = solar_os_stream_get_info(argv[2], &info);
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "stream: not found: %s\n", argv[2]);
            return;
        }
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "stream status failed: %s\n", solar_os_shell_error_text(err));
            return;
        }

        char header[SOLAR_OS_STREAM_CSV_HEADER_MAX];
        solar_os_shell_io_printf(term, "ID: %s\n", info.id);
        solar_os_shell_io_printf(term, "Type: %s\n", solar_os_stream_type_name(info.type));
        solar_os_shell_io_printf(term, "Direction: %s\n",
                                 solar_os_stream_direction_name(info.direction));
        solar_os_shell_io_printf(term, "Sharing: %s\n",
                                 solar_os_stream_sharing_name(info.sharing));
        solar_os_shell_io_printf(term, "Provider: %s\n",
                                 info.provider[0] != '\0' ? info.provider : "-");
        solar_os_shell_io_printf(term, "Device: %s\n",
                                 info.device[0] != '\0' ? info.device : "-");
        solar_os_shell_io_printf(term, "Format: %s\n", info.format);
        solar_os_shell_io_printf(term, "Unit: %s\n", info.unit[0] != '\0' ? info.unit : "-");
        solar_os_shell_io_printf(term, "Summary: %s\n", info.summary);
        solar_os_shell_io_printf(term, "Handles: %" PRIu32 "\n", info.active_handles);
        solar_os_shell_io_printf(term, "Owner: %s\n",
                                 info.owner[0] != '\0' ? info.owner : "-");
        solar_os_shell_io_printf(term,
                                 "Units: read %" PRIu64 ", written %" PRIu64 "\n",
                                 info.read_units,
                                 info.written_units);
        solar_os_shell_io_printf(term,
                                 "Flow errors: overruns %" PRIu32 ", underruns %" PRIu32 "\n",
                                 info.overruns,
                                 info.underruns);
        if (info.type == SOLAR_OS_STREAM_TYPE_AUDIO) {
            solar_os_shell_io_printf(
                term,
                "Audio: %s, %" PRIu32 " Hz, %u ch, %u bit, %u frames/block\n",
                solar_os_stream_audio_sample_format_name(info.audio.sample_format),
                info.audio.sample_rate,
                (unsigned)info.audio.channels,
                (unsigned)info.audio.bits_per_sample,
                (unsigned)info.audio.frames_per_block);
        } else if (solar_os_stream_csv_header(&info, header, sizeof(header)) == ESP_OK) {
            solar_os_shell_io_printf(term, "CSV: %s\n", header);
        }
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "stream",
                                   argc,
                                   argv,
                                   "stream list|status",
                                   stream_commands,
                                   sizeof(stream_commands) / sizeof(stream_commands[0]));
}


static char log_level_letter(solar_os_log_level_t level)
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

static bool parse_on_off_arg(const char *text, bool *enabled)
{
    if (text == NULL || enabled == NULL) {
        return false;
    }
    if (strcmp(text, "on") == 0) {
        *enabled = true;
        return true;
    }
    if (strcmp(text, "off") == 0) {
        *enabled = false;
        return true;
    }
    return false;
}

static void log_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  log status");
    solar_os_shell_io_writeln(term, "  log show [count]");
    solar_os_shell_io_writeln(term, "  log follow [error|warn|info|debug]");
    solar_os_shell_io_writeln(term, "  log clear");
    solar_os_shell_io_writeln(term, "  log level [error|warn|info|debug]");
    solar_os_shell_io_writeln(term, "  log sink cdc [on|off]");
}

static void log_print_status(solar_os_shell_io_t *term)
{
    solar_os_log_status_t status;
    const esp_err_t err = solar_os_log_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "log status failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term, "Log: %s\n", status.initialized ? "ready" : "unavailable");
    solar_os_shell_io_printf(term, "Level: %s\n", solar_os_log_level_name(status.level));
    solar_os_shell_io_printf(term, "CDC sink: %s\n", status.cdc_enabled ? "on" : "off");
    solar_os_shell_io_printf(term,
                             "Ring: %u/%u entries\n",
                             (unsigned)status.count,
                             (unsigned)status.capacity);
    solar_os_shell_io_printf(term,
                             "Storage: %s %u bytes\n",
                             status.ring_in_psram ? "PSRAM" : "SRAM",
                             (unsigned)status.bytes);
    solar_os_shell_io_printf(term, "Dropped: %" PRIu32 "\n", status.dropped);
}

static void log_cmd_show(solar_os_shell_io_t *term, int argc, char **argv)
{
    solar_os_log_status_t status;
    esp_err_t err = solar_os_log_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "log show failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    size_t count = status.count < LOG_SHOW_DEFAULT ? status.count : LOG_SHOW_DEFAULT;
    if (argc == 3) {
        if (!parse_size_arg(argv[2], 1, status.capacity, &count)) {
            solar_os_shell_io_printf(term, "log show count: 1..%u\n", (unsigned)status.capacity);
            return;
        }
    } else if (argc > 3) {
        solar_os_shell_diag_unexpected(term, "log show", argv[3], "log show [count]");
        return;
    }

    if (count == 0) {
        solar_os_shell_io_writeln(term, "logs: empty");
        return;
    }

    solar_os_log_entry_t *entries = solar_os_memory_alloc(sizeof(*entries) * count,
                                                           SOLAR_OS_MEMORY_TRANSIENT,
                                                           "shell.log");
    if (entries == NULL) {
        solar_os_shell_io_writeln(term, "log show: no memory");
        return;
    }

    size_t total = 0;
    const size_t copied = solar_os_log_snapshot(entries, count, &total);
    if (copied == 0) {
        solar_os_shell_io_writeln(term, "logs: empty");
        solar_os_memory_free(entries);
        return;
    }

    if (total > copied) {
        solar_os_shell_io_printf(term,
                                 "showing last %u of %u\n",
                                 (unsigned)copied,
                                 (unsigned)total);
    }

    for (size_t i = 0; i < copied; i++) {
        const solar_os_log_entry_t *entry = &entries[i];
        const uint32_t seconds = entry->timestamp_ms / 1000U;
        const uint32_t ms = entry->timestamp_ms % 1000U;
        solar_os_shell_io_printf(term,
                                 "%06" PRIu32 " %5" PRIu32 ".%03" PRIu32 " %c %-16s %s%s\n",
                                 entry->sequence,
                                 seconds,
                                 ms,
                                 log_level_letter(entry->level),
                                 entry->tag,
                                 entry->message,
                                 entry->truncated ? "..." : "");
    }

    solar_os_memory_free(entries);
}

static void log_cmd_follow(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    solar_os_log_level_t level = SOLAR_OS_LOG_LEVEL_INFO;

    if (argc == 2) {
        solar_os_log_status_t status;
        const esp_err_t err = solar_os_log_get_status(&status);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log follow failed: %s\n", solar_os_shell_error_text(err));
            return;
        }
        level = status.level;
    } else if (argc == 3) {
        if (!solar_os_log_parse_level(argv[2], &level)) {
            solar_os_shell_diag_invalid(term, "log follow", "level", argv[2],
                                        "error, warn, info, or debug",
                                        "log follow [error|warn|info|debug]", false);
            return;
        }
    } else {
        solar_os_shell_diag_unexpected(term, "log follow", argv[3],
                                       "log follow [error|warn|info|debug]");
        return;
    }

    const esp_err_t err = solar_os_shell_session_start_log_follow(ctx, level);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "log follow: another foreground shell mode is active");
    } else if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "log follow failed: %s\n", solar_os_shell_error_text(err));
    }
}

void solar_os_shell_cmd_log(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_diag_unexpected(term, "log status", argv[2], "log status");
            return;
        }
        log_print_status(term);
        return;
    }

    if (strcmp(argv[1], "show") == 0) {
        log_cmd_show(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "follow") == 0) {
        log_cmd_follow(ctx, argc, argv);
        return;
    }

    if (strcmp(argv[1], "clear") == 0) {
        if (argc > 2) {
            solar_os_shell_diag_unexpected(term, "log clear", argv[2], "log clear");
            return;
        }
        const esp_err_t err = solar_os_log_clear();
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log clear failed: %s\n", solar_os_shell_error_text(err));
            return;
        }
        solar_os_shell_io_writeln(term, "log: cleared");
        return;
    }

    if (strcmp(argv[1], "level") == 0) {
        solar_os_log_status_t status;
        if (argc == 2) {
            const esp_err_t err = solar_os_log_get_status(&status);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term, "log level failed: %s\n", solar_os_shell_error_text(err));
                return;
            }
            solar_os_shell_io_printf(term, "level: %s\n", solar_os_log_level_name(status.level));
            return;
        }
        if (argc != 3) {
            solar_os_shell_diag_unexpected(term, "log level", argv[3],
                                           "log level [error|warn|info|debug]");
            return;
        }

        solar_os_log_level_t level;
        if (!solar_os_log_parse_level(argv[2], &level)) {
            solar_os_shell_diag_invalid(term, "log level", "level", argv[2],
                                        "error, warn, info, or debug",
                                        "log level [error|warn|info|debug]", false);
            return;
        }

        const esp_err_t err = solar_os_log_set_level(level);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log level failed: %s\n", solar_os_shell_error_text(err));
            return;
        }
        solar_os_shell_io_printf(term, "level: %s\n", solar_os_log_level_name(level));
        return;
    }

    if (strcmp(argv[1], "sink") == 0) {
        if (argc == 3 && strcmp(argv[2], "cdc") == 0) {
            solar_os_log_status_t status;
            const esp_err_t err = solar_os_log_get_status(&status);
            if (err != ESP_OK) {
                solar_os_shell_io_printf(term, "log sink failed: %s\n", solar_os_shell_error_text(err));
                return;
            }
            solar_os_shell_io_printf(term, "cdc: %s\n", status.cdc_enabled ? "on" : "off");
            return;
        }
        if (argc != 4 || strcmp(argv[2], "cdc") != 0) {
            if (argc < 3) {
                solar_os_shell_diag_missing(term, "log sink", "<sink>",
                                            "log sink cdc [on|off]");
            } else if (strcmp(argv[2], "cdc") != 0) {
                solar_os_shell_diag_invalid(term, "log sink", "sink", argv[2], "cdc",
                                            "log sink cdc [on|off]", false);
            } else if (argc < 4) {
                solar_os_shell_diag_missing(term, "log sink cdc", "<state>",
                                            "log sink cdc [on|off]");
            } else {
                solar_os_shell_diag_unexpected(term, "log sink cdc", argv[4],
                                               "log sink cdc [on|off]");
            }
            return;
        }

        bool enabled = false;
        if (!parse_on_off_arg(argv[3], &enabled)) {
            solar_os_shell_diag_invalid(term, "log sink cdc", "state", argv[3],
                                        "on or off", "log sink cdc [on|off]", false);
            return;
        }

        const esp_err_t err = solar_os_log_set_cdc_enabled(enabled);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log sink failed: %s\n", solar_os_shell_error_text(err));
            return;
        }
        solar_os_shell_io_printf(term, "cdc: %s\n", enabled ? "on" : "off");
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "log",
                                   argc,
                                   argv,
                                   "log status|show|follow|clear|level|sink",
                                   log_commands,
                                   sizeof(log_commands) / sizeof(log_commands[0]));
}

static void port_print_info(solar_os_shell_io_t *term, const solar_os_port_info_t *info)
{
    char caps[4];

    solar_os_shell_io_printf(term,
                             "%-8s %-3s %-10s %s\n",
                             info->name,
                             solar_os_port_capabilities_text(info->capabilities,
                                                             caps,
                                                             sizeof(caps)),
                             info->claimed ? info->owner : "-",
                             info->label);
}

static void port_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  port list");
    solar_os_shell_io_writeln(term, "  port status <name>");
}

void solar_os_shell_cmd_port(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc > 2 && strcmp(argv[1], "list") == 0) {
        solar_os_shell_diag_unexpected(term, "port list", argv[2], "port list");
        return;
    }

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_port_info_t ports[PORT_LIST_MAX];
        const size_t count = solar_os_port_list(ports, PORT_LIST_MAX);
        if (count == 0) {
            solar_os_shell_io_writeln(term, "ports: none");
            return;
        }

        solar_os_shell_io_writeln(term, "name     cap owner      label");
        const size_t printed = count < PORT_LIST_MAX ? count : PORT_LIST_MAX;
        for (size_t i = 0; i < printed; i++) {
            port_print_info(term, &ports[i]);
        }
        if (count > printed) {
            solar_os_shell_io_printf(term, "... %u more\n", (unsigned)(count - printed));
        }
        return;
    }

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        solar_os_shell_diag_missing(term, "port status", "<name>",
                                    "port status <name>");
        return;
    }
    if (argc > 3 && strcmp(argv[1], "status") == 0) {
        solar_os_shell_diag_unexpected(term, "port status", argv[3],
                                       "port status <name>");
        return;
    }

    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_port_info_t info;
        const esp_err_t err = solar_os_port_get_info(argv[2], &info);
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "port: not found: %s\n", argv[2]);
            return;
        }
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "port status failed: %s\n", solar_os_shell_error_text(err));
            return;
        }

        solar_os_shell_io_writeln(term, "name     cap owner      label");
        port_print_info(term, &info);
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "port",
                                   argc,
                                   argv,
                                   "port list|status",
                                   port_commands,
                                   sizeof(port_commands) / sizeof(port_commands[0]));
}

typedef struct {
    solar_os_shell_io_t *term;
} xfer_shell_state_t;

typedef struct {
    const char *direction;
    const char *port_name;
    const char *path_arg;
    solar_os_transfer_protocol_t protocol;
    uint32_t delay_ms;
    uint32_t idle_ms;
    bool append;
} xfer_command_config_t;

static void xfer_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  xfer protocols");
    solar_os_shell_io_writeln(term, "  xfer send <port> <file> --raw [-d ms]");
    solar_os_shell_io_writeln(term, "  xfer recv <port> <file> --raw [--append|--replace] [--idle-ms ms]");
    solar_os_shell_io_writeln(term, "  xfer send <port> <file> --zmodem");
    solar_os_shell_io_writeln(term, "  xfer recv <port> <file> --zmodem [--append|--replace]");
    solar_os_shell_io_writeln(term, "protocols: raw and zmodem are supported; kermit is reserved");
}

static void xfer_print_protocols(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "raw     supported");
    solar_os_shell_io_writeln(term, "zmodem  supported");
    solar_os_shell_io_writeln(term, "kermit  not implemented");
}

static bool xfer_is_send(const char *direction)
{
    return strcmp(direction, "send") == 0;
}

static bool xfer_is_recv(const char *direction)
{
    return strcmp(direction, "recv") == 0;
}

static bool xfer_parse_args(solar_os_shell_io_t *term,
                            int argc,
                            char **argv,
                            xfer_command_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->protocol = SOLAR_OS_TRANSFER_PROTOCOL_RAW;

    if (argc < 2) {
        solar_os_shell_diag_missing(term, "xfer", "subcommand",
                                    "xfer <protocols|send|recv> ...");
        return false;
    }

    config->direction = argv[1];
    if (strcmp(argv[1], "protocols") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "xfer protocols", argv[2], "xfer protocols");
            return false;
        }
        return true;
    }

    if (!xfer_is_send(config->direction) && !xfer_is_recv(config->direction)) {
        solar_os_shell_diag_unknown(term, "xfer", "subcommand", argv[1],
                                    solar_os_shell_suggest(argv[1], xfer_commands,
                                                           sizeof(xfer_commands) / sizeof(xfer_commands[0])),
                                    "xfer <protocols|send|recv> ...");
        return false;
    }
    if (argc < 4) {
        solar_os_shell_diag_missing(term, "xfer", argc < 3 ? "port" : "file",
                                    "xfer <send|recv> <port> <file> [options]");
        return false;
    }

    config->port_name = argv[2];
    config->path_arg = argv[3];

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            config->protocol = SOLAR_OS_TRANSFER_PROTOCOL_RAW;
        } else if (strcmp(argv[i], "--zmodem") == 0) {
            config->protocol = SOLAR_OS_TRANSFER_PROTOCOL_ZMODEM;
        } else if (strcmp(argv[i], "--kermit") == 0) {
            config->protocol = SOLAR_OS_TRANSFER_PROTOCOL_KERMIT;
        } else if (strcmp(argv[i], "--protocol") == 0) {
            if (i + 1 >= argc ||
                !solar_os_transfer_parse_protocol(argv[++i], &config->protocol)) {
                solar_os_shell_io_writeln(term, "xfer protocol: raw zmodem kermit");
                return false;
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delay-ms") == 0) {
            size_t parsed = 0;
            if (i + 1 >= argc || !parse_size_arg(argv[++i], 0, XFER_DELAY_MAX_MS, &parsed)) {
                solar_os_shell_io_printf(term,
                                         "xfer delay: 0..%u ms\n",
                                         (unsigned)XFER_DELAY_MAX_MS);
                return false;
            }
            config->delay_ms = (uint32_t)parsed;
        } else if (strcmp(argv[i], "--idle-ms") == 0) {
            size_t parsed = 0;
            if (i + 1 >= argc || !parse_size_arg(argv[++i], 0, XFER_IDLE_MAX_MS, &parsed)) {
                solar_os_shell_io_printf(term,
                                         "xfer idle timeout: 0..%u ms\n",
                                         (unsigned)XFER_IDLE_MAX_MS);
                return false;
            }
            config->idle_ms = (uint32_t)parsed;
        } else if (strcmp(argv[i], "--append") == 0) {
            config->append = true;
        } else if (strcmp(argv[i], "--replace") == 0) {
            config->append = false;
        } else {
            solar_os_shell_diag_unknown(term, "xfer", "option", argv[i],
                                        solar_os_shell_suggest(argv[i], xfer_options,
                                                               sizeof(xfer_options) / sizeof(xfer_options[0])),
                                        "xfer <send|recv> <port> <file> [options]");
            return false;
        }
    }

    return true;
}

static bool xfer_read_cancel_key(void *user)
{
    xfer_shell_state_t *state = (xfer_shell_state_t *)user;
    char chars[8];
    size_t count;

#if SOLAR_OS_PACKAGE_SERVICE_BLE
    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            if ((uint8_t)chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    }
#else
    (void)chars;
    (void)count;
#endif

    solar_os_shell_io_t *term = state != NULL ? state->term : NULL;
    if (term == NULL ||
        solar_os_shell_io_kind(term) != SOLAR_OS_SHELL_IO_KIND_PORT ||
        !solar_os_port_handle_valid(&term->port)) {
        return false;
    }

    uint8_t port_chars[8];
    do {
        count = 0;
        if (solar_os_port_read(&term->port,
                               port_chars,
                               sizeof(port_chars),
                               0,
                               &count) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (port_chars[i] == 0x1d || port_chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    } while (count > 0);

    return false;
}

static void xfer_progress(uint64_t bytes, void *user)
{
    xfer_shell_state_t *state = (xfer_shell_state_t *)user;
    if (state == NULL || state->term == NULL) {
        return;
    }
    solar_os_shell_io_printf(state->term, "xfer: %" PRIu64 " bytes\n", bytes);
    solar_os_shell_io_flush(state->term);
}

static void xfer_print_error(solar_os_shell_io_t *term,
                             const xfer_command_config_t *config,
                             esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE && config != NULL && config->port_name != NULL) {
        solar_os_port_info_t info;
        if (solar_os_port_get_info(config->port_name, &info) == ESP_OK && info.claimed) {
            solar_os_shell_io_printf(term,
                                     "xfer: port %s owned by %s\n",
                                     config->port_name,
                                     info.owner);
            return;
        }
    }

    if (err == ESP_ERR_NOT_FOUND && config != NULL && config->port_name != NULL) {
        solar_os_port_info_t info;
        if (solar_os_port_get_info(config->port_name, &info) == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "xfer: port not found: %s\n", config->port_name);
            return;
        }
        solar_os_shell_io_printf(term, "xfer: file not found: %s\n", config->path_arg);
        return;
    }

    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "xfer: protocol or port capability not supported");
        return;
    }

    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_diag_problem(term, "xfer", "invalid transfer arguments",
                                    "xfer <send|recv> <port> <file> [options]", NULL);
        return;
    }

    solar_os_shell_io_printf(term, "xfer failed: %s\n", solar_os_shell_error_text(err));
}

void solar_os_shell_cmd_xfer(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    xfer_command_config_t config;

    if (!xfer_parse_args(term, argc, argv, &config)) {
        return;
    }

    if (strcmp(config.direction, "protocols") == 0) {
        xfer_print_protocols(term);
        return;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const esp_err_t path_err =
        solar_os_shell_resolve_path(ctx, config.path_arg, path, sizeof(path));
    if (path_err != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "xfer: %s: %s\n",
                                 path_err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path",
                                 config.path_arg);
        return;
    }

    xfer_shell_state_t state = {
        .term = term,
    };
    solar_os_transfer_options_t options = {
        .port_name = config.port_name,
        .path = path,
        .protocol = config.protocol,
        .char_delay_ms = config.delay_ms,
        .idle_timeout_ms = config.idle_ms,
        .append = config.append,
        .should_cancel = xfer_read_cancel_key,
        .progress = xfer_progress,
        .user = &state,
    };
    solar_os_transfer_result_t result;

    solar_os_shell_io_printf(term,
                             "xfer %s %s %s, %s stops\n",
                             config.direction,
                             config.port_name,
                             config.path_arg,
                             solar_os_shell_io_app_exit_key(term));
    solar_os_shell_io_flush(term);

    const esp_err_t err = xfer_is_send(config.direction) ?
        solar_os_transfer_send(&options, &result) :
        solar_os_transfer_recv(&options, &result);
    if (err != ESP_OK) {
        xfer_print_error(term, &config, err);
        return;
    }

    if (result.cancelled) {
        solar_os_shell_io_printf(term, "xfer: stopped after %" PRIu64 " bytes\n", result.bytes);
    } else if (result.idle_timeout) {
        solar_os_shell_io_printf(term, "xfer: idle after %" PRIu64 " bytes\n", result.bytes);
    } else {
        solar_os_shell_io_printf(term, "xfer: done, %" PRIu64 " bytes\n", result.bytes);
    }
}



#if SOLAR_OS_PACKAGE_SERVICE_SSH
static void sshkey_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  sshkey [status]");
    solar_os_shell_io_writeln(term, "  sshkey gen [-f] [2048|3072|4096]");
    solar_os_shell_io_writeln(term, "  sshkey pub");
    solar_os_shell_io_writeln(term, "  sshkey rm");
}

static void sshkey_print_status(solar_os_shell_io_t *term)
{
    solar_os_ssh_key_status_t status;
    const esp_err_t err = solar_os_ssh_keys_get_status(&status);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "sshkey: storage is not mounted");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "sshkey: status failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term, "private: %s", status.private_key_path);
    if (status.private_key_exists) {
        solar_os_shell_io_printf(term, " (%" PRIu32 " bytes)\n", status.private_key_size);
    } else {
        solar_os_shell_io_writeln(term, " (missing)");
    }

    solar_os_shell_io_printf(term, "public:  %s", status.public_key_path);
    if (status.public_key_exists) {
        solar_os_shell_io_printf(term, " (%" PRIu32 " bytes)\n", status.public_key_size);
    } else {
        solar_os_shell_io_writeln(term, " (missing)");
    }
}

static void sshkey_print_public(solar_os_shell_io_t *term)
{
    solar_os_ssh_key_status_t status;
    esp_err_t err = solar_os_ssh_keys_get_status(&status);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "sshkey: storage is not mounted");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "sshkey: status failed: %s\n", solar_os_shell_error_text(err));
        return;
    }
    if (!status.public_key_exists) {
        solar_os_shell_io_writeln(term, "sshkey: public key is missing");
        return;
    }

    FILE *file = fopen(status.public_key_path, "r");
    if (file == NULL) {
        solar_os_shell_io_printf(term,
                                 "sshkey: cannot open public key: %s\n",
                                 strerror(errno));
        return;
    }

    char line[128];
    bool ended_with_newline = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        ended_with_newline = line[strlen(line) - 1] == '\n';
        solar_os_shell_io_write(term, line);
    }
    fclose(file);

    if (!ended_with_newline) {
        solar_os_shell_io_put_char(term, '\n');
    }
}

void solar_os_shell_cmd_sshkey(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        sshkey_print_status(term);
        return;
    }

    if (strcmp(argv[1], "pub") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "sshkey pub", argv[2], "sshkey pub");
            return;
        }
        sshkey_print_public(term);
        return;
    }

    if (strcmp(argv[1], "rm") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "sshkey rm", argv[2], "sshkey rm");
            return;
        }

        const esp_err_t err = solar_os_ssh_keys_remove_default();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "sshkey: removed");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "sshkey: no key to remove");
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "sshkey: storage is not mounted");
        } else {
            solar_os_shell_io_printf(term, "sshkey: remove failed: %s\n", solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "gen") == 0) {
        bool overwrite = false;
        size_t bits = SOLAR_OS_SSH_KEY_DEFAULT_BITS;

        if (!solar_os_storage_is_mounted()) {
            solar_os_shell_io_writeln(term, "sshkey: storage is not mounted");
            return;
        }

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-f") == 0) {
                overwrite = true;
                continue;
            }
            if (!parse_size_arg(argv[i],
                                SOLAR_OS_SSH_KEY_MIN_BITS,
                                SOLAR_OS_SSH_KEY_MAX_BITS,
                                &bits) ||
                bits % 1024U != 0) {
                solar_os_shell_diag_invalid(term, "sshkey gen", "option or key size", argv[i],
                                            "-f, 2048, 3072, or 4096",
                                            "sshkey gen [-f] [2048|3072|4096]", false);
                return;
            }
        }

        solar_os_shell_io_printf(term, "sshkey: generating RSA-%u key...\n", (unsigned)bits);
        solar_os_shell_io_flush(term);

        const esp_err_t err = solar_os_ssh_keys_generate_rsa((uint32_t)bits, overwrite);
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "sshkey: generated /.ssh/id_rsa");
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "sshkey: key exists; use sshkey gen -f to replace");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "sshkey: RSA bits must be 2048, 3072, or 4096");
        } else {
            solar_os_shell_io_printf(term, "sshkey: generate failed: %s\n", solar_os_shell_error_text(err));
        }
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "sshkey",
                                   argc,
                                   argv,
                                   "sshkey status|gen|pub|rm",
                                   sshkey_commands,
                                   sizeof(sshkey_commands) / sizeof(sshkey_commands[0]));
}
#endif
