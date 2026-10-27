#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_shell_io.h"

#define SOLAR_OS_PORT_SHELL_SESSION_ID_BASE 16
#define SOLAR_OS_PORT_APP_SESSION_ID_BASE 32

typedef struct {
    solar_os_shell_terminal_profile_t terminal_profile;
    solar_os_shell_charset_t charset;
    uint16_t cols;
    uint16_t rows;
} solar_os_port_shell_options_t;

esp_err_t solar_os_port_shell_init(bool reserve_worker);
esp_err_t solar_os_port_shell_start(solar_os_context_t *ctx,
                                    const char *port_name,
                                    bool run_startup,
                                    uint8_t *session_id);
esp_err_t solar_os_port_shell_start_with_options(solar_os_context_t *ctx,
                                                 const char *port_name,
                                                 const solar_os_port_shell_options_t *options,
                                                 bool run_startup,
                                                 uint8_t *session_id);
esp_err_t solar_os_port_shell_stop(uint8_t session_id);
/* Nonblocking stop request for every port-owned shell and app. */
esp_err_t solar_os_port_shell_prepare_shutdown(void);
esp_err_t solar_os_port_shell_set_dimensions(uint8_t session_id,
                                             uint16_t cols,
                                             uint16_t rows);
bool solar_os_port_shell_is_session_id(uint8_t session_id);
bool solar_os_port_shell_context_owns_session(const solar_os_context_t *ctx,
                                              uint8_t session_id);
bool solar_os_port_shell_is_app_session_id(uint8_t session_id);
bool solar_os_port_shell_context_owns_app_session(
    const solar_os_context_t *ctx,
    uint8_t session_id);
esp_err_t solar_os_port_shell_foreground_app_session(
    solar_os_context_t *caller,
    uint8_t session_id);
esp_err_t solar_os_port_shell_foreground_last_app(solar_os_context_t *caller,
                                                  uint8_t *session_id);
esp_err_t solar_os_port_shell_close_app_session(uint8_t session_id);
size_t solar_os_port_shell_session_count(void);
bool solar_os_port_shell_get_session_id(size_t index, uint8_t *session_id);
size_t solar_os_port_shell_app_session_count(void);
bool solar_os_port_shell_get_app_session_id(size_t index, uint8_t *session_id);
void solar_os_port_shell_print_list(solar_os_shell_io_t *io);
