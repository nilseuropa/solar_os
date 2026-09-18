#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sim7670.h"
#include "solar_os_expansion.h"

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool gnss_powered;
} solar_os_sim7670_info_t;

esp_err_t solar_os_sim7670_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count);
esp_err_t solar_os_sim7670_detach(const char *name);

size_t solar_os_sim7670_count(void);
bool solar_os_sim7670_get(size_t index, solar_os_sim7670_info_t *info);
esp_err_t solar_os_sim7670_read_status(const char *name,
                                       sim7670_status_t *status);
esp_err_t solar_os_sim7670_command(const char *name,
                                   const char *command,
                                   uint32_t timeout_ms,
                                   char *response,
                                   size_t response_size);

extern const solar_os_expansion_driver_t solar_os_sim7670_expansion_driver;
