#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solar_os_expansion.h"

bool solar_os_shell_expansion_parse_binding_token(
    const char *arg,
    solar_os_expansion_binding_t *bindings,
    size_t *binding_count);

esp_err_t solar_os_shell_expansion_attach_command(
    const solar_os_expansion_driver_t *driver,
    const solar_os_expansion_device_t *device,
    char *command,
    size_t command_len);
