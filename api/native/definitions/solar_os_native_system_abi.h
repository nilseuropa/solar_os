#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_SYSTEM_SERVICE "system"
#define SOLAR_OS_NATIVE_SYSTEM_ABI 1U
#define SOLAR_OS_NATIVE_SYSTEM_AVAILABLE 1U
#define SOLAR_OS_NATIVE_SYSTEM_SCOPED 2U

typedef struct {
    char name[48];
    uint32_t abi_version;
    uint32_t struct_size; /* Accessible table bytes; zero if unavailable. */
    uint32_t flags;
} solar_os_native_system_service_v1_t;

/* Enumerates known public contracts, including services omitted by this build.
 * AVAILABLE means get_service can return the table in the calling task's scope.
 * It does not imply that hardware, a model or a device lease is available.
 * SCOPED tables expire with the calling execution and must not be cached by
 * resident libraries. Enumeration is a snapshot; acquire and check the actual
 * service before using it. No handles or caller pointers are retained. */
typedef struct {
    uint32_t abi_version, struct_size;
    uint32_t (*service_count)(void);
    int (*service_info)(uint32_t index, solar_os_native_system_service_v1_t *info);
} solar_os_native_system_api_v1_t;
