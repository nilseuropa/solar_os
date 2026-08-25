#include "solar_os_shell_common.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static solar_os_shell_io_t shell_command_fallback_io;

solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx)
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

solar_os_terminal_t *solar_os_shell_display_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

bool solar_os_shell_print_not_supported(solar_os_shell_io_t *term,
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

bool solar_os_shell_parse_u8(const char *text, uint8_t *value)
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

bool solar_os_shell_parse_size_arg(const char *text,
                                   size_t min,
                                   size_t max,
                                   size_t *value)
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


const char *solar_os_shell_error_text(esp_err_t error)
{
    switch (error) {
    case ESP_OK:
        return "no error";
    case ESP_FAIL:
        return "operation failed";
    case ESP_ERR_NO_MEM:
        return "not enough memory; run 'mem policy' for details";
    case ESP_ERR_INVALID_ARG:
        return "invalid value or syntax";
    case ESP_ERR_INVALID_STATE:
        return "resource is busy or not ready";
    case ESP_ERR_INVALID_SIZE:
        return "value or data is too large";
    case ESP_ERR_NOT_FOUND:
        return "requested item was not found";
    case ESP_ERR_NOT_SUPPORTED:
        return "not supported by this board or firmware";
    case ESP_ERR_TIMEOUT:
        return "timed out; check the target and retry";
    case ESP_ERR_INVALID_RESPONSE:
        return "device or server returned an invalid response";
    case ESP_ERR_INVALID_CRC:
        return "data integrity check failed";
    default:
        return esp_err_to_name(error);
    }
}


void solar_os_shell_diag_problem(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *problem,
                                 const char *usage,
                                 const char *hint)
{
    if (io != NULL && io->diagnostic_source != NULL) {
        solar_os_shell_io_printf(io, "%s:%u: ", io->diagnostic_source,
                                 (unsigned)io->diagnostic_line);
    }
    solar_os_shell_io_write(io, command);
    solar_os_shell_io_write(io, ": ");
    solar_os_shell_io_writeln(io, problem);
    if (usage != NULL && usage[0] != '\0') {
        solar_os_shell_io_write(io, "usage: ");
        solar_os_shell_io_write(io, usage);
        if (usage[strlen(usage) - 1U] != '\n') {
            solar_os_shell_io_newline(io);
        }
    }
    if (hint != NULL && hint[0] != '\0') {
        solar_os_shell_io_write(io, "hint: ");
        solar_os_shell_io_write(io, hint);
        if (hint[strlen(hint) - 1U] != '\n') {
            solar_os_shell_io_newline(io);
        }
    }
}

void solar_os_shell_diag_set_source(solar_os_shell_io_t *io,
                                    const char *source,
                                    size_t line)
{
    if (io == NULL) {
        return;
    }
    io->diagnostic_source = source;
    io->diagnostic_line = source != NULL ? line : 0;
}

void solar_os_shell_diag_missing(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *argument,
                                 const char *usage)
{
    char problem[128];
    snprintf(problem, sizeof(problem), "missing argument %s", argument);
    solar_os_shell_diag_problem(io, command, problem, usage, NULL);
}

void solar_os_shell_diag_unexpected(solar_os_shell_io_t *io,
                                    const char *command,
                                    const char *argument,
                                    const char *usage)
{
    char problem[128];
    snprintf(problem, sizeof(problem), "unexpected argument '%.*s'", 72,
             argument != NULL ? argument : "");
    solar_os_shell_diag_problem(io, command, problem, usage, NULL);
}

void solar_os_shell_diag_invalid(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *argument,
                                 const char *value,
                                 const char *expected,
                                 const char *usage,
                                 bool sensitive)
{
    char problem[192];
    snprintf(problem,
             sizeof(problem),
             "invalid %s '%.*s'; expected %s",
             argument,
             sensitive ? 10 : 72,
             sensitive ? "<redacted>" : (value != NULL ? value : ""),
             expected);
    solar_os_shell_diag_problem(io, command, problem, usage, NULL);
}

void solar_os_shell_diag_unknown(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *kind,
                                 const char *value,
                                 const char *suggestion,
                                 const char *usage)
{
    char problem[160];
    char hint[128];
    snprintf(problem, sizeof(problem), "unknown %s '%.*s'", kind, 72,
             value != NULL ? value : "");
    if (suggestion != NULL) {
        snprintf(hint, sizeof(hint), "did you mean '%s'?", suggestion);
    }
    solar_os_shell_diag_problem(io,
                                command,
                                problem,
                                usage,
                                suggestion != NULL ? hint : NULL);
}

void solar_os_shell_diag_subcommand(solar_os_shell_io_t *io,
                                    const char *command,
                                    int argc,
                                    char **argv,
                                    const char *usage,
                                    const char * const *subcommands,
                                    size_t subcommand_count)
{
    if (argc < 2 || argv == NULL) {
        solar_os_shell_diag_missing(io, command, "<subcommand>", usage);
        return;
    }

    for (size_t i = 0; i < subcommand_count; i++) {
        if (strcmp(argv[1], subcommands[i]) == 0) {
            char problem[160];
            snprintf(problem,
                     sizeof(problem),
                     "invalid arguments for subcommand '%.*s'",
                     72,
                     argv[1]);
            solar_os_shell_diag_problem(io, command, problem, usage, NULL);
            return;
        }
    }

    const char *suggestion =
        solar_os_shell_suggest(argv[1], subcommands, subcommand_count);
    solar_os_shell_diag_unknown(io,
                                command,
                                "subcommand",
                                argv[1],
                                suggestion,
                                usage);
}

void solar_os_shell_diag_esp(solar_os_shell_io_t *io,
                             const char *operation,
                             esp_err_t err,
                             const char *detail,
                             const char *hint)
{
    const char *reason = NULL;
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        reason = "not found";
        break;
    case ESP_ERR_NOT_SUPPORTED:
        reason = "not supported by this firmware or board";
        break;
    case ESP_ERR_INVALID_STATE:
        reason = "not available in the current state";
        break;
    case ESP_ERR_TIMEOUT:
        reason = "timed out";
        break;
    case ESP_ERR_NO_MEM:
        reason = "not enough memory";
        break;
    case ESP_ERR_INVALID_SIZE:
        reason = "value or data is too large";
        break;
    case ESP_ERR_INVALID_ARG:
        reason = "invalid argument";
        break;
    default:
        break;
    }

    char problem[192];
    if (reason != NULL && detail != NULL && detail[0] != '\0') {
        snprintf(problem, sizeof(problem), "%s: %s", reason, detail);
    } else if (reason != NULL) {
        strlcpy(problem, reason, sizeof(problem));
    } else if (detail != NULL && detail[0] != '\0') {
        snprintf(problem, sizeof(problem), "%s (%s, 0x%x)", detail,
                 esp_err_to_name(err), (unsigned)err);
    } else {
        snprintf(problem, sizeof(problem), "operation failed (%s, 0x%x)",
                 esp_err_to_name(err), (unsigned)err);
    }
    solar_os_shell_diag_problem(io, operation, problem, NULL, hint);
}
