#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_shell_io.h"

#define SOLAR_OS_PORT_SHELL_SESSION_ID_BASE 16

typedef struct {
    solar_os_shell_terminal_profile_t terminal_profile;
    uint16_t cols;
    uint16_t rows;
} solar_os_port_shell_options_t;

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
bool solar_os_port_shell_is_session_id(uint8_t session_id);
size_t solar_os_port_shell_session_count(void);
bool solar_os_port_shell_get_session_id(size_t index, uint8_t *session_id);
void solar_os_port_shell_print_list(solar_os_shell_io_t *io);
