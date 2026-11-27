#include "solar_os_native_runtime_stats.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_memory_status_abi.h"
#include "solar_os_native_task_admission_abi.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_memory.h"
#include "solar_os_task.h"

static const solar_os_native_memory_status_api_v1_t memory_status_api = {
    .abi_version = SOLAR_OS_NATIVE_MEMORY_STATUS_ABI,
    .struct_size = sizeof(memory_status_api),
    .status = solar_os_memory_get_status,
    .pressure = solar_os_memory_get_pressure_stats,
    .owners = solar_os_memory_get_owners,
    .class_name = solar_os_memory_class_name,
    .reclaim = solar_os_memory_reclaim,
    .compact = solar_os_memory_compact,
};

static const solar_os_native_task_admission_api_v1_t task_admission_api = {
    .abi_version = SOLAR_OS_NATIVE_TASK_ADMISSION_ABI,
    .struct_size = sizeof(task_admission_api),
    .status = solar_os_task_get_status,
    .limits = solar_os_task_get_admission_status,
    .can_create = solar_os_task_can_create,
    .role_name = solar_os_task_role_name,
};
#endif

const void *solar_os_native_runtime_stats_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_runtime_stats_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
