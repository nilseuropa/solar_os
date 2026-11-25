#include "solar_os_native_components.h"

#include <stdio.h>
#include <string.h>
#include "solar_os_components.h"
#include "solar_os_config.h"
#include "solar_os_native_components_abi.h"

_Static_assert(SOLAR_OS_MODULE_TYPE_APP == SOLAR_OS_NATIVE_COMPONENT_APP, "app type");
_Static_assert(SOLAR_OS_MODULE_TYPE_JOB == SOLAR_OS_NATIVE_COMPONENT_JOB, "job type");
_Static_assert(SOLAR_OS_MODULE_TYPE_DRIVER == SOLAR_OS_NATIVE_COMPONENT_DRIVER, "driver type");
_Static_assert(SOLAR_OS_MODULE_TYPE_LIBRARY == SOLAR_OS_NATIVE_COMPONENT_LIBRARY, "library type");

static uint32_t component_count(void)
{
    return solar_os_component_count();
}

static int component_copy(const solar_os_component_t *component, solar_os_native_component_info_v1_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (!component) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    snprintf(info->id, sizeof(info->id), "%s", component->id);
    snprintf(info->name, sizeof(info->name), "%s", component->name);
    snprintf(info->version, sizeof(info->version), "%s", component->version);
    info->type = component->type;
    info->lifecycle_abi = component->lifecycle_abi;
    return SOLAR_OS_NATIVE_OK;
}

static int component_info(uint32_t index, solar_os_native_component_info_v1_t *info)
{
    return component_copy(solar_os_component_get(index), info);
}

static int component_find(uint32_t type, const char *id, solar_os_native_component_info_v1_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
    if (!id || !*id || strnlen(id, 32) >= 32 || type > SOLAR_OS_NATIVE_COMPONENT_LIBRARY)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return component_copy(solar_os_component_find((solar_os_module_type_t)type, id), info);
}

static const solar_os_native_components_api_v1_t components_api = {
    .abi_version = SOLAR_OS_NATIVE_COMPONENTS_ABI, .struct_size = sizeof(components_api),
    .count = component_count, .info = component_info, .find = component_find,
};

const void *solar_os_native_components_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_components_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
