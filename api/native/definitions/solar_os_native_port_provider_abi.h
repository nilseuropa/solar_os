#pragma once

#include "solar_os_native_ports_abi.h"

#define SOLAR_OS_NATIVE_PORT_PROVIDER_SERVICE "io.ports.provider"
#define SOLAR_OS_NATIVE_PORT_PROVIDER_ABI 1U

typedef struct {
    void *user;
    int (*open)(void *user);
    int (*close)(void *user);
    int (*read)(void *user, uint8_t *bytes, size_t capacity, uint32_t timeout_ms, size_t *count);
    int (*write)(void *user, const uint8_t *bytes, size_t size, size_t *count);
    int (*serial_get)(void *user, solar_os_native_serial_config_v1_t *config);
    int (*serial_set)(void *user, const solar_os_native_serial_config_v1_t *config, uint32_t mask);
    int (*serial_break)(void *user, uint16_t milliseconds);
} solar_os_native_port_provider_callbacks_v1_t;

typedef struct {
    const char *name;
    const char *label;
    uint32_t capabilities;
    solar_os_native_port_provider_callbacks_v1_t callbacks;
} solar_os_native_port_provider_endpoint_v1_t;

/* Publish a group of byte ports after registering all counterpart streams.
 * Registration copies the names, labels and callback table. A library
 * lease pins callback code. Zero selects the currently attaching native
 * driver. Callback user state remains valid until close succeeds. A claim,
 * active operation, or stream observer can make close return BUSY. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*register_many)(solar_os_native_handle_t library,
        const solar_os_native_port_provider_endpoint_v1_t *endpoints, size_t count,
        solar_os_native_handle_t *registration);
    int (*close)(solar_os_native_handle_t registration);
} solar_os_native_port_provider_api_v1_t;
