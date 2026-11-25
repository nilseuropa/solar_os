#pragma once

#include <stddef.h>
#include <stdint.h>
#include "solar_os_native_app_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOLAR_OS_MODULE_TYPE_INVALID = 0,
    SOLAR_OS_MODULE_TYPE_APP,
    SOLAR_OS_MODULE_TYPE_JOB,
    SOLAR_OS_MODULE_TYPE_DRIVER,
    SOLAR_OS_MODULE_TYPE_LIBRARY,
} solar_os_module_type_t;

typedef struct {
    const char *id;
    const char *version;
} solar_os_component_dependency_t;

typedef struct {
    solar_os_module_type_t type;
    const char *id;
    const char *name;
    const char *version;
    uint32_t lifecycle_abi;
    int (*entry)(int argc, char **argv);
    size_t dependency_count;
    const solar_os_component_dependency_t *dependencies;
    /* Compiled foreground metadata has the same validated record as an ELF. */
    const solar_os_native_app_manifest_v2_t *manifest;
} solar_os_component_t;

/* Immutable firmware descriptors. They remain valid for the entire boot and
 * allocate no per-component state until their lifecycle is activated. */
size_t solar_os_component_count(void);
const solar_os_component_t *solar_os_component_get(size_t index);
/* INVALID searches all types; module identities are unique across types. */
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type,
                                                    const char *id);

#ifdef __cplusplus
}
#endif
