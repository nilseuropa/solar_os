#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_board_caps.h"
#include "solar_os_expansion_types.h"
#include "solar_os_expansion_types_v1.h"
#include "solar_os_stream.h"

bool solar_os_expansion_stream_binding_valid(solar_os_expansion_binding_kind_t,
                                             const solar_os_stream_info_t *);

typedef esp_err_t (*solar_os_expansion_attach_fn_t)(const char *name,
                                                    const solar_os_expansion_binding_t *bindings,
                                                    size_t binding_count);
typedef esp_err_t (*solar_os_expansion_detach_fn_t)(const char *name);

typedef struct {
    const char *name;
    const char *summary;
    solar_os_expansion_category_t category;
    solar_os_board_capabilities_t required_capabilities;
    bool probe_supported;
    bool early;
    const solar_os_expansion_binding_spec_t *binding_specs;
    size_t binding_spec_count;
    bool allow_unlisted_bindings;
    solar_os_expansion_attach_fn_t attach;
    solar_os_expansion_detach_fn_t detach;
    void *callback_user;
    esp_err_t (*attach_with_user)(void *user,
                                  const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count, void **instance);
    esp_err_t (*detach_with_user)(void *user, const char *name, void *instance);
    /* Optional cross-binding checks, shared by CLI, scripts and board defaults. */
    esp_err_t (*validate_bindings)(const solar_os_expansion_binding_t *bindings,
                                   size_t binding_count,
                                   solar_os_expansion_binding_validation_t *validation);
    /* Board manifest adapters: excluded from manual enumeration and attachment. */
    bool board_only;
} solar_os_expansion_driver_t;

typedef struct {
    const char *driver;
    const char *name;
    bool detachable;
    size_t binding_count;
    const solar_os_expansion_binding_t *bindings;
} solar_os_expansion_default_device_t;

esp_err_t solar_os_expansion_init(void);
esp_err_t solar_os_expansion_init_early(void);
void solar_os_expansion_poll(uint32_t now_ms);
bool solar_os_expansion_available(void);

/* Enumerate drivers available for manual attachment, excluding board adapters. */
size_t solar_os_expansion_driver_count(void);
bool solar_os_expansion_get_driver(size_t index, solar_os_expansion_driver_t *driver);
esp_err_t solar_os_expansion_get_driver_info(size_t index, solar_os_expansion_driver_info_t *info);
esp_err_t solar_os_expansion_find_driver_info(const char *name, solar_os_expansion_driver_info_t *info);
esp_err_t solar_os_expansion_get_binding_spec(const char *name, size_t index,
    solar_os_expansion_binding_spec_info_t *out, int *values, size_t capacity);
esp_err_t solar_os_expansion_register_driver(
    const solar_os_expansion_driver_t *driver);
esp_err_t solar_os_expansion_unregister_driver(
    const solar_os_expansion_driver_t *driver);
bool solar_os_expansion_driver_supported(const char *name);
const char *solar_os_expansion_category_name(solar_os_expansion_category_t category);
bool solar_os_expansion_binding_pin_supported(
    solar_os_expansion_binding_kind_t kind,
    const char *target,
    int pin);
esp_err_t solar_os_expansion_validate_bindings(
    const char *driver,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_expansion_binding_validation_t *validation);

size_t solar_os_expansion_i2c_bus_count(void);
bool solar_os_expansion_get_i2c_bus(size_t index, solar_os_expansion_i2c_bus_t *bus);
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index);

size_t solar_os_expansion_spi_bus_count(void);
bool solar_os_expansion_get_spi_bus(size_t index, solar_os_expansion_spi_bus_t *bus);
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index);
bool solar_os_expansion_spi_cs_allowed(const char *bus_name, int pin);

size_t solar_os_expansion_uart_port_count(void);
bool solar_os_expansion_get_uart_port(size_t index, solar_os_expansion_uart_port_t *port);
bool solar_os_expansion_find_uart_port(const char *name, solar_os_expansion_uart_port_t *port, size_t *index);

size_t solar_os_expansion_onewire_bus_count(void);
bool solar_os_expansion_get_onewire_bus(size_t index, solar_os_expansion_onewire_bus_t *bus);
bool solar_os_expansion_find_onewire_bus(const char *name,
                                         solar_os_expansion_onewire_bus_t *bus,
                                         size_t *index);

esp_err_t solar_os_expansion_attach(const char *driver,
                                    const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count);
/* Restore a board device with its manifest bindings and board provenance. */
esp_err_t solar_os_expansion_attach_board_device(const char *name);
esp_err_t solar_os_expansion_detach(const char *name);
esp_err_t solar_os_expansion_device_set_ready(const char *name, bool ready);
size_t solar_os_expansion_device_count(void);
/* Returns a caller-owned snapshot; no registry pointer is borrowed. */
bool solar_os_expansion_get_device(size_t index, solar_os_expansion_device_t *device);

const char *solar_os_expansion_binding_kind_name(solar_os_expansion_binding_kind_t kind);
const char *solar_os_expansion_origin_name(solar_os_expansion_origin_t origin);
