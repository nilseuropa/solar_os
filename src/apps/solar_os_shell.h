#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solar_os.h"
#include "solar_os_log.h"

typedef enum {
    SOLAR_OS_SHELL_STARTUP_FLASH = 0,
    SOLAR_OS_SHELL_STARTUP_SD = 1,
} solar_os_shell_startup_source_t;

const solar_os_app_t *solar_os_shell_app(void);

solar_os_shell_startup_source_t solar_os_shell_startup_source(void);
const char *solar_os_shell_startup_source_name(solar_os_shell_startup_source_t source);
bool solar_os_shell_parse_startup_source(const char *name,
                                         solar_os_shell_startup_source_t *source);
esp_err_t solar_os_shell_set_startup_source(solar_os_shell_startup_source_t source);
esp_err_t solar_os_shell_startup_path(char *path, size_t path_len);

solar_os_shell_session_t *solar_os_shell_session_create(void);
void solar_os_shell_session_destroy(solar_os_shell_session_t *session);
solar_os_shell_io_t *solar_os_shell_session_io(solar_os_shell_session_t *session);
const solar_os_app_t *solar_os_shell_session_foreground_app(solar_os_shell_session_t *session);
void solar_os_shell_session_set_foreground_app(solar_os_shell_session_t *session,
                                               const solar_os_app_t *app);
void solar_os_shell_session_set_exit_result(solar_os_shell_session_t *session,
                                            int exit_code,
                                            const char *message);
int solar_os_shell_session_last_exit_code(
    const solar_os_shell_session_t *session);
esp_err_t solar_os_shell_session_start(solar_os_context_t *ctx,
                                       solar_os_shell_session_t *session,
                                       solar_os_shell_io_t *io,
                                       bool preserve_terminal,
                                       bool run_startup);
bool solar_os_shell_session_event(solar_os_context_t *ctx,
                                  solar_os_shell_session_t *session,
                                  const solar_os_event_t *event);
esp_err_t solar_os_shell_session_submit_command(solar_os_context_t *ctx,
                                                solar_os_shell_session_t *session,
                                                const char *command);
void solar_os_shell_session_prompt(solar_os_context_t *ctx, solar_os_shell_session_t *session);
void solar_os_shell_session_prepare_foreground_launch(solar_os_context_t *ctx,
                                                      bool clear_on_resume);
esp_err_t solar_os_shell_session_start_log_follow(solar_os_context_t *ctx,
                                                  solar_os_log_level_t level);
esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx,
                                      const char *arg,
                                      char *path,
                                      size_t path_len);
solar_os_shell_io_t *solar_os_shell_context_io(solar_os_context_t *ctx);
bool solar_os_shell_resolve_path_for_command(solar_os_context_t *ctx,
                                             solar_os_shell_io_t *term,
                                             const char *command,
                                             const char *arg,
                                             char *path,
                                             size_t path_len);
bool solar_os_shell_run_script(solar_os_context_t *ctx,
                               const char *path,
                               const char *display_path,
                               bool report_open_error);
esp_err_t solar_os_shell_set_cwd(solar_os_context_t *ctx, const char *path);
