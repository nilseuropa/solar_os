#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell_parse.h"
#include "solar_os_terminal.h"

solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx);
solar_os_terminal_t *solar_os_shell_display_terminal(solar_os_context_t *ctx);
esp_err_t solar_os_shell_run_native_builtin(solar_os_context_t *ctx,
    int (*entry)(int, char **), int argc, char **argv);

bool solar_os_shell_print_not_supported(solar_os_shell_io_t *term,
                                        const char *command,
                                        const char *feature,
                                        esp_err_t err);

bool solar_os_shell_parse_u8(const char *text, uint8_t *value);
bool solar_os_shell_parse_size_arg(const char *text,
                                   size_t min,
                                   size_t max,
                                   size_t *value);

const char *solar_os_shell_error_text(esp_err_t error);
void solar_os_shell_diag_set_source(solar_os_shell_io_t *io,
                                    const char *source,
                                    size_t line);

void solar_os_shell_diag_problem(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *problem,
                                 const char *usage,
                                 const char *hint);
void solar_os_shell_diag_missing(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *argument,
                                 const char *usage);
void solar_os_shell_diag_unexpected(solar_os_shell_io_t *io,
                                    const char *command,
                                    const char *argument,
                                    const char *usage);
void solar_os_shell_diag_invalid(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *argument,
                                 const char *value,
                                 const char *expected,
                                 const char *usage,
                                 bool sensitive);
void solar_os_shell_diag_unknown(solar_os_shell_io_t *io,
                                 const char *command,
                                 const char *kind,
                                 const char *value,
                                 const char *suggestion,
                                 const char *usage);
void solar_os_shell_diag_subcommand(solar_os_shell_io_t *io,
                                    const char *command,
                                    int argc,
                                    char **argv,
                                    const char *usage,
                                    const char * const *subcommands,
                                    size_t subcommand_count);
void solar_os_shell_diag_esp(solar_os_shell_io_t *io,
                             const char *operation,
                             esp_err_t err,
                             const char *detail,
                             const char *hint);
