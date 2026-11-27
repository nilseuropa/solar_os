#pragma once

#include "solar_os_expansion_types_v1.h"
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_DRIVER_REGISTRATION_SERVICE "driver.registration"
#define SOLAR_OS_NATIVE_DRIVER_REGISTRATION_ABI 1U

/* Registration borrows immutable binding specs and their strings and values
 * until the driver unloads. Core normalizes and claims bindings before attach.
 * The attach callback borrows the name and bindings for the duration of the
 * call. Store driver state in the instance pointer for detach. The driver
 * owns that state. It may retain a bus lease using the device name as owner and register
 * providers with library_lease zero. Return a normalized native service
 * result. A failed attach must release its handles. A failed detach can be
 * retried and must leave the remaining resources safe to close. */
typedef struct {
    uint32_t abi_version, struct_size;
    const char *id;
    const char *summary;
    solar_os_expansion_category_t category;
    const solar_os_expansion_binding_spec_t *binding_specs;
    size_t binding_spec_count;
    int (*attach)(const char *name, const solar_os_expansion_binding_t *bindings, size_t binding_count, void **instance);
    int (*detach)(void *instance);
} solar_os_native_bound_driver_descriptor_v1_t;


typedef struct {
    uint32_t abi_version, struct_size;
    /* Call from the entry point of a driver component. */
    int (*register_driver)(const solar_os_native_bound_driver_descriptor_v1_t *descriptor);
} solar_os_native_driver_registration_api_v1_t;
