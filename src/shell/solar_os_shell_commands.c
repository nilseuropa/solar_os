#include "solar_os_shell_commands.h"
#include "solar_os_shell_io.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "solar_os_app_registry.h"
#include "solar_os_adc.h"
#include "solar_os_audio.h"
#include "solar_os_battery.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_daq_job.h"
#include "solar_os_gpio.h"
#include "solar_os_i2c.h"
#include "solar_os_identity.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_mqtt.h"
#include "solar_os_net.h"
#endif
#include "solar_os_ota.h"
#include "solar_os_port.h"
#include "solar_os_power.h"
#include "solar_os_pwm.h"
#include "solar_os_sensors.h"
#include "solar_os_shell.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_ssh_keys.h"
#endif
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_keys.h"
#include "solar_os_terminal.h"
#include "solar_os_time.h"
#include "solar_os_transfer.h"
#include "solar_os_tui.h"
#include "solar_os_uart.h"
#include "solar_os_wifi.h"

#define SOLAR_OS_SHELL_ARG_MAX 20
#define I2C_READ_MAX_LEN 32
#define UART_READ_MAX_LEN 96
#define UART_WRITE_MAX_LEN 128
#define XFER_DELAY_MAX_MS 60000U
#define XFER_IDLE_MAX_MS 86400000U
#define PORT_LIST_MAX SOLAR_OS_PORT_MAX
#define LOG_SHOW_DEFAULT 40
#define SETTERM_TUI_EDIT_MAX SOLAR_OS_OTA_URL_MAX
#define SETTERM_TUI_CURSOR_BLINK_MS 500
#define WIFI_TUI_STATUS_MAX 96
#define WIFI_TUI_REFRESH_MS 1000
#define OTA_PROGRESS_BAR_WIDTH 24
#define OTA_PROGRESS_STEP_BYTES (64U * 1024U)
#define NETSCAN_MAX_PORTS 128
#define NETSCAN_MAX_HOSTS 256
#define NETSCAN_TIMEOUT_MS 350U

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

static solar_os_shell_io_t shell_command_fallback_io;

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&shell_command_fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &shell_command_fallback_io);
        io = &shell_command_fallback_io;
    }
    return io;
}

static solar_os_terminal_t *display_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

static bool shell_print_not_supported(solar_os_shell_io_t *term,
                                      const char *command,
                                      const char *feature,
                                      esp_err_t err)
{
    if (err != ESP_ERR_NOT_SUPPORTED) {
        return false;
    }

    solar_os_shell_io_printf(term,
                             "%s: %s not available on this board\n",
                             command,
                             feature);
    return true;
}

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

static void audio_print_gain(solar_os_shell_io_t *term, float gain_db);

static bool parse_u8(const char *text, uint8_t *value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > 0xff) {
        return false;
    }

    *value = (uint8_t)parsed;
    return true;
}

static bool parse_size_arg(const char *text, size_t min, size_t max, size_t *value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *value = (size_t)parsed;
    return true;
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
    solar_os_shell_io_printf(term, "Packages: %s\n", SOLAR_OS_PACKAGE_LIST);
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
    pkg_print_wrapped_list(term, "Build units", SOLAR_OS_PACKAGE_LIST);
}

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
        solar_os_shell_io_printf(term, "ota: status failed: %s\n", esp_err_to_name(err));
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

typedef struct {
    solar_os_shell_io_t *term;
    size_t row;
    bool row_valid;
    solar_os_ota_progress_stage_t last_stage;
    uint8_t last_percent;
    uint32_t next_bytes;
    bool last_known;
} ota_shell_progress_t;

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

static bool ota_parse_slot(const char *text, uint8_t *slot)
{
    if (text == NULL || slot == NULL) {
        return false;
    }

    size_t parsed = 0;
    if (!parse_size_arg(text, 0, 15, &parsed)) {
        return false;
    }

    *slot = (uint8_t)parsed;
    return true;
}

void solar_os_shell_cmd_ota(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        ota_print_usage(term);
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: ota status");
            return;
        }
        ota_print_status(term);
        return;
    }

    if (strcmp(argv[1], "check") == 0) {
        solar_os_ota_check_result_t result;

        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: ota check");
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
        } else {
            solar_os_shell_io_printf(term,
                                     "ota: check failed: %s",
                                     esp_err_to_name(err));
            if (result.status_code > 0) {
                solar_os_shell_io_printf(term, " HTTP %d", result.status_code);
            }
            solar_os_shell_io_put_char(term, '\n');
        }
        return;
    }

    if (strcmp(argv[1], "upgrade") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: ota upgrade");
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
        const esp_err_t err = solar_os_ota_upgrade(ota_shell_progress_cb, &progress);
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
                                     esp_err_to_name(err));
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
            solar_os_shell_io_writeln(term, "usage: ota url [url]");
            return;
        }

        const esp_err_t err = solar_os_ota_set_url(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "ota url: %s\n", argv[2]);
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_printf(term, "ota url: invalid URL: %s\n", argv[2]);
        } else {
            solar_os_shell_io_printf(term, "ota url: save failed: %s\n", esp_err_to_name(err));
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
                                         esp_err_to_name(err));
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
            solar_os_shell_io_writeln(term, "usage: ota flavor [flavor]");
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
                                     esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "boot") == 0) {
        uint8_t slot = 0;
        if (argc != 3 || !ota_parse_slot(argv[2], &slot)) {
            solar_os_shell_io_writeln(term, "usage: ota boot 0|1");
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
                                     esp_err_to_name(err));
        }
        return;
    }

    ota_print_usage(term);
}

void solar_os_shell_cmd_apps(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: apps");
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

static void job_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  jobs");
    solar_os_shell_io_writeln(term, "  job status [name]");
    solar_os_shell_io_writeln(term, "  job start <name> [args...]");
    solar_os_shell_io_writeln(term, "  job stop <name>");
}

static void job_print_status(solar_os_shell_io_t *term, const solar_os_job_status_t *status)
{
    if (status == NULL) {
        return;
    }

    solar_os_shell_io_printf(term,
                             "%-12s %-8s %5u %s\n",
                             status->name != NULL ? status->name : "?",
                             solar_os_job_state_name(status->state),
                             (unsigned)status->tick_count,
                             status->summary != NULL ? status->summary : "job");
    if (status->state == SOLAR_OS_JOB_FAILED && status->last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "  last error: %s\n",
                                 esp_err_to_name(status->last_error));
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

    if (strcmp(argv[2], "shell") == 0) {
        *port_name = argv[3];
        *required_caps = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE;
        return true;
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
            solar_os_jobs_get_by_name(info.owner, &owner_job);
        solar_os_shell_io_printf(term,
                                 "job start failed: %s%s owns %s\n",
                                 info.owner[0] != '\0' ? info.owner : "another owner",
                                 owner_is_job ? " job" : "",
                                 info.name);
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

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: jobs");
        return;
    }

    const size_t count = solar_os_jobs_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "no jobs registered");
        return;
    }

    solar_os_shell_io_writeln(term, "NAME         STATE    TICKS SUMMARY");
    for (size_t i = 0; i < count; i++) {
        solar_os_job_status_t status;
        if (solar_os_jobs_get(i, &status)) {
            job_print_status(term, &status);
        }
    }
}

void solar_os_shell_cmd_job(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        job_print_usage(term);
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (argc == 2) {
            solar_os_shell_cmd_jobs(ctx, 1, argv);
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: job status [name]");
            return;
        }

        solar_os_job_status_t status;
        if (!solar_os_jobs_get_by_name(argv[2], &status)) {
            solar_os_shell_io_printf(term, "job: not found: %s\n", argv[2]);
            return;
        }
        solar_os_shell_io_writeln(term, "NAME         STATE    TICKS SUMMARY");
        job_print_status(term, &status);
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) {
            solar_os_shell_io_writeln(term, "usage: job start <name> [args...]");
            return;
        }

        const esp_err_t err = solar_os_jobs_start(ctx, argv[2], argc - 2, &argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(term, "job started: %s\n", argv[2]);
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
            solar_os_shell_io_printf(term,
                                     "job start failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: job stop <name>");
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
                                     esp_err_to_name(err));
        }
        return;
    }

    job_print_usage(term);
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
            if (!parse_size_arg(argv[2], 1, 86400, &seconds)) {
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

static void setterm_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  setterm");
    solar_os_shell_io_writeln(term, "  setterm orientation [0|90|180|270]");
    solar_os_shell_io_writeln(term, "  setterm font [mono|compact]");
    solar_os_shell_io_writeln(term, "  setterm textsize [12|14|16|18|20]");
    solar_os_shell_io_writeln(term, "  setterm keyboard [us|de]");
    solar_os_shell_io_writeln(term, "  setterm keyrate [off|1..60 [delay-ms]]");
    solar_os_shell_io_writeln(term, "  setterm timezone [UTC|Europe/Berlin|POSIX-TZ]");
    solar_os_shell_io_writeln(term, "  setterm otaurl [url]");
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
                                 esp_err_to_name(err));
    }
}

static void setterm_print_keyrate(solar_os_shell_io_t *term)
{
    uint16_t keyrate = 0;
    uint16_t keydelay_ms = 0;

    solar_os_ble_keyboard_get_repeat(&keyrate, &keydelay_ms);
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
                                 esp_err_to_name(err));
    }
}

typedef enum {
    SETTERM_TUI_ORIENTATION,
    SETTERM_TUI_FONT,
    SETTERM_TUI_TEXTSIZE,
    SETTERM_TUI_KEYBOARD,
    SETTERM_TUI_KEYRATE,
    SETTERM_TUI_TIMEZONE,
    SETTERM_TUI_OTAURL,
    SETTERM_TUI_ITEM_COUNT,
} setterm_tui_item_t;

typedef struct {
    const char *label;
} setterm_tui_item_def_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected;
    bool editing;
    bool cursor_visible;
    uint32_t last_cursor_blink_ms;
    char edit_text[SETTERM_TUI_EDIT_MAX];
    char original_text[SETTERM_TUI_EDIT_MAX];
    char status[64];
} setterm_tui_state_t;

static setterm_tui_state_t setterm_tui;

static const setterm_tui_item_def_t setterm_tui_items[] = {
    [SETTERM_TUI_ORIENTATION] = {.label = "orientation"},
    [SETTERM_TUI_FONT] = {.label = "font"},
    [SETTERM_TUI_TEXTSIZE] = {.label = "textsize"},
    [SETTERM_TUI_KEYBOARD] = {.label = "keyboard"},
    [SETTERM_TUI_KEYRATE] = {.label = "keyrate"},
    [SETTERM_TUI_TIMEZONE] = {.label = "timezone"},
    [SETTERM_TUI_OTAURL] = {.label = "otaurl"},
};

static size_t setterm_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void setterm_tui_write_cell(size_t row,
                                   size_t col,
                                   size_t width,
                                   const char *text,
                                   uint8_t attr)
{
    char clipped[SETTERM_TUI_EDIT_MAX];
    size_t len = 0;

    if (width == 0) {
        return;
    }

    solar_os_tui_fill(&setterm_tui.tui, row, col, 1, width, ' ', attr);

    if (text == NULL || text[0] == '\0') {
        return;
    }

    while (text[len] != '\0' && len + 1 < sizeof(clipped) && len < width) {
        clipped[len] = text[len];
        len++;
    }
    clipped[len] = '\0';
    solar_os_tui_addstr(&setterm_tui.tui, row, col, clipped, attr);
}

static void setterm_tui_current_value(setterm_tui_item_t item, char *buffer, size_t buffer_len)
{
    solar_os_terminal_t *term = display_terminal(setterm_tui.ctx);

    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    switch (item) {
    case SETTERM_TUI_ORIENTATION:
        snprintf(buffer, buffer_len, "%u", (unsigned)solar_os_terminal_orientation(term));
        break;
    case SETTERM_TUI_FONT:
        strlcpy(buffer,
                solar_os_terminal_font_name(solar_os_terminal_font(term)),
                buffer_len);
        break;
    case SETTERM_TUI_TEXTSIZE:
        strlcpy(buffer,
                solar_os_terminal_text_size_name(solar_os_terminal_text_size(term)),
                buffer_len);
        break;
    case SETTERM_TUI_KEYBOARD:
        strlcpy(buffer,
                solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout()),
                buffer_len);
        break;
    case SETTERM_TUI_KEYRATE: {
        uint16_t rate = 0;
        uint16_t delay_ms = 0;
        solar_os_ble_keyboard_get_repeat(&rate, &delay_ms);
        if (rate == 0) {
            strlcpy(buffer, "off", buffer_len);
        } else {
            snprintf(buffer, buffer_len, "%u %u", (unsigned)rate, (unsigned)delay_ms);
        }
        break;
    }
    case SETTERM_TUI_TIMEZONE:
        solar_os_time_get_timezone(buffer, buffer_len, NULL, 0);
        break;
    case SETTERM_TUI_OTAURL:
        solar_os_ota_get_url(buffer, buffer_len);
        break;
    default:
        strlcpy(buffer, "-", buffer_len);
        break;
    }
}

static void setterm_tui_set_status(const char *status)
{
    strlcpy(setterm_tui.status, status != NULL ? status : "", sizeof(setterm_tui.status));
}

static void setterm_tui_render(void)
{
    solar_os_tui_t *tui = &setterm_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_tui_clear(tui);

    size_t split = cols / 2;
    if (cols >= 24 && split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    setterm_tui_write_cell(0,
                           0,
                           split,
                           "parameter",
                           SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        setterm_tui_write_cell(0,
                               split + 1,
                               setterm_tui_visible_width(cols, split + 1),
                               "value",
                               SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t value_col = split + 1;
    const size_t value_width = setterm_tui_visible_width(cols, value_col);
    for (size_t i = 0; i < SETTERM_TUI_ITEM_COUNT && i + 1 < rows; i++) {
        char value[SETTERM_TUI_EDIT_MAX];
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (i == setterm_tui.selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }

        setterm_tui_current_value((setterm_tui_item_t)i, value, sizeof(value));
        if (setterm_tui.editing && i == setterm_tui.selected) {
            strlcpy(value, setterm_tui.edit_text, sizeof(value));
            value_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
        }

        setterm_tui_write_cell(i + 1,
                               0,
                               split,
                               setterm_tui_items[i].label,
                               label_attr);
        if (value_width > 0) {
            setterm_tui_write_cell(i + 1, value_col, value_width, value, value_attr);
        }
    }

    if (setterm_tui.status[0] != '\0' && rows > 1) {
        setterm_tui_write_cell(rows - 1,
                               0,
                               cols,
                               setterm_tui.status,
                               SOLAR_OS_TUI_ATTR_INVERSE);
    }

    if (setterm_tui.editing && value_width > 0) {
        const size_t edit_len = strlen(setterm_tui.edit_text);
        size_t cursor_col = value_col + edit_len;
        if (cursor_col >= cols) {
            cursor_col = cols - 1;
        }
        solar_os_tui_move(tui, setterm_tui.selected + 1, cursor_col);
    }

    solar_os_terminal_set_cursor_visible(tui->terminal,
                                         setterm_tui.editing && setterm_tui.cursor_visible);
    solar_os_tui_refresh(tui);
}

static bool setterm_tui_cycle_value(const char * const *values,
                                    size_t count,
                                    int direction)
{
    size_t index = 0;

    if (values == NULL || count == 0) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(setterm_tui.edit_text, values[i]) == 0) {
            index = i;
            break;
        }
    }

    if (direction > 0) {
        index = (index + 1) % count;
    } else {
        index = index == 0 ? count - 1 : index - 1;
    }
    strlcpy(setterm_tui.edit_text, values[index], sizeof(setterm_tui.edit_text));
    return true;
}

static bool setterm_tui_cycle_selected(int direction)
{
    static const char * const orientation_values[] = {"0", "90", "180", "270"};
    static const char * const font_values[] = {"mono", "compact"};
    static const char * const textsize_values[] = {"12", "14", "16", "18", "20"};
    static const char * const keyboard_values[] = {"us", "de"};

    switch ((setterm_tui_item_t)setterm_tui.selected) {
    case SETTERM_TUI_ORIENTATION:
        return setterm_tui_cycle_value(orientation_values,
                                       sizeof(orientation_values) / sizeof(orientation_values[0]),
                                       direction);
    case SETTERM_TUI_FONT:
        return setterm_tui_cycle_value(font_values,
                                       sizeof(font_values) / sizeof(font_values[0]),
                                       direction);
    case SETTERM_TUI_TEXTSIZE:
        return setterm_tui_cycle_value(textsize_values,
                                       sizeof(textsize_values) / sizeof(textsize_values[0]),
                                       direction);
    case SETTERM_TUI_KEYBOARD:
        return setterm_tui_cycle_value(keyboard_values,
                                       sizeof(keyboard_values) / sizeof(keyboard_values[0]),
                                       direction);
    default:
        return false;
    }
}

static void setterm_tui_reset_cursor_blink(void)
{
    setterm_tui.cursor_visible = true;
    setterm_tui.last_cursor_blink_ms = 0;
}

static int setterm_tui_tokenize(char *line, char **argv, int argv_max)
{
    int argc = 0;
    char *saveptr = NULL;
    char *token = NULL;

    if (line == NULL || argv == NULL || argv_max <= 0) {
        return 0;
    }

    token = strtok_r(line, " \t", &saveptr);
    while (token != NULL && argc < argv_max) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    return argc;
}

static bool setterm_tui_apply_keyrate(void)
{
    char text[SETTERM_TUI_EDIT_MAX];
    char *argv[2];
    uint16_t current_delay_ms = 0;

    strlcpy(text, setterm_tui.edit_text, sizeof(text));
    const int argc = setterm_tui_tokenize(text, argv, 2);
    if (argc < 1) {
        return false;
    }

    solar_os_ble_keyboard_get_repeat(NULL, &current_delay_ms);
    if (strcmp(argv[0], "off") == 0) {
        return argc == 1 &&
            solar_os_ble_keyboard_set_repeat(0, current_delay_ms) == ESP_OK;
    }

    size_t rate = 0;
    size_t delay_ms = current_delay_ms;
    if (!parse_size_arg(argv[0],
                        SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN,
                        SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX,
                        &rate)) {
        return false;
    }
    if (argc == 2 &&
        !parse_size_arg(argv[1],
                        SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS,
                        SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS,
                        &delay_ms)) {
        return false;
    }

    return solar_os_ble_keyboard_set_repeat((uint16_t)rate, (uint16_t)delay_ms) == ESP_OK;
}

static bool setterm_tui_apply_selected(void)
{
    solar_os_terminal_t *term = display_terminal(setterm_tui.ctx);

    switch ((setterm_tui_item_t)setterm_tui.selected) {
    case SETTERM_TUI_ORIENTATION: {
        size_t degrees = 0;
        if (!parse_size_arg(setterm_tui.edit_text, 0, 270, &degrees) ||
            !(degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270)) {
            return false;
        }
        return solar_os_terminal_set_orientation(term, (uint16_t)degrees) == ESP_OK;
    }
    case SETTERM_TUI_FONT: {
        solar_os_terminal_font_t font;
        return solar_os_terminal_parse_font(setterm_tui.edit_text, &font) &&
            solar_os_terminal_set_font(term, font) == ESP_OK;
    }
    case SETTERM_TUI_TEXTSIZE: {
        solar_os_terminal_text_size_t text_size;
        return solar_os_terminal_parse_text_size(setterm_tui.edit_text, &text_size) &&
            solar_os_terminal_set_text_size(term, text_size) == ESP_OK;
    }
    case SETTERM_TUI_KEYBOARD: {
        solar_os_ble_keyboard_layout_t layout;
        return solar_os_ble_keyboard_parse_layout(setterm_tui.edit_text, &layout) &&
            solar_os_ble_keyboard_set_layout(layout) == ESP_OK;
    }
    case SETTERM_TUI_KEYRATE:
        return setterm_tui_apply_keyrate();
    case SETTERM_TUI_TIMEZONE:
        return solar_os_time_set_timezone(setterm_tui.edit_text) == ESP_OK;
    case SETTERM_TUI_OTAURL:
        return solar_os_ota_set_url(setterm_tui.edit_text) == ESP_OK;
    default:
        return false;
    }
}

static void setterm_tui_begin_edit(void)
{
    setterm_tui_current_value((setterm_tui_item_t)setterm_tui.selected,
                              setterm_tui.edit_text,
                              sizeof(setterm_tui.edit_text));
    strlcpy(setterm_tui.original_text,
            setterm_tui.edit_text,
            sizeof(setterm_tui.original_text));
    setterm_tui.editing = true;
    setterm_tui_reset_cursor_blink();
    setterm_tui_set_status("");
    setterm_tui_render();
}

static void setterm_tui_commit_edit(void)
{
    if (setterm_tui_apply_selected()) {
        setterm_tui.editing = false;
        setterm_tui.cursor_visible = false;
        setterm_tui_set_status("saved");
    } else {
        setterm_tui_reset_cursor_blink();
        setterm_tui_set_status("invalid value");
    }
    setterm_tui_render();
}

static void setterm_tui_cancel_edit(void)
{
    strlcpy(setterm_tui.edit_text,
            setterm_tui.original_text,
            sizeof(setterm_tui.edit_text));
    setterm_tui.editing = false;
    setterm_tui.cursor_visible = false;
    setterm_tui_set_status("");
    setterm_tui_render();
}

static void setterm_tui_handle_edit_key(char ch)
{
    const uint8_t key = (uint8_t)ch;
    const size_t len = strlen(setterm_tui.edit_text);

    setterm_tui_reset_cursor_blink();

    switch (key) {
    case SOLAR_OS_KEY_ESCAPE:
        setterm_tui_cancel_edit();
        return;
    case SOLAR_OS_KEY_LEFT:
        if (setterm_tui_cycle_selected(-1)) {
            setterm_tui_render();
        }
        return;
    case SOLAR_OS_KEY_RIGHT:
        if (setterm_tui_cycle_selected(1)) {
            setterm_tui_render();
        }
        return;
    case '\r':
    case '\n':
        setterm_tui_commit_edit();
        return;
    case '\b':
    case 0x7f:
    case SOLAR_OS_KEY_DELETE:
        if (len > 0) {
            setterm_tui.edit_text[len - 1] = '\0';
            setterm_tui_set_status("");
            setterm_tui_render();
        }
        return;
    default:
        break;
    }

    if (isprint((unsigned char)ch) && len + 1 < sizeof(setterm_tui.edit_text)) {
        setterm_tui.edit_text[len] = ch;
        setterm_tui.edit_text[len + 1] = '\0';
        setterm_tui_set_status("");
        setterm_tui_render();
    }
}

static esp_err_t setterm_tui_start(solar_os_context_t *ctx)
{
    memset(&setterm_tui, 0, sizeof(setterm_tui));
    setterm_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_begin(&setterm_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), false);
    setterm_tui_render();
    return ESP_OK;
}

static void setterm_tui_stop(solar_os_context_t *ctx)
{
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), true);
    solar_os_terminal_clear(display_terminal(ctx));
    solar_os_context_request_terminal_preserve(ctx);
}

static bool setterm_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!setterm_tui.editing) {
            return false;
        }

        const uint32_t now_ms = event->data.tick_ms;
        if (setterm_tui.last_cursor_blink_ms == 0) {
            setterm_tui.last_cursor_blink_ms = now_ms;
            return true;
        }
        if ((now_ms - setterm_tui.last_cursor_blink_ms) >= SETTERM_TUI_CURSOR_BLINK_MS) {
            setterm_tui.last_cursor_blink_ms = now_ms;
            setterm_tui.cursor_visible = !setterm_tui.cursor_visible;
            solar_os_terminal_set_cursor_visible(display_terminal(setterm_tui.ctx),
                                                 setterm_tui.cursor_visible);
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(setterm_tui.ctx);
        return true;
    }

    if (setterm_tui.editing) {
        setterm_tui_handle_edit_key(event->data.ch);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (setterm_tui.selected > 0) {
            setterm_tui.selected--;
            setterm_tui_set_status("");
            setterm_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (setterm_tui.selected + 1 < SETTERM_TUI_ITEM_COUNT) {
            setterm_tui.selected++;
            setterm_tui_set_status("");
            setterm_tui_render();
        }
        break;
    case '\r':
    case '\n':
        setterm_tui_begin_edit();
        break;
    case SOLAR_OS_KEY_ESCAPE:
        solar_os_context_request_exit(setterm_tui.ctx);
        break;
    default:
        break;
    }

    return true;
}

static const solar_os_app_t setterm_tui_app = {
    .name = "setterm",
    .summary = "terminal settings",
    .start = setterm_tui_start,
    .stop = setterm_tui_stop,
    .event = setterm_tui_event,
};

void solar_os_shell_cmd_setterm(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    solar_os_terminal_t *display = display_terminal(ctx);

    if (argc == 1) {
        if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term, "setterm: TUI is only available on the display shell");
            setterm_print_usage(term);
            return;
        }
        const esp_err_t err = solar_os_context_request_launch(ctx, &setterm_tui_app, 0, NULL);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "setterm: launch failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "orientation") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(term,
                                     "orientation: %u\n",
                                     (unsigned)solar_os_terminal_orientation(display));
            solar_os_shell_io_writeln(term, "values: 0 90 180 270");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: setterm orientation [0|90|180|270]");
            return;
        }

        size_t degrees = 0;
        if (!parse_size_arg(argv[2], 0, 270, &degrees) ||
            !(degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270)) {
            solar_os_shell_io_writeln(term, "orientation values: 0 90 180 270");
            return;
        }

        const esp_err_t err = solar_os_terminal_set_orientation(display, (uint16_t)degrees);
        setterm_print_save_result(term, "orientation", argv[2], err);
        return;
    }

    if (strcmp(argv[1], "font") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(term,
                                     "font: %s\n",
                                     solar_os_terminal_font_name(solar_os_terminal_font(display)));
            solar_os_shell_io_writeln(term, "values: mono compact");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: setterm font [mono|compact]");
            return;
        }

        solar_os_terminal_font_t font;
        if (!solar_os_terminal_parse_font(argv[2], &font)) {
            solar_os_shell_io_writeln(term, "font values: mono compact");
            return;
        }

        const esp_err_t err = solar_os_terminal_set_font(display, font);
        setterm_print_save_result(term, "font", argv[2], err);
        return;
    }

    if (strcmp(argv[1], "textsize") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(
                term,
                "textsize: %s\n",
                solar_os_terminal_text_size_name(solar_os_terminal_text_size(display)));
            solar_os_shell_io_writeln(term, "values: 12 14 16 18 20");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: setterm textsize [12|14|16|18|20]");
            return;
        }

        solar_os_terminal_text_size_t text_size;
        if (!solar_os_terminal_parse_text_size(argv[2], &text_size)) {
            solar_os_shell_io_writeln(term, "textsize values: 12 14 16 18 20");
            return;
        }

        const esp_err_t err = solar_os_terminal_set_text_size(display, text_size);
        setterm_print_save_result(term, "textsize", argv[2], err);
        return;
    }

    if (strcmp(argv[1], "keyboard") == 0 || strcmp(argv[1], "keymap") == 0) {
        if (argc == 2) {
            solar_os_shell_io_printf(term,
                                     "keyboard: %s\n",
                                     solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout()));
            solar_os_shell_io_writeln(term, "values: us de");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: setterm keyboard [us|de]");
            return;
        }

        solar_os_ble_keyboard_layout_t layout;
        if (!solar_os_ble_keyboard_parse_layout(argv[2], &layout)) {
            solar_os_shell_io_writeln(term, "keyboard values: us de");
            return;
        }

        const esp_err_t err = solar_os_ble_keyboard_set_layout(layout);
        setterm_print_save_result(term, "keyboard", argv[2], err);
        return;
    }

    if (strcmp(argv[1], "keyrate") == 0 ||
        strcmp(argv[1], "typerate") == 0 ||
        strcmp(argv[1], "repeat") == 0) {
        if (argc == 2) {
            setterm_print_keyrate(term);
            solar_os_shell_io_printf(term,
                                     "values: off or %u..%u [delay %u..%u ms]\n",
                                     (unsigned)SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN,
                                     (unsigned)SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX,
                                     (unsigned)SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS,
                                     (unsigned)SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS);
            return;
        }
        if (argc < 3 || argc > 4) {
            solar_os_shell_io_writeln(term, "usage: setterm keyrate [off|1..60 [delay-ms]]");
            return;
        }

        uint16_t current_delay_ms = 0;
        solar_os_ble_keyboard_get_repeat(NULL, &current_delay_ms);

        if (strcmp(argv[2], "off") == 0) {
            if (argc != 3) {
                solar_os_shell_io_writeln(term, "usage: setterm keyrate off");
                return;
            }

            const esp_err_t err = solar_os_ble_keyboard_set_repeat(0, current_delay_ms);
            setterm_print_keyrate_result(term, err);
            return;
        }

        size_t rate = 0;
        if (!parse_size_arg(argv[2],
                            SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN,
                            SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX,
                            &rate)) {
            solar_os_shell_io_writeln(term, "keyrate values: off or 1..60");
            return;
        }

        size_t delay_ms = current_delay_ms;
        if (argc == 4 &&
            !parse_size_arg(argv[3],
                            SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS,
                            SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS,
                            &delay_ms)) {
            solar_os_shell_io_writeln(term, "keyrate delay values: 100..2000 ms");
            return;
        }

        const esp_err_t err =
            solar_os_ble_keyboard_set_repeat((uint16_t)rate, (uint16_t)delay_ms);
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
            solar_os_shell_io_writeln(term, "values: UTC Europe/Berlin or POSIX TZ");
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term,
                                      "usage: setterm timezone [UTC|Europe/Berlin|POSIX-TZ]");
            return;
        }

        const esp_err_t err = solar_os_time_set_timezone(argv[2]);
        if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_printf(term, "timezone: invalid value: %s\n", argv[2]);
            solar_os_shell_io_writeln(term, "values: UTC Europe/Berlin or POSIX TZ");
            return;
        }

        solar_os_time_get_timezone(timezone, sizeof(timezone), NULL, 0);
        setterm_print_save_result(term, "timezone", timezone, err);
        return;
    }

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
            solar_os_shell_io_writeln(term, "usage: setterm otaurl [url]");
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

    setterm_print_usage(term);
}

void solar_os_shell_cmd_status(solar_os_context_t *ctx, int argc, char **argv)
{
    char ble_status[64];
    char sd_status[64];
    char wifi_status[64];
    char uptime[32];
    solar_os_battery_status_t battery_status;
    solar_os_audio_status_t audio_status;
    solar_os_uart_status_t uart_status;
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argc;
    (void)argv;

    solar_os_ble_keyboard_get_status(ble_status, sizeof(ble_status));
    solar_os_storage_get_status(sd_status, sizeof(sd_status));
    solar_os_wifi_get_status_text(wifi_status, sizeof(wifi_status));
    solar_os_time_format_uptime(solar_os_time_uptime_ms(), uptime, sizeof(uptime));

    solar_os_shell_io_printf(term, "BLE: %s\n", ble_status);
    solar_os_shell_io_printf(term, "SD: %s\n", sd_status);
    solar_os_shell_io_printf(term, "WiFi: %s\n", wifi_status);
    solar_os_shell_io_printf(term, "Uptime: %s\n", uptime);
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
    solar_os_shell_io_printf(term,
                             "Heap: internal %u, PSRAM %u\n",
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        solar_os_shell_io_printf(term,
                                 "I2C: SDA %d, SCL %d, %" PRIu32 " Hz\n",
                                 solar_os_i2c_get_sda_pin(),
                                 solar_os_i2c_get_scl_pin(),
                                 solar_os_i2c_get_speed_hz());
    } else {
        solar_os_shell_io_writeln(term, "I2C: not available on this board");
    }
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

    solar_os_shell_io_writeln(term, "ID           TYPE    FORMAT UNIT      SUMMARY");
    for (size_t i = 0; i < count; i++) {
        solar_os_stream_info_t info;
        if (!solar_os_stream_get(i, &info)) {
            continue;
        }
        solar_os_shell_io_printf(term,
                                 "%-12s %-7s %-6s %-9s %s\n",
                                 info.id,
                                 solar_os_stream_type_name(info.type),
                                 info.format,
                                 info.unit[0] != '\0' ? info.unit : "-",
                                 info.summary);
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
            solar_os_shell_io_printf(term, "stream status failed: %s\n", esp_err_to_name(err));
            return;
        }

        char header[SOLAR_OS_STREAM_CSV_HEADER_MAX];
        solar_os_shell_io_printf(term, "ID: %s\n", info.id);
        solar_os_shell_io_printf(term, "Type: %s\n", solar_os_stream_type_name(info.type));
        solar_os_shell_io_printf(term, "Format: %s\n", info.format);
        solar_os_shell_io_printf(term, "Unit: %s\n", info.unit[0] != '\0' ? info.unit : "-");
        solar_os_shell_io_printf(term, "Summary: %s\n", info.summary);
        if (solar_os_stream_csv_header(&info, header, sizeof(header)) == ESP_OK) {
            solar_os_shell_io_printf(term, "CSV: %s\n", header);
        }
        return;
    }

    stream_print_usage(term);
}

static void daq_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  daq help");
    solar_os_shell_io_writeln(term, "  daq status");
    solar_os_shell_io_writeln(term, "  daq streams");
    solar_os_shell_io_writeln(term, "  daq start <file.csv> <stream...> [--rate seconds|--rate-ms ms]");
    solar_os_shell_io_writeln(term, "  daq start <stream...> <file.csv> [--rate seconds|--rate-ms ms]");
    solar_os_shell_io_writeln(term, "  daq start <file.csv> <stream> --changes [--append|--replace]");
    solar_os_shell_io_writeln(term, "  daq start <file.bin> <byte-stream> --raw [--rate-ms ms]");
    solar_os_shell_io_writeln(term, "  daq stop");
    solar_os_shell_io_writeln(term, "");
    solar_os_shell_io_writeln(term, "examples:");
    solar_os_shell_io_writeln(term, "  daq start /logs/env.csv temperature humidity battery --rate 60");
    solar_os_shell_io_writeln(term, "  daq start /logs/key.csv gpio17 --changes");
    solar_os_shell_io_writeln(term, "  daq start /logs/uart0.bin uart0 --raw --rate-ms 25");
}

static void daq_print_status(solar_os_shell_io_t *term)
{
    solar_os_daq_status_t status;
    solar_os_daq_job_get_status(&status);

    if (!status.running) {
        solar_os_shell_io_printf(term,
                                 "DAQ: stopped%s%s\n",
                                 status.last_error == ESP_OK ? "" : ", last error ",
                                 status.last_error == ESP_OK ? "" : esp_err_to_name(status.last_error));
        return;
    }

    solar_os_shell_io_printf(term, "DAQ: running %s -> %s\n", status.stream_ids, status.path);
    solar_os_shell_io_printf(term,
                             "Streams: %u, type %s, mode %s, interval %" PRIu32 " ms, %s, %s\n",
                             (unsigned)status.stream_count,
                             solar_os_stream_type_name(status.stream_type),
                             status.raw ? "raw" : "csv",
                             status.interval_ms,
                             status.change_only ? "changes" : "samples",
                             status.append ? "append" : "replace");
    if (status.raw) {
        solar_os_shell_io_printf(term,
                                 "Chunks: written %" PRIu32 ", bytes %" PRIu64
                                 ", skipped %" PRIu32 ", failed %" PRIu32 "\n",
                                 status.written_records,
                                 status.written_bytes,
                                 status.skipped_records,
                                 status.failed_records);
    } else {
        solar_os_shell_io_printf(term,
                                 "Records: written %" PRIu32
                                 ", skipped %" PRIu32 ", failed %" PRIu32 "\n",
                                 status.written_records,
                                 status.skipped_records,
                                 status.failed_records);
    }
    if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term, "Last error: %s\n", esp_err_to_name(status.last_error));
    }
}

static void daq_print_start_error(solar_os_shell_io_t *term, const char *stream_id, esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "daq: stream not found: %s\n", stream_id);
        return;
    }
    if (err == ESP_ERR_INVALID_STATE && stream_id != NULL) {
        solar_os_stream_info_t stream;
        solar_os_port_info_t port;
        if (solar_os_stream_get_info(stream_id, &stream) == ESP_OK &&
            stream.type == SOLAR_OS_STREAM_TYPE_BYTES &&
            solar_os_port_get_info(stream.id, &port) == ESP_OK &&
            port.claimed) {
            solar_os_shell_io_printf(term, "daq: %s is owned by %s\n", stream.id, port.owner);
            return;
        }
    }
    if (err == ESP_ERR_INVALID_ARG) {
        daq_print_usage(term);
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "daq: unsupported stream/mode combination");
        return;
    }

    solar_os_shell_io_printf(term, "daq start failed: %s\n", esp_err_to_name(err));
}

void solar_os_shell_cmd_daq(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "help") == 0)) {
        daq_print_usage(term);
        return;
    }

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        daq_print_status(term);
        return;
    }

    if (argc == 2 && strcmp(argv[1], "streams") == 0) {
        stream_print_list(term);
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: daq stop");
            return;
        }
        const esp_err_t err = solar_os_jobs_stop(ctx, "daq");
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "daq: stopped");
        } else {
            solar_os_shell_io_printf(term, "daq stop failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 4 || argc > SOLAR_OS_APP_ARG_MAX + 1) {
            daq_print_usage(term);
            return;
        }

        char *job_argv[SOLAR_OS_APP_ARG_MAX];
        job_argv[0] = "daq";
        for (int i = 2; i < argc; i++) {
            job_argv[i - 1] = argv[i];
        }

        const esp_err_t err = solar_os_jobs_start(ctx, "daq", argc - 1, job_argv);
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "daq: started");
        } else {
            daq_print_start_error(term, argv[2], err);
        }
        return;
    }

    daq_print_usage(term);
}

void solar_os_shell_cmd_df(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    (void)argv;

    if (argc != 1) {
        solar_os_shell_io_writeln(term, "usage: df");
        return;
    }

    const esp_err_t scan_err = solar_os_storage_rescan();
    if (scan_err != ESP_OK) {
        if (shell_print_not_supported(term, "df", "SD storage", scan_err)) {
            return;
        }
        solar_os_shell_io_printf(term, "df: SD card not available: %s\n", esp_err_to_name(scan_err));
        return;
    }

    bool any = false;
    solar_os_shell_io_writeln(term, "Filesystem  Total    Used     Free     Use% Mount");
    const size_t count = solar_os_storage_block_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        solar_os_storage_usage_t usage;
        char total[12];
        char used[12];
        char free_space[12];

        if (!solar_os_storage_get_block(i, &block) || !block.mounted) {
            continue;
        }

        const esp_err_t err = solar_os_storage_get_usage_for_block(&block, &usage);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "%-10s read failed: %s\n", block.name, esp_err_to_name(err));
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
                                 block.name,
                                 total,
                                 used,
                                 free_space,
                                 (unsigned)used_percent,
                                 block.mount_point);
        any = true;
    }

    if (!any) {
        solar_os_shell_io_writeln(term, "df: no mounted filesystems");
    }
}

static void sd_print_status(solar_os_shell_io_t *term)
{
    char status[64];
    solar_os_storage_get_status(status, sizeof(status));
    solar_os_shell_io_printf(term, "SD: %s\n", status);
    solar_os_shell_io_printf(term, "Mount: %s\n", solar_os_storage_mount_point());
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
    solar_os_shell_io_writeln(term, "  sd unmount [sd0pN|mount]");
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

    if (strcmp(argv[1], "unmount") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: sd unmount [sd0pN|mount]");
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
            solar_os_shell_io_printf(term, "sd unmount failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    sd_print_usage(term);
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
    TaskStatus_t *tasks = calloc(task_capacity, sizeof(TaskStatus_t));
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
        free(tasks);
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

    free(tasks);
#else
    solar_os_shell_io_writeln(term, "top: FreeRTOS trace facility disabled");
#endif
}

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

static void wifi_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  wifi [status]");
    solar_os_shell_io_writeln(term, "  wifi on");
    solar_os_shell_io_writeln(term, "  wifi off");
    solar_os_shell_io_writeln(term, "  wifi scan");
    solar_os_shell_io_writeln(term, "  wifi connect [ssid [password]]");
    solar_os_shell_io_writeln(term, "  wifi disconnect");
    solar_os_shell_io_writeln(term, "  wifi known");
    solar_os_shell_io_writeln(term, "  wifi forget [ssid|all]");
    solar_os_shell_io_writeln(term, "  wifi nat [status|on|off]");
    solar_os_shell_io_writeln(term, "  wifi ap [status]");
    solar_os_shell_io_writeln(term, "  wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]");
    solar_os_shell_io_writeln(term, "  wifi ap off");
}

static void wifi_print_nat_status(solar_os_shell_io_t *term, const solar_os_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if (!status->nat_enabled) {
        solar_os_shell_io_writeln(term, "NAT: off");
        return;
    }
    if (status->nat_active) {
        solar_os_shell_io_writeln(term, "NAT: active");
        return;
    }
    if (status->nat_last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "NAT: error %s\n",
                                 esp_err_to_name(status->nat_last_error));
        return;
    }

    solar_os_shell_io_writeln(term, "NAT: waiting for APSTA link");
}

static void wifi_print_status(solar_os_shell_io_t *term)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    solar_os_shell_io_printf(term,
                             "WiFi: %s%s\n",
                             solar_os_wifi_state_name(status.state),
                             status.started ? "" : " (radio off)");
    if (status.ssid[0] != '\0') {
        solar_os_shell_io_printf(term, "SSID: %s\n", status.ssid);
    }
    if (status.has_ip) {
        solar_os_shell_io_printf(term, "IP: %s\n", status.ip);
        solar_os_shell_io_printf(term, "Gateway: %s\n", status.gateway);
        solar_os_shell_io_printf(term, "Netmask: %s\n", status.netmask);
    }
    if (status.connected) {
        solar_os_shell_io_printf(term,
                                 "Link: ch %u, RSSI %d dBm\n",
                                 (unsigned)status.channel,
                                 (int)status.rssi);
    }
    if (status.has_saved_config) {
        solar_os_shell_io_printf(term,
                                 "Saved: %u, preferred %s\n",
                                 (unsigned)status.saved_profile_count,
                                 status.saved_ssid);
    } else {
        solar_os_shell_io_writeln(term, "Saved: none");
    }
    if (status.has_saved_ap_config) {
        solar_os_shell_io_printf(term,
                                 "Saved AP: %s (%s)\n",
                                 status.saved_ap_ssid,
                                 status.saved_ap_auth[0] != '\0' ? status.saved_ap_auth : "open");
    } else {
        solar_os_shell_io_writeln(term, "Saved AP: none");
    }
    if (status.ap_enabled || status.ap_running) {
        solar_os_shell_io_printf(term, "AP: %s\n", status.ap_running ? "on" : "starting");
        if (status.ap_ssid[0] != '\0') {
            solar_os_shell_io_printf(term, "AP SSID: %s\n", status.ap_ssid);
        }
        if (status.ap_ip[0] != '\0') {
            solar_os_shell_io_printf(term, "AP IP: %s\n", status.ap_ip);
        }
        solar_os_shell_io_printf(term,
                                 "AP Link: ch %u, %s, clients %u/%u\n",
                                 (unsigned)status.ap_channel,
                                 status.ap_auth[0] != '\0' ? status.ap_auth : "open",
                                 (unsigned)status.ap_station_count,
                                 (unsigned)status.ap_max_connections);
    } else {
        solar_os_shell_io_writeln(term, "AP: off");
    }
    wifi_print_nat_status(term, &status);
    if (status.disconnect_reason != 0) {
        solar_os_shell_io_printf(term,
                                 "Last disconnect reason: %u\n",
                                 (unsigned)status.disconnect_reason);
    }
}

static void wifi_cmd_scan(solar_os_shell_io_t *term)
{
    solar_os_wifi_ap_t aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t found = 0;
    const esp_err_t err = solar_os_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);

    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi scan failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "RSSI CH AUTH       K SSID");
    for (size_t i = 0; i < found; i++) {
        const bool known = !aps[i].hidden && solar_os_wifi_is_known_ssid(aps[i].ssid);
        solar_os_shell_io_printf(term,
                                 "%4d %2u %-10s %c %s\n",
                                 (int)aps[i].rssi,
                                 (unsigned)aps[i].channel,
                                 aps[i].auth,
                                 known ? '*' : '-',
                                 aps[i].ssid);
    }
    solar_os_shell_io_printf(term,
                             "%u network%s shown\n",
                             (unsigned)found,
                             found == 1 ? "" : "s");
}

static void wifi_cmd_known(solar_os_shell_io_t *term)
{
    solar_os_wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;
    const esp_err_t err = solar_os_wifi_known(profiles,
                                              sizeof(profiles) / sizeof(profiles[0]),
                                              &count);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi known failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (count == 0) {
        solar_os_shell_io_writeln(term, "no known networks");
        return;
    }

    solar_os_shell_io_writeln(term, "P SSID");
    const size_t shown = count < SOLAR_OS_WIFI_PROFILE_MAX ? count : SOLAR_OS_WIFI_PROFILE_MAX;
    for (size_t i = 0; i < shown; i++) {
        solar_os_shell_io_printf(term,
                                 "%c %s\n",
                                 profiles[i].preferred ? '*' : '-',
                                 profiles[i].ssid);
    }
    if (count > shown) {
        solar_os_shell_io_printf(term, "%u more not shown\n", (unsigned)(count - shown));
    }
}

static void wifi_cmd_ap(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: wifi ap [status]");
            return;
        }
        wifi_print_status(term);
        return;
    }

    if (strcmp(argv[2], "on") == 0) {
        if (argc > 6) {
            solar_os_shell_io_writeln(
                term,
                "usage: wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]");
            return;
        }

        const char *ssid = argc >= 4 ? argv[3] : NULL;
        const char *password = argc >= 5 ? argv[4] : NULL;
        const char *auth = argc >= 6 ? argv[5] : NULL;
        const esp_err_t err = solar_os_wifi_ap_start(ssid, password, auth);
        if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "WiFi AP on: %s (%s)\n",
                                     status.ap_ssid,
                                     status.ap_auth[0] != '\0' ? status.ap_auth : "open");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "wifi ap: WEP is not supported in SoftAP mode");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "wifi ap: invalid SSID, password, or auth mode");
        } else {
            solar_os_shell_io_printf(term, "wifi ap on failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[2], "off") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: wifi ap off");
            return;
        }

        const esp_err_t err = solar_os_wifi_ap_stop();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi AP off");
        } else {
            solar_os_shell_io_printf(term, "wifi ap off failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    wifi_print_usage(term);
}

static void wifi_cmd_nat(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
            return;
        }
        solar_os_wifi_status_t status;
        solar_os_wifi_get_status(&status);
        wifi_print_nat_status(term, &status);
        return;
    }

    if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
            return;
        }

        const bool enabled = strcmp(argv[2], "on") == 0;
        const esp_err_t err = solar_os_wifi_nat_set(enabled);
        if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            wifi_print_nat_status(term, &status);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "wifi nat: NAT is not supported in this build");
        } else {
            solar_os_shell_io_printf(term, "wifi nat failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
}

static void wifi_cmd_connect(solar_os_shell_io_t *term, int argc, char **argv)
{
    esp_err_t err;

    if (argc == 2) {
        err = solar_os_wifi_connect_saved();
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "wifi: no saved network");
        } else if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "WiFi connecting to %s\n",
                                     status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
        } else {
            solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (argc < 3 || argc > 4) {
        solar_os_shell_io_writeln(term, "usage: wifi connect [ssid [password]]");
        return;
    }

    const char *ssid = argv[2];
    const char *password = argc == 4 ? argv[3] : "";
    err = solar_os_wifi_connect(ssid, password);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "WiFi connecting to %s\n", ssid);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "wifi: invalid SSID or password length");
    } else {
        solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
    }
}

static void wifi_cmd_on(solar_os_shell_io_t *term)
{
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi on failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    if (status.connected || status.state == SOLAR_OS_WIFI_STATE_CONNECTING) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
        return;
    }

    if (!status.has_saved_config) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
        return;
    }

    err = solar_os_wifi_connect_saved();
    if (err == ESP_OK) {
        solar_os_wifi_get_status(&status);
        solar_os_shell_io_printf(term,
                                 "WiFi radio on, connecting to %s\n",
                                 status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
    } else {
        solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
    }
}

typedef enum {
    WIFI_TUI_RADIO,
    WIFI_TUI_STATION,
    WIFI_TUI_DISCONNECT,
    WIFI_TUI_AP,
    WIFI_TUI_NAT,
    WIFI_TUI_SCAN,
    WIFI_TUI_SAVED_STA,
    WIFI_TUI_SAVED_AP,
    WIFI_TUI_ITEM_COUNT,
} wifi_tui_item_t;

typedef struct {
    const char *label;
} wifi_tui_item_def_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected;
    char status[WIFI_TUI_STATUS_MAX];
    solar_os_wifi_ap_t scan_aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t scan_count;
    bool scan_valid;
    uint32_t last_refresh_ms;
} wifi_tui_state_t;

static wifi_tui_state_t wifi_tui;

static const wifi_tui_item_def_t wifi_tui_items[] = {
    [WIFI_TUI_RADIO] = {.label = "radio"},
    [WIFI_TUI_STATION] = {.label = "station"},
    [WIFI_TUI_DISCONNECT] = {.label = "disconnect"},
    [WIFI_TUI_AP] = {.label = "ap"},
    [WIFI_TUI_NAT] = {.label = "nat"},
    [WIFI_TUI_SCAN] = {.label = "scan"},
    [WIFI_TUI_SAVED_STA] = {.label = "saved sta"},
    [WIFI_TUI_SAVED_AP] = {.label = "saved ap"},
};

static size_t wifi_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void wifi_tui_set_status(const char *status)
{
    strlcpy(wifi_tui.status, status != NULL ? status : "", sizeof(wifi_tui.status));
}

static void wifi_tui_write_cell(size_t row,
                                size_t col,
                                size_t width,
                                const char *text,
                                uint8_t attr)
{
    char clipped[WIFI_TUI_STATUS_MAX];
    size_t len = 0;

    if (width == 0) {
        return;
    }

    solar_os_tui_fill(&wifi_tui.tui, row, col, 1, width, ' ', attr);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    while (text[len] != '\0' && len + 1 < sizeof(clipped) && len < width) {
        clipped[len] = text[len];
        len++;
    }
    clipped[len] = '\0';
    solar_os_tui_addstr(&wifi_tui.tui, row, col, clipped, attr);
}

static void wifi_tui_nat_value(const solar_os_wifi_status_t *status,
                               char *buffer,
                               size_t buffer_len)
{
    if (!status->nat_enabled) {
        strlcpy(buffer, "off", buffer_len);
    } else if (status->nat_active) {
        strlcpy(buffer, "active", buffer_len);
    } else if (status->nat_last_error != ESP_OK) {
        snprintf(buffer, buffer_len, "error %s", esp_err_to_name(status->nat_last_error));
    } else {
        strlcpy(buffer, "waiting", buffer_len);
    }
}

static void wifi_tui_current_value(wifi_tui_item_t item,
                                   const solar_os_wifi_status_t *status,
                                   char *buffer,
                                   size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0 || status == NULL) {
        return;
    }

    switch (item) {
    case WIFI_TUI_RADIO:
        strlcpy(buffer, status->started ? "on" : "off", buffer_len);
        break;
    case WIFI_TUI_STATION:
        if (status->connected && status->has_ip) {
            snprintf(buffer,
                     buffer_len,
                     "%s %s",
                     status->ssid[0] != '\0' ? status->ssid : "connected",
                     status->ip);
        } else if (status->state == SOLAR_OS_WIFI_STATE_CONNECTING) {
            snprintf(buffer,
                     buffer_len,
                     "connecting %s",
                     status->ssid[0] != '\0' ? status->ssid : status->saved_ssid);
        } else {
            strlcpy(buffer, solar_os_wifi_state_name(status->state), buffer_len);
        }
        break;
    case WIFI_TUI_DISCONNECT:
        strlcpy(buffer, status->connected ? "ready" : "-", buffer_len);
        break;
    case WIFI_TUI_AP:
        if (status->ap_running) {
            snprintf(buffer,
                     buffer_len,
                     "on %s",
                     status->ap_ssid[0] != '\0' ? status->ap_ssid : status->ap_ip);
        } else if (status->ap_enabled) {
            strlcpy(buffer, "starting", buffer_len);
        } else if (status->has_saved_ap_config) {
            snprintf(buffer, buffer_len, "off saved %s", status->saved_ap_ssid);
        } else {
            strlcpy(buffer, "off", buffer_len);
        }
        break;
    case WIFI_TUI_NAT:
        wifi_tui_nat_value(status, buffer, buffer_len);
        break;
    case WIFI_TUI_SCAN:
        if (wifi_tui.scan_valid) {
            snprintf(buffer, buffer_len, "%u shown", (unsigned)wifi_tui.scan_count);
        } else {
            strlcpy(buffer, "enter", buffer_len);
        }
        break;
    case WIFI_TUI_SAVED_STA:
        if (status->has_saved_config) {
            snprintf(buffer,
                     buffer_len,
                     "%u %s",
                     (unsigned)status->saved_profile_count,
                     status->saved_ssid);
        } else {
            strlcpy(buffer, "none", buffer_len);
        }
        break;
    case WIFI_TUI_SAVED_AP:
        if (status->has_saved_ap_config) {
            snprintf(buffer,
                     buffer_len,
                     "%s (%s)",
                     status->saved_ap_ssid,
                     status->saved_ap_auth[0] != '\0' ? status->saved_ap_auth : "open");
        } else {
            strlcpy(buffer, "none", buffer_len);
        }
        break;
    default:
        strlcpy(buffer, "-", buffer_len);
        break;
    }
}

static void wifi_tui_render_scan(size_t start_row, size_t rows, size_t cols)
{
    if (start_row >= rows || cols == 0 || !wifi_tui.scan_valid) {
        return;
    }

    wifi_tui_write_cell(start_row,
                        0,
                        cols,
                        wifi_tui.scan_count == 0 ? "scan: no networks" : "scan: rssi ch auth k ssid",
                        SOLAR_OS_TUI_ATTR_BOLD);

    for (size_t i = 0; i < wifi_tui.scan_count && start_row + i + 1 < rows; i++) {
        char line[WIFI_TUI_STATUS_MAX];
        const bool known = !wifi_tui.scan_aps[i].hidden &&
            solar_os_wifi_is_known_ssid(wifi_tui.scan_aps[i].ssid);
        snprintf(line,
                 sizeof(line),
                 "%4d %2u %-10s %c %s",
                 (int)wifi_tui.scan_aps[i].rssi,
                 (unsigned)wifi_tui.scan_aps[i].channel,
                 wifi_tui.scan_aps[i].auth,
                 known ? '*' : '-',
                 wifi_tui.scan_aps[i].ssid);
        wifi_tui_write_cell(start_row + i + 1, 0, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void wifi_tui_render(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    solar_os_wifi_status_t status;

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_wifi_get_status(&status);
    solar_os_tui_clear(tui);

    size_t split = cols / 2;
    if (cols >= 24 && split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    wifi_tui_write_cell(0,
                        0,
                        split,
                        "wifi",
                        SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        wifi_tui_write_cell(0,
                            split + 1,
                            wifi_tui_visible_width(cols, split + 1),
                            "value",
                            SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t value_col = split + 1;
    const size_t value_width = wifi_tui_visible_width(cols, value_col);
    for (size_t i = 0; i < WIFI_TUI_ITEM_COUNT && i + 1 < rows; i++) {
        char value[WIFI_TUI_STATUS_MAX];
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (i == wifi_tui.selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }

        wifi_tui_current_value((wifi_tui_item_t)i, &status, value, sizeof(value));
        wifi_tui_write_cell(i + 1, 0, split, wifi_tui_items[i].label, label_attr);
        if (value_width > 0) {
            wifi_tui_write_cell(i + 1, value_col, value_width, value, value_attr);
        }
    }

    const size_t scan_row = WIFI_TUI_ITEM_COUNT + 2;
    const size_t status_row = rows > 1 ? rows - 1 : 0;
    if (scan_row < status_row) {
        wifi_tui_render_scan(scan_row, status_row, cols);
    }

    if (wifi_tui.status[0] != '\0' && rows > 1) {
        wifi_tui_write_cell(status_row, 0, cols, wifi_tui.status, SOLAR_OS_TUI_ATTR_INVERSE);
    }

    solar_os_terminal_set_cursor_visible(tui->terminal, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_start_radio(void)
{
    solar_os_wifi_status_t status;
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "wifi on failed: %s", esp_err_to_name(err));
        wifi_tui_set_status(message);
        return;
    }

    solar_os_wifi_get_status(&status);
    if (status.connected || status.state == SOLAR_OS_WIFI_STATE_CONNECTING ||
        !status.has_saved_config) {
        wifi_tui_set_status("radio on");
        return;
    }

    err = solar_os_wifi_connect_saved();
    if (err == ESP_OK) {
        solar_os_wifi_get_status(&status);
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message,
                 sizeof(message),
                 "connecting %s",
                 status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
        wifi_tui_set_status(message);
    } else if (err == ESP_ERR_NOT_FOUND) {
        wifi_tui_set_status("radio on");
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "connect failed: %s", esp_err_to_name(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_apply_selected(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    switch ((wifi_tui_item_t)wifi_tui.selected) {
    case WIFI_TUI_RADIO:
        if (status.started) {
            const esp_err_t err = solar_os_wifi_stop();
            wifi_tui_set_status(err == ESP_OK ? "radio off" : esp_err_to_name(err));
        } else {
            wifi_tui_start_radio();
        }
        break;
    case WIFI_TUI_STATION: {
        const esp_err_t err = solar_os_wifi_connect_saved();
        if (err == ESP_OK) {
            solar_os_wifi_get_status(&status);
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message,
                     sizeof(message),
                     "connecting %s",
                     status.saved_ssid[0] != '\0' ? status.saved_ssid : status.ssid);
            wifi_tui_set_status(message);
        } else if (err == ESP_ERR_NOT_FOUND) {
            wifi_tui_set_status("no saved station");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "connect failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_DISCONNECT: {
        const esp_err_t err = solar_os_wifi_disconnect();
        wifi_tui_set_status(err == ESP_OK ? "station disconnected" : esp_err_to_name(err));
        break;
    }
    case WIFI_TUI_AP: {
        const esp_err_t err =
            (status.ap_running || status.ap_enabled) ?
            solar_os_wifi_ap_stop() :
            solar_os_wifi_ap_start(NULL, NULL, NULL);
        if (err == ESP_OK) {
            wifi_tui_set_status(status.ap_running || status.ap_enabled ? "ap off" : "ap on");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "ap failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_NAT: {
        const esp_err_t err = solar_os_wifi_nat_set(!status.nat_enabled);
        if (err == ESP_OK) {
            wifi_tui_set_status(status.nat_enabled ? "nat off" : "nat on");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            wifi_tui_set_status("nat unsupported");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "nat failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SCAN: {
        size_t found = 0;
        wifi_tui_set_status("scanning...");
        wifi_tui.scan_valid = false;
        wifi_tui_render();
        const esp_err_t err = solar_os_wifi_scan(wifi_tui.scan_aps,
                                                 sizeof(wifi_tui.scan_aps) / sizeof(wifi_tui.scan_aps[0]),
                                                 &found);
        if (err == ESP_OK) {
            wifi_tui.scan_count = found;
            wifi_tui.scan_valid = true;
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "%u network%s", (unsigned)found, found == 1 ? "" : "s");
            wifi_tui_set_status(message);
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "scan failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SAVED_STA:
    case WIFI_TUI_SAVED_AP:
        wifi_tui_set_status("read only");
        break;
    default:
        break;
    }

    wifi_tui_render();
}

static esp_err_t wifi_tui_start(solar_os_context_t *ctx)
{
    memset(&wifi_tui, 0, sizeof(wifi_tui));
    wifi_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_begin(&wifi_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    wifi_tui_set_status("enter acts, esc exits");
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), false);
    wifi_tui_render();
    return ESP_OK;
}

static void wifi_tui_stop(solar_os_context_t *ctx)
{
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), true);
    solar_os_terminal_clear(display_terminal(ctx));
    solar_os_context_request_terminal_preserve(ctx);
}

static bool wifi_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if (wifi_tui.last_refresh_ms == 0) {
            wifi_tui.last_refresh_ms = now_ms;
            return true;
        }
        if ((now_ms - wifi_tui.last_refresh_ms) >= WIFI_TUI_REFRESH_MS) {
            wifi_tui.last_refresh_ms = now_ms;
            wifi_tui_render();
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(wifi_tui.ctx);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (wifi_tui.selected > 0) {
            wifi_tui.selected--;
            wifi_tui_set_status("");
            wifi_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (wifi_tui.selected + 1 < WIFI_TUI_ITEM_COUNT) {
            wifi_tui.selected++;
            wifi_tui_set_status("");
            wifi_tui_render();
        }
        break;
    case '\r':
    case '\n':
        wifi_tui_apply_selected();
        break;
    default:
        break;
    }

    return true;
}

static const solar_os_app_t wifi_tui_app = {
    .name = "wifi",
    .summary = "Wi-Fi control",
    .start = wifi_tui_start,
    .stop = wifi_tui_stop,
    .event = wifi_tui_event,
};

void solar_os_shell_cmd_wifi(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1) {
        if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term, "wifi: TUI is only available on the display shell");
            solar_os_shell_io_writeln(term, "usage: wifi status|on|off|scan|connect|disconnect|known|forget|ap|nat");
            return;
        }
        const esp_err_t err = solar_os_context_request_launch(ctx, &wifi_tui_app, 0, NULL);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "wifi: launch failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        wifi_print_status(term);
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        wifi_cmd_on(term);
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        const esp_err_t err = solar_os_wifi_stop();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi radio off");
        } else {
            solar_os_shell_io_printf(term, "wifi off failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "ap") == 0) {
        wifi_cmd_ap(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "nat") == 0) {
        wifi_cmd_nat(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: wifi scan");
            return;
        }
        wifi_cmd_scan(term);
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        wifi_cmd_connect(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        const esp_err_t err = solar_os_wifi_disconnect();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi disconnected");
        } else {
            solar_os_shell_io_printf(term, "wifi disconnect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "known") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: wifi known");
            return;
        }
        wifi_cmd_known(term);
        return;
    }

    if (strcmp(argv[1], "forget") == 0) {
        esp_err_t err;
        bool forgetting_all = false;
        if (argc == 2) {
            err = solar_os_wifi_forget();
        } else if (argc == 3 && strcmp(argv[2], "all") == 0) {
            forgetting_all = true;
            err = solar_os_wifi_forget_all();
        } else if (argc == 3) {
            err = solar_os_wifi_forget_ssid(argv[2]);
        } else {
            solar_os_shell_io_writeln(term, "usage: wifi forget [ssid|all]");
            return;
        }

        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term,
                                      forgetting_all ? "WiFi profiles forgotten" : "WiFi profile forgotten");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "wifi: profile not found");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "wifi: invalid SSID");
        } else {
            solar_os_shell_io_printf(term, "wifi forget failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    wifi_print_usage(term);
}

#if SOLAR_OS_PACKAGE_NET
static void ping_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: ping <host> [count]");
    solar_os_shell_io_writeln(term, "ESC stops a running ping");
}

static bool shell_read_escape_key(void *user)
{
    char chars[8];
    size_t count;

    (void)user;

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            if (chars[i] == SOLAR_OS_KEY_ESCAPE) {
                return true;
            }
        }
    }

    return false;
}

static void ping_print_event(const solar_os_net_ping_event_t *event, void *user)
{
    solar_os_shell_io_t *term = (solar_os_shell_io_t *)user;

    if (event == NULL || term == NULL) {
        return;
    }

    if (event->type == SOLAR_OS_NET_PING_REPLY) {
        solar_os_shell_io_printf(term,
                                 "%" PRIu32 "B from %s seq=%u ttl=%u time=%" PRIu32 "ms\n",
                                 event->bytes,
                                 event->from,
                                 (unsigned)event->seqno,
                                 (unsigned)event->ttl,
                                 event->elapsed_ms);
    } else {
        solar_os_shell_io_printf(term,
                                 "timeout from %s seq=%u\n",
                                 event->from,
                                 (unsigned)event->seqno);
    }
    solar_os_shell_io_flush(term);
}

void solar_os_shell_cmd_ping(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    size_t count = SOLAR_OS_NET_PING_FOREVER;

    if (argc < 2 || argc > 3) {
        ping_print_usage(term);
        return;
    }

    if (argc == 3 &&
        !parse_size_arg(argv[2], 1, SOLAR_OS_NET_PING_MAX_COUNT, &count)) {
        solar_os_shell_io_printf(term,
                                 "ping count: 1..%u\n",
                                 (unsigned)SOLAR_OS_NET_PING_MAX_COUNT);
        return;
    }

    const char *host = argv[1];
    solar_os_net_ping_options_t options = {
        .count = (uint32_t)count,
    };
    solar_os_net_ping_result_t result;

    if (count == SOLAR_OS_NET_PING_FOREVER) {
        solar_os_shell_io_printf(term, "ping %s, ESC to stop\n", host);
    } else {
        solar_os_shell_io_printf(term, "ping %s (%u packets)\n", host, (unsigned)count);
    }
    solar_os_shell_io_flush(term);

    const esp_err_t err = solar_os_net_ping(host,
                                            &options,
                                            ping_print_event,
                                            term,
                                            shell_read_escape_key,
                                            NULL,
                                            &result);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ping: WiFi not connected");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "ping: unknown host: %s\n", host);
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        ping_print_usage(term);
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ping failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (result.interrupted) {
        solar_os_shell_io_writeln(term, "ping: stopped");
    }
    solar_os_shell_io_printf(term,
                             "%" PRIu32 " tx, %" PRIu32 " rx, %" PRIu32 "%% loss, %" PRIu32 "ms\n",
                             result.transmitted,
                             result.received,
                             result.loss_percent,
                             result.total_time_ms);
    if (result.received > 0) {
        solar_os_shell_io_printf(term,
                                 "rtt min/avg/max %" PRIu32 "/%" PRIu32 "/%" PRIu32 " ms\n",
                                 result.min_time_ms,
                                 result.avg_time_ms,
                                 result.max_time_ms);
    }
}

static void netscan_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: netscan <host|range> [ports]");
    solar_os_shell_io_writeln(term, "  target: host, 192.168.1.20, 192.168.1.1-32, 192.168.1.0/24");
    solar_os_shell_io_writeln(term, "  ports: 22,80,443 or 1-128");
    solar_os_shell_io_writeln(term, "  default: 22,80,443,1883,8080");
    solar_os_shell_io_writeln(term, "ESC stops a running scan");
}

typedef struct {
    bool range;
    char label[SOLAR_OS_NET_HOST_MAX];
    char single_ip[SOLAR_OS_NET_ADDR_MAX];
    uint8_t prefix[3];
    uint8_t first;
    uint8_t last;
    size_t count;
} netscan_target_spec_t;

static bool netscan_parse_ipv4_octet(const char **cursor, uint8_t *octet)
{
    if (cursor == NULL || *cursor == NULL || octet == NULL || !isdigit((unsigned char)**cursor)) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(*cursor, &end, 10);
    if (errno != 0 || end == *cursor || parsed > 255UL) {
        return false;
    }

    *octet = (uint8_t)parsed;
    *cursor = end;
    return true;
}

static bool netscan_parse_ipv4_address(const char *text, uint8_t octets[4], const char **end)
{
    if (text == NULL || octets == NULL) {
        return false;
    }

    const char *cursor = text;
    for (size_t i = 0; i < 4; i++) {
        if (!netscan_parse_ipv4_octet(&cursor, &octets[i])) {
            return false;
        }
        if (i < 3) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        }
    }

    if (end != NULL) {
        *end = cursor;
    }
    return true;
}

static void netscan_format_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    snprintf(buffer,
             len,
             "%u.%u.%u.%u",
             (unsigned)a,
             (unsigned)b,
             (unsigned)c,
             (unsigned)d);
}

static bool netscan_target_set_range(netscan_target_spec_t *target,
                                     const uint8_t octets[4],
                                     uint8_t first,
                                     uint8_t last)
{
    if (target == NULL || octets == NULL || last < first) {
        return false;
    }

    const size_t count = (size_t)last - (size_t)first + 1U;
    if (count == 0 || count > NETSCAN_MAX_HOSTS) {
        return false;
    }

    target->range = true;
    target->prefix[0] = octets[0];
    target->prefix[1] = octets[1];
    target->prefix[2] = octets[2];
    target->first = first;
    target->last = last;
    target->count = count;
    snprintf(target->label,
             sizeof(target->label),
             "%u.%u.%u.%u-%u",
             (unsigned)target->prefix[0],
             (unsigned)target->prefix[1],
             (unsigned)target->prefix[2],
             (unsigned)first,
             (unsigned)last);
    return true;
}

static esp_err_t netscan_parse_target(const char *text, netscan_target_spec_t *target)
{
    if (text == NULL || text[0] == '\0' || target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(target, 0, sizeof(*target));
    strlcpy(target->label, text, sizeof(target->label));

    uint8_t octets[4] = {0};
    const char *end = NULL;
    if (netscan_parse_ipv4_address(text, octets, &end)) {
        if (*end == '\0') {
            target->range = false;
            target->count = 1;
            netscan_format_ipv4(octets[0],
                                octets[1],
                                octets[2],
                                octets[3],
                                target->single_ip,
                                sizeof(target->single_ip));
            return ESP_OK;
        }

        if (*end == '/') {
            char *mask_end = NULL;
            errno = 0;
            unsigned long mask = strtoul(end + 1, &mask_end, 10);
            if (errno != 0 || mask_end == end + 1 || *mask_end != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            if (mask != 24UL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            return netscan_target_set_range(target, octets, 1, 254) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }

        if (*end == '-') {
            const char *range_end = NULL;
            uint8_t last_octets[4] = {0};
            if (netscan_parse_ipv4_address(end + 1, last_octets, &range_end)) {
                if (*range_end != '\0' ||
                    last_octets[0] != octets[0] ||
                    last_octets[1] != octets[1] ||
                    last_octets[2] != octets[2]) {
                    return ESP_ERR_INVALID_ARG;
                }
                return netscan_target_set_range(target, octets, octets[3], last_octets[3])
                           ? ESP_OK
                           : ESP_ERR_INVALID_SIZE;
            }

            const char *last_cursor = end + 1;
            uint8_t last = 0;
            if (!netscan_parse_ipv4_octet(&last_cursor, &last) || *last_cursor != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            return netscan_target_set_range(target, octets, octets[3], last)
                       ? ESP_OK
                       : ESP_ERR_INVALID_SIZE;
        }

        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t resolve_err = solar_os_net_resolve_host(text,
                                                            target->single_ip,
                                                            sizeof(target->single_ip));
    if (resolve_err != ESP_OK) {
        return resolve_err;
    }
    target->range = false;
    target->count = 1;
    return ESP_OK;
}

static void netscan_target_ip(const netscan_target_spec_t *target,
                              size_t index,
                              char *ip,
                              size_t ip_len)
{
    if (target == NULL || ip == NULL || ip_len == 0) {
        return;
    }

    if (!target->range) {
        strlcpy(ip, target->single_ip, ip_len);
        return;
    }

    const uint8_t host = (uint8_t)((size_t)target->first + index);
    netscan_format_ipv4(target->prefix[0],
                        target->prefix[1],
                        target->prefix[2],
                        host,
                        ip,
                        ip_len);
}

static bool netscan_parse_port_value(const char *text, char **end, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }

    errno = 0;
    unsigned long parsed = strtoul(text, end, 10);
    if (errno != 0 || *end == text || parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }

    *port = (uint16_t)parsed;
    return true;
}

static bool netscan_add_port(uint16_t *ports, size_t *count, uint16_t port)
{
    if (ports == NULL || count == NULL || port == 0) {
        return false;
    }

    for (size_t i = 0; i < *count; i++) {
        if (ports[i] == port) {
            return true;
        }
    }
    if (*count >= NETSCAN_MAX_PORTS) {
        return false;
    }

    ports[(*count)++] = port;
    return true;
}

static bool netscan_parse_ports(const char *text, uint16_t *ports, size_t *count)
{
    if (ports == NULL || count == NULL) {
        return false;
    }

    *count = 0;
    if (text == NULL || text[0] == '\0') {
        static const uint16_t defaults[] = {22, 80, 443, 1883, 8080};
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            if (!netscan_add_port(ports, count, defaults[i])) {
                return false;
            }
        }
        return true;
    }

    char buffer[192];
    if (strlcpy(buffer, text, sizeof(buffer)) >= sizeof(buffer)) {
        return false;
    }

    char *saveptr = NULL;
    char *token = strtok_r(buffer, ",", &saveptr);
    while (token != NULL) {
        uint16_t first = 0;
        uint16_t last = 0;
        char *end = NULL;
        if (!netscan_parse_port_value(token, &end, &first)) {
            return false;
        }
        if (*end == '-') {
            if (!netscan_parse_port_value(end + 1, &end, &last) || last < first) {
                return false;
            }
        } else {
            last = first;
        }
        if (*end != '\0') {
            return false;
        }

        for (uint32_t port = first; port <= last; port++) {
            if (!netscan_add_port(ports, count, (uint16_t)port)) {
                return false;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    return *count > 0;
}

static const char *netscan_service_name(uint16_t port)
{
    switch (port) {
    case 21:
        return "ftp";
    case 22:
        return "ssh";
    case 23:
        return "telnet";
    case 25:
        return "smtp";
    case 53:
        return "dns";
    case 80:
        return "http";
    case 110:
        return "pop3";
    case 143:
        return "imap";
    case 443:
        return "https";
    case 587:
        return "submission";
    case 993:
        return "imaps";
    case 995:
        return "pop3s";
    case 1883:
        return "mqtt";
    case 3306:
        return "mysql";
    case 5432:
        return "postgres";
    case 8080:
        return "http-alt";
    default:
        return "";
    }
}

static bool netscan_probe_tcp(const char *ip, uint16_t port, uint32_t timeout_ms, uint32_t *elapsed_ms)
{
    if (elapsed_ms != NULL) {
        *elapsed_ms = 0;
    }

    const TickType_t start = xTaskGetTickCount();
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {
            .s_addr = inet_addr(ip),
        },
    };

    bool open = false;
    int rc = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        open = true;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        struct timeval timeout = {
            .tv_sec = (time_t)(timeout_ms / 1000U),
            .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
        };

        rc = select(sock + 1, NULL, &writefds, NULL, &timeout);
        if (rc > 0 && FD_ISSET(sock, &writefds)) {
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);
            if (getsockopt(sock,
                           SOL_SOCKET,
                           SO_ERROR,
                           &so_error,
                           &so_error_len) == 0 &&
                so_error == 0) {
                open = true;
            }
        }
    }

    close(sock);
    if (elapsed_ms != NULL) {
        *elapsed_ms = (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
    }
    return open;
}

static void netscan_update_progress(solar_os_shell_io_t *term,
                                    size_t row,
                                    const char *ip,
                                    uint16_t port,
                                    size_t probe_index,
                                    size_t total_probes)
{
    static const char frames[] = "|/-\\";
    const char frame = frames[probe_index % (sizeof(frames) - 1U)];

    solar_os_shell_io_set_cursor(term, row, 0);
    solar_os_shell_io_clear_line_from(term, row, 0);
    solar_os_shell_io_printf(term,
                             "%c %s:%u %u/%u",
                             frame,
                             ip,
                             (unsigned)port,
                             (unsigned)(probe_index + 1U),
                             (unsigned)total_probes);
    solar_os_shell_io_flush(term);
}

static void netscan_clear_progress(solar_os_shell_io_t *term, size_t row)
{
    solar_os_shell_io_set_cursor(term, row, 0);
    solar_os_shell_io_clear_line_from(term, row, 0);
}

void solar_os_shell_cmd_netscan(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2 || argc > 3) {
        netscan_print_usage(term);
        return;
    }

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip) {
        solar_os_shell_io_writeln(term, "netscan: WiFi not connected");
        return;
    }

    uint16_t ports[NETSCAN_MAX_PORTS];
    size_t port_count = 0;
    if (!netscan_parse_ports(argc == 3 ? argv[2] : NULL, ports, &port_count)) {
        solar_os_shell_io_printf(term,
                                 "netscan: invalid ports or too many ports, max %u\n",
                                 (unsigned)NETSCAN_MAX_PORTS);
        return;
    }

    netscan_target_spec_t target;
    const esp_err_t target_err = netscan_parse_target(argv[1], &target);
    if (target_err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "netscan: unknown host: %s\n", argv[1]);
        return;
    }
    if (target_err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "netscan: only IPv4 /24 ranges are supported");
        return;
    }
    if (target_err == ESP_ERR_INVALID_SIZE) {
        solar_os_shell_io_printf(term,
                                 "netscan: too many hosts, max %u\n",
                                 (unsigned)NETSCAN_MAX_HOSTS);
        return;
    }
    if (target_err != ESP_OK) {
        solar_os_shell_io_printf(term, "netscan: invalid target: %s\n", esp_err_to_name(target_err));
        return;
    }

    char first_ip[SOLAR_OS_NET_ADDR_MAX];
    netscan_target_ip(&target, 0, first_ip, sizeof(first_ip));
    solar_os_shell_io_printf(term,
                             "netscan %s (%s), %u host%s, %u ports, ESC to stop\n",
                             target.label,
                             first_ip,
                             (unsigned)target.count,
                             target.count == 1 ? "" : "s",
                             (unsigned)port_count);
    solar_os_shell_io_writeln(term, "HOST             PORT     STATE  SERVICE");
    solar_os_shell_io_flush(term);

    size_t open_count = 0;
    size_t probe_count = 0;
    size_t progress_row = solar_os_shell_io_cursor_row(term);
    const size_t total_probes = target.count * port_count;
    const bool cursor_was_visible = solar_os_shell_io_cursor_visible(term);
    solar_os_shell_io_set_cursor_visible(term, false);
    bool stopped = false;
    for (size_t host_index = 0; host_index < target.count; host_index++) {
        char ip[SOLAR_OS_NET_ADDR_MAX];
        netscan_target_ip(&target, host_index, ip, sizeof(ip));

        for (size_t port_index = 0; port_index < port_count; port_index++) {
            if (shell_read_escape_key(NULL)) {
                stopped = true;
                break;
            }

            uint32_t elapsed_ms = 0;
            const uint16_t port = ports[port_index];
            netscan_update_progress(term, progress_row, ip, port, probe_count, total_probes);
            probe_count++;
            if (netscan_probe_tcp(ip, port, NETSCAN_TIMEOUT_MS, &elapsed_ms)) {
                netscan_clear_progress(term, progress_row);
                solar_os_shell_io_printf(term,
                                         "%-15s %-8u open   %s",
                                         ip,
                                         (unsigned)port,
                                         netscan_service_name(port));
                if (elapsed_ms > 0) {
                    solar_os_shell_io_printf(term, " (%" PRIu32 "ms)", elapsed_ms);
                }
                solar_os_shell_io_put_char(term, '\n');
                solar_os_shell_io_flush(term);
                open_count++;
                progress_row = solar_os_shell_io_cursor_row(term);
            }

            vTaskDelay(1);
        }
        if (stopped) {
            break;
        }
    }

    netscan_clear_progress(term, progress_row);
    if (stopped) {
        solar_os_shell_io_writeln(term, "netscan: stopped");
    }
    solar_os_shell_io_printf(term,
                             "netscan: %u open, %u probes\n",
                             (unsigned)open_count,
                             (unsigned)probe_count);
    solar_os_shell_io_set_cursor_visible(term, cursor_was_visible);
}

static void mqtt_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  mqtt status");
    solar_os_shell_io_writeln(term, "  mqtt connect [mqtt[s]://host[:port] [username [password]]]");
    solar_os_shell_io_writeln(term, "  mqtt disconnect");
    solar_os_shell_io_writeln(term, "  mqtt publish <topic> <payload> [qos] [retain]");
    solar_os_shell_io_writeln(term, "  mqtt subscribe <topic> [qos]");
}

static bool mqtt_parse_qos(const char *text, int *qos)
{
    size_t value = 0;

    if (qos == NULL || !parse_size_arg(text, 0, 2, &value)) {
        return false;
    }

    *qos = (int)value;
    return true;
}

static bool mqtt_parse_retain(const char *text, bool *retain)
{
    if (text == NULL || retain == NULL) {
        return false;
    }
    if (strcmp(text, "1") == 0 ||
        strcmp(text, "true") == 0 ||
        strcmp(text, "on") == 0 ||
        strcmp(text, "retain") == 0) {
        *retain = true;
        return true;
    }
    if (strcmp(text, "0") == 0 ||
        strcmp(text, "false") == 0 ||
        strcmp(text, "off") == 0) {
        *retain = false;
        return true;
    }
    return false;
}

static bool mqtt_read_stop_key(void)
{
    char chars[8];
    size_t count;

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            if (chars[i] == SOLAR_OS_KEY_ESCAPE ||
                chars[i] == SOLAR_OS_KEY_APP_EXIT ||
                chars[i] == 'q') {
                return true;
            }
        }
    }

    return false;
}

static void mqtt_print_status(solar_os_shell_io_t *term)
{
    solar_os_mqtt_status_t status;
    const esp_err_t err = solar_os_mqtt_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt status failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "MQTT: %s%s\n",
                             status.running ? "on" : "off",
                             status.connected ? ", connected" : "");
    solar_os_shell_io_printf(term,
                             "Broker: %s\n",
                             status.configured ? status.url : "(not configured)");
    solar_os_shell_io_printf(term,
                             "Auth: user %s, password %s\n",
                             status.username_set ? status.username : "none",
                             status.password_set ? "set" : "none");
    solar_os_shell_io_printf(term, "Client: %s\n", status.client_id);
    solar_os_shell_io_printf(term,
                             "Messages: rx %" PRIu32 ", tx %" PRIu32 ", queued %u, dropped %" PRIu32 "\n",
                             status.rx_count,
                             status.tx_count,
                             (unsigned)status.queued_messages,
                             status.dropped_count);
    if (status.last_error[0] != '\0') {
        solar_os_shell_io_printf(term,
                                 "Last error: %s (%s)\n",
                                 status.last_error,
                                 esp_err_to_name(status.last_esp_error));
    }
}

static void mqtt_wait_and_print_connect(solar_os_shell_io_t *term)
{
    for (int i = 0; i < 50; i++) {
        solar_os_mqtt_status_t status;
        if (solar_os_mqtt_get_status(&status) == ESP_OK) {
            if (status.connected) {
                solar_os_shell_io_writeln(term, "mqtt: connected");
                return;
            }
            if (status.last_error[0] != '\0') {
                solar_os_shell_io_printf(term, "mqtt: %s\n", status.last_error);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    solar_os_shell_io_writeln(term, "mqtt: connecting");
}

static void mqtt_cmd_connect(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc > 5) {
        solar_os_shell_io_writeln(term, "usage: mqtt connect [url [username [password]]]");
        return;
    }

    const char *url = argc >= 3 ? argv[2] : NULL;
    const char *username = argc >= 4 ? argv[3] : NULL;
    const char *password = argc >= 5 ? argv[4] : NULL;
    const esp_err_t err = solar_os_mqtt_connect(url, username, password);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "mqtt: no saved broker URL");
        solar_os_shell_io_writeln(term, "usage: mqtt connect mqtt[s]://host[:port] [username [password]]");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "mqtt: invalid URL, username, or password");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt connect failed: %s\n", esp_err_to_name(err));
        return;
    }

    mqtt_wait_and_print_connect(term);
}

static void mqtt_cmd_publish(solar_os_shell_io_t *term, int argc, char **argv)
{
    int qos = 0;
    bool retain = false;

    if (argc < 4 ||
        argc > 6 ||
        (argc >= 5 && !mqtt_parse_qos(argv[4], &qos)) ||
        (argc >= 6 && !mqtt_parse_retain(argv[5], &retain))) {
        solar_os_shell_io_writeln(term, "usage: mqtt publish <topic> <payload> [qos] [retain]");
        solar_os_shell_io_writeln(term, "qos: 0..2, retain: 0|1|on|off|retain");
        return;
    }

    int msg_id = 0;
    const char *payload = argv[3];
    const esp_err_t err = solar_os_mqtt_publish(argv[2],
                                                payload,
                                                strlen(payload),
                                                qos,
                                                retain,
                                                &msg_id);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "mqtt: not connected");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "mqtt: invalid topic, payload, qos, or retain flag");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt publish failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term, "mqtt publish: msg %d\n", msg_id);
}

static void mqtt_print_message(solar_os_shell_io_t *term, const solar_os_mqtt_message_t *message)
{
    solar_os_shell_io_printf(term,
                             "%s %.*s%s\n",
                             message->topic,
                             (int)message->payload_len,
                             message->payload,
                             message->truncated ? " ..." : "");
}

static void mqtt_cmd_subscribe(solar_os_shell_io_t *term, int argc, char **argv)
{
    int qos = 0;

    if (argc < 3 || argc > 4 || (argc == 4 && !mqtt_parse_qos(argv[3], &qos))) {
        solar_os_shell_io_writeln(term, "usage: mqtt subscribe <topic> [qos]");
        solar_os_shell_io_writeln(term, "qos: 0..2");
        return;
    }

    int msg_id = 0;
    const esp_err_t err = solar_os_mqtt_subscribe(argv[2], qos, &msg_id);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "mqtt: not connected");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "mqtt: invalid topic or qos");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt subscribe failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "mqtt subscribe: %s qos %d msg %d, ESC/q to stop display\n",
                             argv[2],
                             qos,
                             msg_id);
    solar_os_shell_io_flush(term);

    bool stopped = false;
    while (!stopped) {
        if (mqtt_read_stop_key()) {
            stopped = true;
            break;
        }

        solar_os_mqtt_message_t message;
        const esp_err_t read_err = solar_os_mqtt_read_message(&message, 250);
        if (read_err == ESP_OK) {
            mqtt_print_message(term, &message);
            solar_os_shell_io_flush(term);
        } else if (read_err != ESP_ERR_TIMEOUT) {
            solar_os_shell_io_printf(term, "mqtt read failed: %s\n", esp_err_to_name(read_err));
            break;
        }
    }

    solar_os_shell_io_writeln(term, "mqtt subscribe: display stopped");
}

void solar_os_shell_cmd_mqtt(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: mqtt status");
            return;
        }
        mqtt_print_status(term);
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        mqtt_cmd_connect(term, argc, argv);
    } else if (strcmp(argv[1], "disconnect") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: mqtt disconnect");
            return;
        }
        const esp_err_t err = solar_os_mqtt_disconnect();
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "mqtt disconnect failed: %s\n", esp_err_to_name(err));
            return;
        }
        solar_os_shell_io_writeln(term, "mqtt: disconnected");
    } else if (strcmp(argv[1], "publish") == 0) {
        mqtt_cmd_publish(term, argc, argv);
    } else if (strcmp(argv[1], "subscribe") == 0) {
        mqtt_cmd_subscribe(term, argc, argv);
    } else {
        mqtt_print_usage(term);
    }
}
#endif

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
    solar_os_shell_io_printf(term,
                             "I2S: port %d mclk %d bclk %d ws %d din %d dout %d\n",
                             status.i2s_port,
                             status.mclk_pin,
                             status.bclk_pin,
                             status.ws_pin,
                             status.din_pin,
                             status.dout_pin);
    solar_os_shell_io_printf(term, "PA pin: %d\n", status.pa_pin);
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
    uint8_t volume = 50;

    if (argc > 5 ||
        (argc >= 3 && !audio_parse_frequency(argv[2], &frequency_hz)) ||
        (argc >= 4 && !audio_parse_duration(argv[3], &duration_ms)) ||
        (argc >= 5 && !parse_u8(argv[4], &volume)) ||
        volume > 100) {
        solar_os_shell_io_writeln(term, "usage: audio tone [hz] [ms] [volume]");
        solar_os_shell_io_printf(term,
                                 "hz: %u..%u, ms: 1..%u, volume: 0..100\n",
                                 (unsigned)SOLAR_OS_AUDIO_TONE_MIN_HZ,
                                 (unsigned)SOLAR_OS_AUDIO_TONE_MAX_HZ,
                                 (unsigned)SOLAR_OS_AUDIO_TEST_MAX_MS);
        return;
    }

    solar_os_shell_io_printf(term,
                             "tone: %" PRIu32 " Hz %" PRIu32 " ms volume %u\n",
                             frequency_hz,
                             duration_ms,
                             (unsigned)volume);
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
        solar_os_audio_deinit();
        solar_os_shell_io_writeln(term, "audio: off");
    } else {
        audio_print_usage(term);
    }
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
        solar_os_shell_io_printf(term, "log status failed: %s\n", esp_err_to_name(err));
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
        solar_os_shell_io_printf(term, "log show failed: %s\n", esp_err_to_name(err));
        return;
    }

    size_t count = status.count < LOG_SHOW_DEFAULT ? status.count : LOG_SHOW_DEFAULT;
    if (argc == 3) {
        if (!parse_size_arg(argv[2], 1, status.capacity, &count)) {
            solar_os_shell_io_printf(term, "log show count: 1..%u\n", (unsigned)status.capacity);
            return;
        }
    } else if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: log show [count]");
        return;
    }

    if (count == 0) {
        solar_os_shell_io_writeln(term, "logs: empty");
        return;
    }

    solar_os_log_entry_t *entries =
        heap_caps_malloc(sizeof(*entries) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (entries == NULL) {
        entries = heap_caps_malloc(sizeof(*entries) * count, MALLOC_CAP_8BIT);
    }
    if (entries == NULL) {
        solar_os_shell_io_writeln(term, "log show: no memory");
        return;
    }

    size_t total = 0;
    const size_t copied = solar_os_log_snapshot(entries, count, &total);
    if (copied == 0) {
        solar_os_shell_io_writeln(term, "logs: empty");
        free(entries);
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

    free(entries);
}

static void log_cmd_follow(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    solar_os_log_level_t level = SOLAR_OS_LOG_LEVEL_INFO;

    if (argc == 2) {
        solar_os_log_status_t status;
        const esp_err_t err = solar_os_log_get_status(&status);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log follow failed: %s\n", esp_err_to_name(err));
            return;
        }
        level = status.level;
    } else if (argc == 3) {
        if (!solar_os_log_parse_level(argv[2], &level)) {
            solar_os_shell_io_writeln(term, "levels: error warn info debug");
            return;
        }
    } else {
        solar_os_shell_io_writeln(term, "usage: log follow [error|warn|info|debug]");
        return;
    }

    const esp_err_t err = solar_os_shell_session_start_log_follow(ctx, level);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "log follow: another foreground shell mode is active");
    } else if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "log follow failed: %s\n", esp_err_to_name(err));
    }
}

void solar_os_shell_cmd_log(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_io_writeln(term, "usage: log status");
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
            solar_os_shell_io_writeln(term, "usage: log clear");
            return;
        }
        const esp_err_t err = solar_os_log_clear();
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log clear failed: %s\n", esp_err_to_name(err));
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
                solar_os_shell_io_printf(term, "log level failed: %s\n", esp_err_to_name(err));
                return;
            }
            solar_os_shell_io_printf(term, "level: %s\n", solar_os_log_level_name(status.level));
            return;
        }
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: log level [error|warn|info|debug]");
            return;
        }

        solar_os_log_level_t level;
        if (!solar_os_log_parse_level(argv[2], &level)) {
            solar_os_shell_io_writeln(term, "levels: error warn info debug");
            return;
        }

        const esp_err_t err = solar_os_log_set_level(level);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log level failed: %s\n", esp_err_to_name(err));
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
                solar_os_shell_io_printf(term, "log sink failed: %s\n", esp_err_to_name(err));
                return;
            }
            solar_os_shell_io_printf(term, "cdc: %s\n", status.cdc_enabled ? "on" : "off");
            return;
        }
        if (argc != 4 || strcmp(argv[2], "cdc") != 0) {
            solar_os_shell_io_writeln(term, "usage: log sink cdc [on|off]");
            return;
        }

        bool enabled = false;
        if (!parse_on_off_arg(argv[3], &enabled)) {
            solar_os_shell_io_writeln(term, "cdc values: on off");
            return;
        }

        const esp_err_t err = solar_os_log_set_cdc_enabled(enabled);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "log sink failed: %s\n", esp_err_to_name(err));
            return;
        }
        solar_os_shell_io_printf(term, "cdc: %s\n", enabled ? "on" : "off");
        return;
    }

    log_print_usage(term);
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

    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_port_info_t info;
        const esp_err_t err = solar_os_port_get_info(argv[2], &info);
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_printf(term, "port: not found: %s\n", argv[2]);
            return;
        }
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "port status failed: %s\n", esp_err_to_name(err));
            return;
        }

        solar_os_shell_io_writeln(term, "name     cap owner      label");
        port_print_info(term, &info);
        return;
    }

    port_print_usage(term);
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
        xfer_print_usage(term);
        return false;
    }

    config->direction = argv[1];
    if (strcmp(argv[1], "protocols") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: xfer protocols");
            return false;
        }
        return true;
    }

    if (!xfer_is_send(config->direction) && !xfer_is_recv(config->direction)) {
        xfer_print_usage(term);
        return false;
    }
    if (argc < 4) {
        xfer_print_usage(term);
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
            solar_os_shell_io_printf(term, "xfer: unknown option: %s\n", argv[i]);
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

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            if ((uint8_t)chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    }

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
        xfer_print_usage(term);
        return;
    }

    solar_os_shell_io_printf(term, "xfer failed: %s\n", esp_err_to_name(err));
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
                             "GPIO%-2d %-8s %-6s %-6s pull %-4s",
                             info->pin,
                             info->runtime_allowed ? "user" : "reserved",
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

static void i2c_print_status(solar_os_shell_io_t *term)
{
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        solar_os_shell_io_writeln(term, "I2C: not available on this board");
        return;
    }
    solar_os_shell_io_printf(term,
                             "I2C: SDA %d, SCL %d, %" PRIu32 " Hz\n",
                             solar_os_i2c_get_sda_pin(),
                             solar_os_i2c_get_scl_pin(),
                             solar_os_i2c_get_speed_hz());
}

static void i2c_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  i2c scan");
    solar_os_shell_io_writeln(term, "  i2c probe <addr>");
    solar_os_shell_io_writeln(term, "  i2c read <addr> <reg> [len]");
    solar_os_shell_io_writeln(term, "  i2c write <addr> <reg> <byte...>");
    solar_os_shell_io_writeln(term, "  i2c status");
}

static void i2c_cmd_scan(solar_os_shell_io_t *term)
{
    size_t found = 0;

    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_I2C)) {
        solar_os_shell_io_writeln(term, "i2c: I2C hardware not available on this board");
        return;
    }

    solar_os_shell_io_writeln(term, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
    for (uint8_t row = 0; row < 0x80; row += 0x10) {
        solar_os_shell_io_printf(term, "%02x: ", row);
        for (uint8_t col = 0; col < 0x10; col++) {
            const uint8_t address = row + col;
            if (address < SOLAR_OS_I2C_SCAN_MIN_ADDR || address > SOLAR_OS_I2C_SCAN_MAX_ADDR) {
                solar_os_shell_io_write(term, "   ");
                continue;
            }

            const esp_err_t err = solar_os_i2c_probe(address);
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
    uint8_t address;
    if (argc != 3 || !parse_u8(argv[2], &address) ||
        address < SOLAR_OS_I2C_SCAN_MIN_ADDR || address > SOLAR_OS_I2C_SCAN_MAX_ADDR) {
        solar_os_shell_io_writeln(term, "usage: i2c probe <addr>");
        return;
    }

    const esp_err_t err = solar_os_i2c_probe(address);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "0x%02x: ACK\n", address);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "i2c: I2C hardware not available on this board");
    } else if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_printf(term, "0x%02x: bus busy\n", address);
    } else {
        solar_os_shell_io_printf(term, "0x%02x: no response (%s)\n", address, esp_err_to_name(err));
    }
}

static void i2c_cmd_read(solar_os_shell_io_t *term, int argc, char **argv)
{
    uint8_t address;
    uint8_t reg;
    size_t len = 1;

    if (argc < 4 ||
        argc > 5 ||
        !parse_u8(argv[2], &address) ||
        !parse_u8(argv[3], &reg) ||
        (argc == 5 && !parse_size_arg(argv[4], 1, I2C_READ_MAX_LEN, &len))) {
        solar_os_shell_io_writeln(term, "usage: i2c read <addr> <reg> [len]");
        return;
    }

    uint8_t data[I2C_READ_MAX_LEN];
    const esp_err_t err = solar_os_i2c_read_reg(address, reg, data, len);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "i2c", "I2C hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "i2c read failed: %s\n", esp_err_to_name(err));
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
    uint8_t address;
    uint8_t reg;
    uint8_t data[SOLAR_OS_SHELL_ARG_MAX - 3];

    if (argc < 5 ||
        !parse_u8(argv[2], &address) ||
        !parse_u8(argv[3], &reg)) {
        solar_os_shell_io_writeln(term, "usage: i2c write <addr> <reg> <byte...>");
        return;
    }

    const size_t len = (size_t)(argc - 4);
    for (size_t i = 0; i < len; i++) {
        if (!parse_u8(argv[i + 4], &data[i])) {
            solar_os_shell_io_writeln(term, "i2c write: byte values must be 0..255");
            return;
        }
    }

    const esp_err_t err = solar_os_i2c_write_reg(address, reg, data, len);
    if (err != ESP_OK) {
        if (shell_print_not_supported(term, "i2c", "I2C hardware", err)) {
            return;
        }
        solar_os_shell_io_printf(term, "i2c write failed: %s\n", esp_err_to_name(err));
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
        i2c_print_status(term);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        i2c_cmd_scan(term);
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

static void print_datetime_line(solar_os_shell_io_t *term,
                                const char *label,
                                const solar_os_datetime_t *datetime)
{
    solar_os_shell_io_printf(term,
                             "%s: %04u-%02u-%02u %02u:%02u:%02u\n",
                             label,
                             (unsigned)datetime->year,
                             (unsigned)datetime->month,
                             (unsigned)datetime->day,
                             (unsigned)datetime->hour,
                             (unsigned)datetime->minute,
                             (unsigned)datetime->second);
}

void solar_os_shell_cmd_ntp(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc > 2) {
        solar_os_shell_io_writeln(term, "usage: ntp [server]");
        return;
    }

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.has_ip) {
        solar_os_shell_io_writeln(term, "ntp: WiFi is not connected");
        return;
    }

    const char *server = argc == 2 ? argv[1] : SOLAR_OS_NTP_DEFAULT_SERVER;
    solar_os_shell_io_printf(term, "ntp: syncing with %s\n", server);

    solar_os_datetime_t utc;
    solar_os_datetime_t local;
    const esp_err_t err = solar_os_time_ntp_sync(server,
                                                 SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS,
                                                 &utc,
                                                 &local);
    if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_writeln(term, "ntp: sync timed out");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "usage: ntp [server]");
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "ntp: RTC not available on this board");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ntp: sync failed: %s\n", esp_err_to_name(err));
        return;
    }

    char timezone[SOLAR_OS_TIMEZONE_NAME_MAX];
    solar_os_time_get_timezone(timezone, sizeof(timezone), NULL, 0);
    print_datetime_line(term, "UTC", &utc);
    print_datetime_line(term, timezone, &local);
}

#if SOLAR_OS_PACKAGE_NET
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
        solar_os_shell_io_writeln(term, "sshkey: SD card is not mounted");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "sshkey: status failed: %s\n", esp_err_to_name(err));
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
        solar_os_shell_io_writeln(term, "sshkey: SD card is not mounted");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "sshkey: status failed: %s\n", esp_err_to_name(err));
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
            solar_os_shell_io_writeln(term, "usage: sshkey pub");
            return;
        }
        sshkey_print_public(term);
        return;
    }

    if (strcmp(argv[1], "rm") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: sshkey rm");
            return;
        }

        const esp_err_t err = solar_os_ssh_keys_remove_default();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "sshkey: removed");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "sshkey: no key to remove");
        } else if (err == ESP_ERR_INVALID_STATE) {
            solar_os_shell_io_writeln(term, "sshkey: SD card is not mounted");
        } else {
            solar_os_shell_io_printf(term, "sshkey: remove failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "gen") == 0) {
        bool overwrite = false;
        size_t bits = SOLAR_OS_SSH_KEY_DEFAULT_BITS;

        if (!solar_os_storage_is_mounted()) {
            solar_os_shell_io_writeln(term, "sshkey: SD card is not mounted");
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
                solar_os_shell_io_writeln(term, "usage: sshkey gen [-f] [2048|3072|4096]");
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
            solar_os_shell_io_printf(term, "sshkey: generate failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    sshkey_print_usage(term);
}
#endif

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
