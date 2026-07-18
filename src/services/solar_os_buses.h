#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_bus_types.h"

esp_err_t solar_os_buses_init(void);
esp_err_t solar_os_bus_register(const solar_os_bus_definition_t *definition);
esp_err_t solar_os_bus_unregister(const char *name);

size_t solar_os_bus_count(void);
size_t solar_os_bus_count_protocol(solar_os_bus_protocol_t protocol);
bool solar_os_bus_get(size_t index, solar_os_bus_info_t *info);
bool solar_os_bus_get_protocol(solar_os_bus_protocol_t protocol,
                               size_t index,
                               solar_os_bus_info_t *info);
bool solar_os_bus_find(const char *name,
                       solar_os_bus_protocol_t protocol,
                       solar_os_bus_info_t *info);

esp_err_t solar_os_bus_acquire(const char *name,
                               solar_os_bus_protocol_t protocol,
                               const char *owner);
esp_err_t solar_os_bus_release(const char *name,
                               solar_os_bus_protocol_t protocol,
                               const char *owner);
size_t solar_os_bus_release_owner(const char *owner);

const char *solar_os_bus_protocol_name(solar_os_bus_protocol_t protocol);
const char *solar_os_bus_origin_name(solar_os_bus_origin_t origin);
const char *solar_os_bus_sharing_name(solar_os_bus_sharing_t sharing);
