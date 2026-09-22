#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_audio.h"
#include "solar_os_keys.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_speech.h"
#include "solar_os_storage.h"

#define SAY_USAGE \
    "say [-v <0..100>] [--volume <0..100>] [--pitch <50..200>] " \
    "[--speed <20..500>] [--drop-if-busy] " \
    "(--file <path> | [--] <text...>)"
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

typedef struct {
    bool active;
    FILE *file;
    solar_os_shell_session_t *owner;
    solar_os_shell_io_t *io;
    uint64_t bytes_total;
    uint64_t bytes_done;
    uint64_t bytes_submitted;
    uint32_t request_id;
    bool final_submitted;
    say_progress_t progress;
    char path_arg[SOLAR_OS_STORAGE_PATH_MAX];
} say_file_playback_t;

static say_file_playback_t say_file_playback;

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

static bool say_parse_u16(const char *text, uint16_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
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

    const size_t start = bytes_done == 0U && chunk >= 3U &&
            (uint8_t)text[0] == 0xefU &&
            (uint8_t)text[1] == 0xbbU &&
            (uint8_t)text[2] == 0xbfU ?
        3U : 0U;
    *text_len = chunk - start;
    if (start > 0U && *text_len > 0U) {
        memmove(text, text + start, *text_len);
    }
    text[*text_len] = '\0';
    return ESP_OK;
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
            (i == filled && filled < width ? '>' : '-');
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
                                  uint16_t pitch,
                                  uint16_t speed,
                                  bool drop_if_busy,
                                  uint32_t *request_id)
{
    const solar_os_speech_request_t request = {
        .text = text,
        .text_len = text_len,
        .volume = volume,
        .pitch = pitch,
        .speed = speed,
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

static void say_file(solar_os_context_t *ctx,
                     solar_os_shell_io_t *io,
                     const char *path_arg,
                     uint8_t volume,
                     uint16_t pitch,
                     uint16_t speed,
                     bool drop_if_busy)
{
    if (say_file_playback.active) {
        solar_os_shell_io_writeln(io, "say: another file read is active");
        return;
    }
    solar_os_shell_session_t *owner = solar_os_context_shell_session(ctx);
    if (owner == NULL ||
        solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_writeln(
            io, "say: file reading requires an interactive shell session");
        return;
    }

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

    const solar_os_speech_stream_options_t stream_options = {
        .volume = volume,
        .pitch = pitch,
        .speed = speed,
        .drop_if_busy = drop_if_busy,
    };
    uint32_t request_id = 0U;
    const esp_err_t begin_err = solar_os_speech_stream_begin(
        &stream_options, &request_id);
    if (begin_err != ESP_OK) {
        (void)fclose(file);
        say_print_enqueue_error(io, begin_err);
        return;
    }

    memset(&say_file_playback, 0, sizeof(say_file_playback));
    say_file_playback.active = true;
    say_file_playback.file = file;
    say_file_playback.owner = owner;
    say_file_playback.io = io;
    say_file_playback.bytes_total = (uint64_t)info.st_size;
    say_file_playback.request_id = request_id;
    say_file_playback.progress.hundredths = UINT16_MAX;
    strlcpy(say_file_playback.path_arg,
            path_arg,
            sizeof(say_file_playback.path_arg));
    solar_os_shell_session_hold_prompt(ctx);
    solar_os_shell_io_printf(
        io, "say: reading %s aloud; Esc or Ctrl+C stops\n", path_arg);
    say_render_progress(io,
                        &say_file_playback.progress,
                        0U,
                        say_file_playback.bytes_total);
}

static void say_file_finish(solar_os_context_t *ctx,
                            bool stopped,
                            esp_err_t err)
{
    solar_os_shell_io_t *io = say_file_playback.io;
    solar_os_shell_session_t *owner = say_file_playback.owner;
    if (say_file_playback.request_id != 0U &&
        (stopped || err != ESP_OK || !say_file_playback.final_submitted)) {
        (void)solar_os_speech_cancel(say_file_playback.request_id);
    }
    const bool close_failed = say_file_playback.file != NULL &&
        fclose(say_file_playback.file) != 0;
    say_file_playback.file = NULL;
    say_file_playback.active = false;
    say_finish_progress(io, &say_file_playback.progress);

    if (stopped) {
        solar_os_shell_io_writeln(io, "say: stopped");
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        solar_os_shell_io_printf(
            io,
            "say: not a plain UTF-8 text file: %s\n",
            say_file_playback.path_arg);
    } else if (err != ESP_OK) {
        say_print_enqueue_error(io, err);
    } else if (close_failed ||
               say_file_playback.bytes_done != say_file_playback.bytes_total) {
        solar_os_shell_io_printf(io,
                                 "say: file changed while reading: %s\n",
                                 say_file_playback.path_arg);
    } else {
        solar_os_shell_io_writeln(io, "say: complete");
    }
    if (owner != NULL) {
        solar_os_shell_session_prompt(ctx, owner);
    }
    memset(&say_file_playback, 0, sizeof(say_file_playback));
}

static void say_file_step(solar_os_context_t *ctx)
{
    solar_os_speech_request_status_t status;
    if (solar_os_speech_request_status(
            say_file_playback.request_id, &status)) {
        if (status.state == SOLAR_OS_SPEECH_REQUEST_COMPLETE) {
            say_file_playback.request_id = 0U;
            say_file_finish(ctx, false, ESP_OK);
            return;
        }
        if (status.state == SOLAR_OS_SPEECH_REQUEST_CANCELLED) {
            say_file_playback.request_id = 0U;
            say_file_finish(ctx, true, ESP_ERR_TIMEOUT);
            return;
        }
        if (status.state == SOLAR_OS_SPEECH_REQUEST_DROPPED) {
            say_file_playback.request_id = 0U;
            say_file_finish(ctx, false, ESP_ERR_INVALID_STATE);
            return;
        }
        if (status.state == SOLAR_OS_SPEECH_REQUEST_FAILED) {
            say_file_playback.request_id = 0U;
            say_file_finish(ctx,
                            false,
                            status.error != ESP_OK ? status.error : ESP_FAIL);
            return;
        }
    } else {
        solar_os_speech_queue_status_t queue_status;
        solar_os_speech_queue_get_status(&queue_status);
        if (!queue_status.running) {
            say_file_finish(ctx, false, ESP_ERR_INVALID_STATE);
        }
        return;
    }
    if (say_file_playback.final_submitted) {
        return;
    }

    char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U];
    size_t text_len = 0U;
    size_t bytes_consumed = 0U;
    bool eof = false;
    const esp_err_t err = say_read_file_chunk(say_file_playback.file,
                                               say_file_playback.bytes_done,
                                               say_file_playback.bytes_total,
                                               text,
                                               &text_len,
                                               &bytes_consumed,
                                               &eof);
    if (err != ESP_OK || eof) {
        say_file_finish(ctx,
                        false,
                        err != ESP_OK ? err : ESP_ERR_INVALID_SIZE);
        return;
    }
    const bool final =
        say_file_playback.bytes_done + bytes_consumed >=
        say_file_playback.bytes_total;
    if (text_len == 0U && !final) {
        say_file_playback.bytes_done += bytes_consumed;
        say_file_playback.bytes_submitted = say_file_playback.bytes_done;
        say_render_progress(say_file_playback.io,
                            &say_file_playback.progress,
                            say_file_playback.bytes_submitted,
                            say_file_playback.bytes_total);
        return;
    }

    const esp_err_t write_err = solar_os_speech_stream_write(
        say_file_playback.request_id,
        text_len > 0U ? text : NULL,
        text_len,
        final);
    if (write_err == ESP_ERR_NO_MEM) {
        if (fseek(say_file_playback.file, -(long)bytes_consumed, SEEK_CUR) != 0) {
            say_file_finish(ctx, false, ESP_FAIL);
        }
        return;
    }
    if (write_err != ESP_OK) {
        say_file_finish(ctx, false, write_err);
        return;
    }
    say_file_playback.bytes_done += bytes_consumed;
    say_file_playback.bytes_submitted = say_file_playback.bytes_done;
    say_file_playback.final_submitted = final;
    say_render_progress(say_file_playback.io,
                        &say_file_playback.progress,
                        say_file_playback.bytes_submitted,
                        say_file_playback.bytes_total);
}

bool solar_os_shell_speech_file_event(solar_os_context_t *ctx,
                                      const solar_os_event_t *event)
{
    if (!say_file_playback.active || ctx == NULL || event == NULL ||
        solar_os_context_shell_session(ctx) != say_file_playback.owner) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 0x03U ||
            ch == SOLAR_OS_KEY_APP_EXIT) {
            if (say_file_playback.request_id != 0U) {
                (void)solar_os_speech_cancel(say_file_playback.request_id);
            }
            say_file_finish(ctx, true, ESP_ERR_TIMEOUT);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        say_file_step(ctx);
    }
    return true;
}

bool solar_os_shell_speech_file_active(
    const solar_os_shell_session_t *session)
{
    return say_file_playback.active && session != NULL &&
        session == say_file_playback.owner;
}

void solar_os_shell_speech_file_session_destroyed(
    const solar_os_shell_session_t *session)
{
    if (!solar_os_shell_speech_file_active(session)) {
        return;
    }
    if (say_file_playback.request_id != 0U) {
        (void)solar_os_speech_cancel(say_file_playback.request_id);
    }
    if (say_file_playback.file != NULL) {
        (void)fclose(say_file_playback.file);
    }
    memset(&say_file_playback, 0, sizeof(say_file_playback));
}

void solar_os_shell_cmd_say(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    uint8_t volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL;
    uint16_t pitch = SOLAR_OS_SPEECH_PITCH_DEFAULT;
    uint16_t speed = SOLAR_OS_SPEECH_SPEED_DEFAULT;
    bool drop_if_busy = false;
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
        if (strcmp(arg, "--pitch") == 0) {
            if (index + 1 >= argc) {
                solar_os_shell_diag_missing(io, "say", "pitch", SAY_USAGE);
                return;
            }
            uint16_t parsed = 0U;
            if (!say_parse_u16(argv[index + 1], &parsed) ||
                parsed < SOLAR_OS_SPEECH_PITCH_MIN ||
                parsed > SOLAR_OS_SPEECH_PITCH_MAX) {
                solar_os_shell_diag_invalid(io,
                                            "say",
                                            "pitch",
                                            argv[index + 1],
                                            "50..200",
                                            SAY_USAGE,
                                            false);
                return;
            }
            pitch = parsed;
            index += 2;
            continue;
        }
        if (strcmp(arg, "--speed") == 0) {
            if (index + 1 >= argc) {
                solar_os_shell_diag_missing(io, "say", "speed", SAY_USAGE);
                return;
            }
            uint16_t parsed = 0U;
            if (!say_parse_u16(argv[index + 1], &parsed) ||
                parsed < SOLAR_OS_SPEECH_SPEED_MIN ||
                parsed > SOLAR_OS_SPEECH_SPEED_MAX) {
                solar_os_shell_diag_invalid(io,
                                            "say",
                                            "speed",
                                            argv[index + 1],
                                            "20..500",
                                            SAY_USAGE,
                                            false);
                return;
            }
            speed = parsed;
            index += 2;
            continue;
        }
        if (arg[0] == '-') {
            solar_os_shell_diag_invalid(io,
                                        "say",
                                        "option",
                                        arg,
                                        "-v, --volume, --pitch, --speed, "
                                        "--drop-if-busy, --file, or --",
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
        say_file(ctx,
                 io,
                 file_arg,
                 volume,
                 pitch,
                 speed,
                 drop_if_busy);
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
        text,
        text_len,
        volume,
        pitch,
        speed,
        drop_if_busy,
        &request_id);
    if (err != ESP_OK) {
        say_print_enqueue_error(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "say: queued request %" PRIu32 "\n", request_id);
}
