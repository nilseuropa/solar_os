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

esp_err_t solar_os_shell_expansion_binding_manifest_field(
    const solar_os_expansion_driver_t *driver,
    const solar_os_expansion_binding_t *binding,
    char *key,
    size_t key_len,
    char *value,
    size_t value_len,
    bool *string_value);

esp_err_t solar_os_shell_expansion_export_manifest(
    const char *path,
    size_t *bus_count,
    size_t *device_count,
    char *unsupported_device,
    size_t unsupported_device_len);
