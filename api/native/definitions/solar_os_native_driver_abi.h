#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_DRIVER_LIFECYCLE_ABI 1U

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *id;
    const char *summary;
    int (*attach)(const char *device_name);
    int (*detach)(const char *device_name);
} solar_os_native_driver_descriptor_v1_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *target;
    const char *firmware_version;
    int (*register_driver)(const solar_os_native_driver_descriptor_v1_t *descriptor);
} solar_os_native_driver_host_api_v1_t;

const solar_os_native_driver_host_api_v1_t *solar_os_native_driver_host_v1(void);

#ifdef __cplusplus
}
#endif
