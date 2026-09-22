#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_audio.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_speech.h"

#define SAY_USAGE \
    "say [-v <0..100>] [--volume <0..100>] [--drop-if-busy] [--] <text...>"

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

void solar_os_shell_cmd_say(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    uint8_t volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL;
    bool drop_if_busy = false;
    int text_index = 1;

    while (text_index < argc) {
        const char *arg = argv[text_index];
        if (strcmp(arg, "--") == 0) {
            text_index++;
            break;
        }
        if (strcmp(arg, "--drop-if-busy") == 0) {
            drop_if_busy = true;
            text_index++;
            continue;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--volume") == 0) {
            if (text_index + 1 >= argc) {
                solar_os_shell_diag_missing(io, "say", "volume", SAY_USAGE);
                return;
            }
            uint8_t parsed = 0U;
            if (!solar_os_shell_parse_u8(argv[text_index + 1], &parsed) ||
                parsed > 100U) {
                solar_os_shell_diag_invalid(io,
                                            "say",
                                            "volume",
                                            argv[text_index + 1],
                                            "0..100",
                                            SAY_USAGE,
                                            false);
                return;
            }
            volume = parsed;
            text_index += 2;
            continue;
        }
        if (arg[0] == '-') {
            solar_os_shell_diag_invalid(io,
                                        "say",
                                        "option",
                                        arg,
                                        "-v, --volume, --drop-if-busy, or --",
                                        SAY_USAGE,
                                        false);
            return;
        }
        break;
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

    const solar_os_speech_request_t request = {
        .text = text,
        .text_len = text_len,
        .volume = volume,
        .drop_if_busy = drop_if_busy,
    };
    uint32_t request_id = 0U;
    const esp_err_t err = solar_os_speech_enqueue(&request, &request_id);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_speech_queue_status_t status;
        solar_os_speech_queue_get_status(&status);
        if (!status.running) {
            solar_os_shell_io_writeln(
                io, "say: speechd is not running; use 'job start speechd'");
        } else {
            solar_os_shell_io_writeln(io, "say: busy; request dropped");
        }
        return;
    }
    if (err == ESP_ERR_NO_MEM) {
        solar_os_shell_io_writeln(io, "say: queue full");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(
            io, "say failed: %s\n", solar_os_shell_error_text(err));
        return;
    }
    solar_os_shell_io_printf(io, "say: queued request %" PRIu32 "\n", request_id);
}
