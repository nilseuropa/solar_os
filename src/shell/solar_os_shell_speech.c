#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble_keyboard.h"
#endif
#include "solar_os_keys.h"
#include "solar_os_port.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_speech.h"
#include "solar_os_storage.h"

#define SAY_USAGE \
    "say [-v <0..100>] [--volume <0..100>] [--drop-if-busy] " \
    "([--force] --file <path> | [--] <text...>)"
#define SAY_FILE_WAIT_MS 25U
#define SAY_FILE_DEFAULT_MAX_BYTES (64U * 1024U)
#define SAY_PROGRESS_BAR_MAX 32U

typedef struct {
    uint32_t codepoint;
    uint32_t minimum;
    uint8_t remaining;
} say_utf8_state_t;

typedef struct {
    size_t row;
    uint16_t hundredths;
    bool row_valid;
    bool rendered;
} say_progress_t;

static bool say_append(char *text,
                       size_t text_capacity,
                       size_t *text_len,
                       const char *word)
{
    const size_t word_len = strlen(word);
    const size_t separator_len = *text_len > 0U ? 1U : 0U;
    if (*text_len + separator_len + word_len >= text_capacity) {
        return false;
    }
    if (separator_len != 0U) {
        text[(*text_len)++] = ' ';
    }
    memcpy(&text[*text_len], word, word_len);
    *text_len += word_len;
    text[*text_len] = '\0';
    return true;
}

static bool say_plain_text_codepoint(uint32_t codepoint)
{
    return codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
        (codepoint >= 0x20U && codepoint != 0x7fU &&
         !(codepoint >= 0x80U && codepoint <= 0x9fU));
}

static bool say_utf8_feed(say_utf8_state_t *state, uint8_t byte)
{
    if (state->remaining == 0U) {
        if (byte < 0x80U) {
            return say_plain_text_codepoint(byte);
        }
        if (byte >= 0xc2U && byte <= 0xdfU) {
            state->codepoint = byte & 0x1fU;
            state->minimum = 0x80U;
            state->remaining = 1U;
            return true;
        }
        if (byte >= 0xe0U && byte <= 0xefU) {
            state->codepoint = byte & 0x0fU;
            state->minimum = 0x800U;
            state->remaining = 2U;
            return true;
        }
        if (byte >= 0xf0U && byte <= 0xf4U) {
            state->codepoint = byte & 0x07U;
            state->minimum = 0x10000U;
            state->remaining = 3U;
            return true;
        }
        return false;
    }

    if ((byte & 0xc0U) != 0x80U) {
        return false;
    }
    state->codepoint = (state->codepoint << 6U) | (byte & 0x3fU);
    state->remaining--;
    if (state->remaining != 0U) {
        return true;
    }
    return state->codepoint >= state->minimum &&
        state->codepoint <= 0x10ffffU &&
        !(state->codepoint >= 0xd800U && state->codepoint <= 0xdfffU) &&
        say_plain_text_codepoint(state->codepoint);
}

static esp_err_t say_validate_plain_text(const char *text, size_t text_len)
{
    say_utf8_state_t utf8 = {0};
    for (size_t i = 0U; i < text_len; i++) {
        if (!say_utf8_feed(&utf8, (uint8_t)text[i])) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return utf8.remaining == 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool say_chunk_break(uint8_t byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
        byte == '.' || byte == ',' || byte == ';' || byte == ':' ||
        byte == '!' || byte == '?';
}

static size_t say_complete_utf8_prefix(const char *text, size_t length)
{
    say_utf8_state_t utf8 = {0};
    size_t complete = 0U;
    for (size_t i = 0U; i < length; i++) {
        if (!say_utf8_feed(&utf8, (uint8_t)text[i])) {
            break;
        }
        if (utf8.remaining == 0U) {
            complete = i + 1U;
        }
    }
    return complete;
}

static esp_err_t say_read_file_chunk(FILE *file,
                                     uint64_t bytes_done,
                                     uint64_t bytes_total,
                                     char *text,
                                     size_t *text_len,
                                     size_t *bytes_consumed,
                                     bool *eof)
{
    *text_len = 0U;
    *bytes_consumed = 0U;
    *eof = false;
    const size_t count = fread(text, 1U, SOLAR_OS_SPEECH_TEXT_MAX, file);
    if (count == 0U) {
        if (ferror(file)) {
            return ESP_FAIL;
        }
        *eof = true;
        return ESP_OK;
    }

    size_t chunk = count;
    if (bytes_done + count < bytes_total) {
        const size_t minimum = count / 2U;
        for (size_t i = count; i > minimum; i--) {
            if (say_chunk_break((uint8_t)text[i - 1U])) {
                chunk = i;
                break;
            }
        }
        if (chunk == count && !say_chunk_break((uint8_t)text[count - 1U])) {
            chunk = say_complete_utf8_prefix(text, count);
        }
    }
    if (chunk == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t validation = say_validate_plain_text(text, chunk);
    if (validation != ESP_OK) {
        return validation;
    }
    if (chunk < count && fseek(file, -(long)(count - chunk), SEEK_CUR) != 0) {
        return ESP_FAIL;
    }
    *bytes_consumed = chunk;

    size_t start = bytes_done == 0U && chunk >= 3U &&
            (uint8_t)text[0] == 0xefU &&
            (uint8_t)text[1] == 0xbbU &&
            (uint8_t)text[2] == 0xbfU ?
        3U : 0U;
    size_t end = chunk;
    while (start < end && (text[start] == ' ' || text[start] == '\t' ||
                           text[start] == '\r' || text[start] == '\n')) {
        start++;
    }
    while (end > start && (text[end - 1U] == ' ' || text[end - 1U] == '\t' ||
                           text[end - 1U] == '\r' || text[end - 1U] == '\n')) {
        end--;
    }
    *text_len = end - start;
    if (start > 0U && *text_len > 0U) {
        memmove(text, text + start, *text_len);
    }
    text[*text_len] = '\0';
    return ESP_OK;
}

static bool say_stop_requested(solar_os_shell_io_t *io)
{
    char chars[8];
    size_t count;
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0U) {
        for (size_t i = 0U; i < count; i++) {
            const uint8_t ch = (uint8_t)chars[i];
            if (ch == SOLAR_OS_KEY_ESCAPE || ch == 0x03U ||
                ch == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    }
#else
    (void)chars;
#endif

    if (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT ||
        !solar_os_port_handle_valid(&io->port)) {
        return false;
    }
    uint8_t port_chars[8];
    do {
        count = 0U;
        if (solar_os_port_read(&io->port,
                               port_chars,
                               sizeof(port_chars),
                               0U,
                               &count) != ESP_OK) {
            return false;
        }
        for (size_t i = 0U; i < count; i++) {
            if (port_chars[i] == SOLAR_OS_KEY_ESCAPE ||
                port_chars[i] == 0x03U || port_chars[i] == 0x1dU ||
                port_chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    } while (count > 0U);
    return false;
}

static void say_render_progress(solar_os_shell_io_t *io,
                                say_progress_t *progress,
                                uint64_t bytes_done,
                                uint64_t bytes_total)
{
    uint64_t calculated = bytes_total > 0U ?
        (bytes_done * 10000U) / bytes_total : 10000U;
    if (calculated > 10000U) {
        calculated = 10000U;
    }
    const uint16_t hundredths = (uint16_t)calculated;
    if (progress->rendered && progress->hundredths == hundredths) {
        return;
    }
    if (!progress->row_valid) {
        progress->row = solar_os_shell_io_cursor_row(io);
        progress->row_valid = true;
    }
    if (solar_os_shell_io_is_cursor_addressable(io)) {
        (void)solar_os_shell_io_set_cursor(io, progress->row, 0U);
        (void)solar_os_shell_io_clear_line_from(io, progress->row, 0U);
    } else if (progress->rendered) {
        (void)solar_os_shell_io_put_char(io, '\r');
    }

    size_t cols = solar_os_shell_io_cols(io);
    if (cols == 0U) {
        cols = 80U;
    }
    const size_t fixed = strlen("say: [] 100.00%") + 1U;
    size_t width = cols > fixed ? cols - fixed : 1U;
    if (width > SAY_PROGRESS_BAR_MAX) {
        width = SAY_PROGRESS_BAR_MAX;
    }
    const size_t filled = (hundredths * width) / 10000U;
    solar_os_shell_io_write(io, "say: [");
    for (size_t i = 0U; i < width; i++) {
        const char marker = i < filled ? '#' :
            (bytes_done > 0U && hundredths < 10000U && i == filled ? '>' : '-');
        solar_os_shell_io_put_char(io, marker);
    }
    solar_os_shell_io_printf(io,
                             "] %3u.%02u%%",
                             (unsigned)(hundredths / 100U),
                             (unsigned)(hundredths % 100U));
    solar_os_shell_io_flush(io);
    progress->hundredths = hundredths;
    progress->rendered = true;
}

static void say_finish_progress(solar_os_shell_io_t *io,
                                say_progress_t *progress)
{
    if (progress->rendered) {
        solar_os_shell_io_newline(io);
        solar_os_shell_io_flush(io);
    }
}

static esp_err_t say_enqueue_text(const char *text,
                                  size_t text_len,
                                  uint8_t volume,
                                  bool drop_if_busy,
                                  uint32_t *request_id)
{
    const solar_os_speech_request_t request = {
        .text = text,
        .text_len = text_len,
        .volume = volume,
        .drop_if_busy = drop_if_busy,
    };
    return solar_os_speech_enqueue(&request, request_id);
}

static void say_print_enqueue_error(solar_os_shell_io_t *io, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_speech_queue_status_t status;
        solar_os_speech_queue_get_status(&status);
        solar_os_shell_io_writeln(
            io,
            !status.running ?
                "say: speechd is not running; use "
                "'job start speechd <voice-directory>'" :
                "say: busy; request dropped");
    } else if (err == ESP_ERR_NO_MEM) {
        solar_os_shell_io_writeln(io, "say: queue full");
    } else {
        solar_os_shell_io_printf(
            io, "say failed: %s\n", solar_os_shell_error_text(err));
    }
}

static esp_err_t say_wait_for_request(solar_os_shell_io_t *io,
                                      uint32_t request_id,
                                      bool *stopped)
{
    bool seen = false;
    for (;;) {
        if (say_stop_requested(io)) {
            (void)solar_os_speech_cancel(request_id);
            *stopped = true;
            return ESP_ERR_TIMEOUT;
        }

        solar_os_speech_request_status_t request_status;
        if (solar_os_speech_request_status(request_id, &request_status)) {
            seen = true;
            switch (request_status.state) {
            case SOLAR_OS_SPEECH_REQUEST_COMPLETE:
                return ESP_OK;
            case SOLAR_OS_SPEECH_REQUEST_CANCELLED:
                return ESP_ERR_TIMEOUT;
            case SOLAR_OS_SPEECH_REQUEST_DROPPED:
                return ESP_ERR_INVALID_STATE;
            case SOLAR_OS_SPEECH_REQUEST_FAILED:
                return request_status.error != ESP_OK ?
                    request_status.error : ESP_FAIL;
            default:
                break;
            }
        } else if (seen) {
            return ESP_ERR_NOT_FOUND;
        }

        solar_os_speech_queue_status_t queue_status;
        solar_os_speech_queue_get_status(&queue_status);
        if (!queue_status.running) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(SAY_FILE_WAIT_MS));
    }
}

static void say_file(solar_os_context_t *ctx,
                     solar_os_shell_io_t *io,
                     const char *path_arg,
                     uint8_t volume,
                     bool drop_if_busy,
                     bool force)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(
            ctx, io, "say", path_arg, path, sizeof(path))) {
        return;
    }

    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        solar_os_shell_io_printf(io, "say: file not found: %s\n", path_arg);
        return;
    }
    if (info.st_size <= 0) {
        solar_os_shell_io_printf(io, "say: empty file: %s\n", path_arg);
        return;
    }
    if (!force && (uint64_t)info.st_size > SAY_FILE_DEFAULT_MAX_BYTES) {
        solar_os_shell_io_printf(
            io,
            "say: file too large: %s (%" PRIu64
            " bytes; default limit %u); use --force to read it anyway\n",
            path_arg,
            (uint64_t)info.st_size,
            (unsigned)SAY_FILE_DEFAULT_MAX_BYTES);
        return;
    }

    solar_os_speech_queue_status_t queue_status;
    solar_os_speech_queue_get_status(&queue_status);
    if (!queue_status.running) {
        say_print_enqueue_error(io, ESP_ERR_INVALID_STATE);
        return;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        solar_os_shell_io_printf(io, "say: cannot open file: %s\n", path_arg);
        return;
    }
    esp_err_t err = ESP_OK;

    const uint64_t bytes_total = (uint64_t)info.st_size;
    uint64_t bytes_done = 0U;
    bool stopped = false;
    say_progress_t progress = {.hundredths = UINT16_MAX};
    solar_os_shell_io_printf(
        io, "say: reading %s aloud; Esc or Ctrl+C stops\n", path_arg);
    say_render_progress(io, &progress, 0U, bytes_total);

    while (err == ESP_OK && !stopped) {
        if (say_stop_requested(io)) {
            stopped = true;
            break;
        }
        char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U];
        size_t text_len = 0U;
        size_t bytes_consumed = 0U;
        bool eof = false;
        err = say_read_file_chunk(file,
                                  bytes_done,
                                  bytes_total,
                                  text,
                                  &text_len,
                                  &bytes_consumed,
                                  &eof);
        if (err != ESP_OK || eof) {
            break;
        }
        if (text_len > 0U) {
            uint32_t request_id = 0U;
            err = say_enqueue_text(text,
                                   text_len,
                                   volume,
                                   drop_if_busy,
                                   &request_id);
            if (err != ESP_OK) {
                break;
            }
            err = say_wait_for_request(io, request_id, &stopped);
            if (err != ESP_OK) {
                break;
            }
        }
        bytes_done += bytes_consumed;
        say_render_progress(io, &progress, bytes_done, bytes_total);
        vTaskDelay(1);
    }
    const bool close_failed = fclose(file) != 0;
    say_finish_progress(io, &progress);

    if (stopped) {
        solar_os_shell_io_writeln(io, "say: stopped");
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        solar_os_shell_io_printf(
            io, "say: not a plain UTF-8 text file: %s\n", path_arg);
    } else if (err != ESP_OK) {
        say_print_enqueue_error(io, err);
    } else if (close_failed || bytes_done != bytes_total) {
        solar_os_shell_io_printf(io, "say: file changed while reading: %s\n", path_arg);
    } else {
        solar_os_shell_io_writeln(io, "say: complete");
    }
}

void solar_os_shell_cmd_say(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    uint8_t volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL;
    bool drop_if_busy = false;
    bool force = false;
    const char *file_arg = NULL;
    int text_index = argc;

    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (strcmp(arg, "--") == 0) {
            text_index = index + 1;
            break;
        }
        if (strcmp(arg, "--drop-if-busy") == 0) {
            drop_if_busy = true;
            index++;
            continue;
        }
        if (strcmp(arg, "--force") == 0) {
            force = true;
            index++;
            continue;
        }
        if (strcmp(arg, "--file") == 0) {
            if (index + 1 >= argc) {
                solar_os_shell_diag_missing(io, "say", "file", SAY_USAGE);
                return;
            }
            if (file_arg != NULL) {
                solar_os_shell_diag_invalid(io,
                                            "say",
                                            "option",
                                            "--file",
                                            "one --file option",
                                            SAY_USAGE,
                                            false);
                return;
            }
            file_arg = argv[index + 1];
            index += 2;
            continue;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--volume") == 0) {
            if (index + 1 >= argc) {
                solar_os_shell_diag_missing(io, "say", "volume", SAY_USAGE);
                return;
            }
            uint8_t parsed = 0U;
            if (!solar_os_shell_parse_u8(argv[index + 1], &parsed) ||
                parsed > 100U) {
                solar_os_shell_diag_invalid(io,
                                            "say",
                                            "volume",
                                            argv[index + 1],
                                            "0..100",
                                            SAY_USAGE,
                                            false);
                return;
            }
            volume = parsed;
            index += 2;
            continue;
        }
        if (arg[0] == '-') {
            solar_os_shell_diag_invalid(io,
                                        "say",
                                        "option",
                                        arg,
                                        "-v, --volume, --drop-if-busy, --force, "
                                        "--file, or --",
                                        SAY_USAGE,
                                        false);
            return;
        }
        text_index = index;
        break;
    }

    if (file_arg != NULL) {
        if (text_index < argc) {
            solar_os_shell_diag_problem(io,
                                        "say",
                                        "--file cannot be combined with text",
                                        SAY_USAGE,
                                        NULL);
            return;
        }
        say_file(ctx, io, file_arg, volume, drop_if_busy, force);
        return;
    }
    if (force) {
        solar_os_shell_diag_problem(io,
                                    "say",
                                    "--force requires --file",
                                    SAY_USAGE,
                                    NULL);
        return;
    }
    if (text_index >= argc) {
        solar_os_shell_diag_missing(io, "say", "text", SAY_USAGE);
        return;
    }

    char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U] = {0};
    size_t text_len = 0U;
    for (int i = text_index; i < argc; i++) {
        if (!say_append(text, sizeof(text), &text_len, argv[i])) {
            solar_os_shell_diag_invalid(io,
                                        "say",
                                        "text",
                                        "<text...>",
                                        "at most 512 UTF-8 bytes",
                                        SAY_USAGE,
                                        false);
            return;
        }
    }
    if (text_len == 0U) {
        solar_os_shell_diag_missing(io, "say", "text", SAY_USAGE);
        return;
    }

    uint32_t request_id = 0U;
    const esp_err_t err = say_enqueue_text(
        text, text_len, volume, drop_if_busy, &request_id);
    if (err != ESP_OK) {
        say_print_enqueue_error(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "say: queued request %" PRIu32 "\n", request_id);
}
