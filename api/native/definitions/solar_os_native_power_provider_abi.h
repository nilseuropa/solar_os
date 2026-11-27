#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_POWER_PROVIDER_SERVICE "power.provider"
#define SOLAR_OS_NATIVE_POWER_PROVIDER_ABI 1U

typedef struct {
    const char *name;
    uint32_t library_lease;
    int (*off)(void *user);
    void *user;
} solar_os_native_power_off_provider_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Register the board-selected power-off driver. The runtime calls the
     * driver during shutdown and retains its code until close. A zero
     * lease is valid during expansion-driver attachment. */
    int (*register_off)(const solar_os_native_power_off_provider_v1_t *provider,
                         solar_os_native_handle_t *handle);
    int (*close)(solar_os_native_handle_t handle);
} solar_os_native_power_provider_api_v1_t;
