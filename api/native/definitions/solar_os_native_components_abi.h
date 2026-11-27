#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_COMPONENTS_SERVICE "components"
#define SOLAR_OS_NATIVE_COMPONENTS_ABI 1U
#define SOLAR_OS_NATIVE_COMPONENT_APP 1U
#define SOLAR_OS_NATIVE_COMPONENT_JOB 2U
#define SOLAR_OS_NATIVE_COMPONENT_DRIVER 3U
#define SOLAR_OS_NATIVE_COMPONENT_LIBRARY 4U

typedef struct {
    char id[32];
    char name[64];
    char version[32];
    uint32_t type;
    uint32_t lifecycle_abi;
} solar_os_native_component_info_v1_t;

/* Compiled native components share their module identity/version with the
 * external artifact. Metadata is immutable for the current boot. This table
 * does not enumerate OS services, hardware instances or installed files. */
typedef struct {
    uint32_t abi_version, struct_size;
    uint32_t (*count)(void);
    int (*info)(uint32_t index, solar_os_native_component_info_v1_t *info);
    /* type=0 accepts any component type. */
    int (*find)(uint32_t type, const char *id, solar_os_native_component_info_v1_t *info);
} solar_os_native_components_api_v1_t;
