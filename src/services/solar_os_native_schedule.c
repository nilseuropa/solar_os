#include "solar_os_native_schedule.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_schedule_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_SCHEDULE
#include "solar_os_schedule.h"

static int schedule_add(const solar_os_schedule_request_v1_t *request)
{
    return solar_os_native_result(solar_os_schedule_add(request));
}

static int schedule_info(size_t index, solar_os_schedule_entry_v1_t *entry)
{
    if (!entry) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *entry = (solar_os_schedule_entry_v1_t){0};
    return solar_os_schedule_get(index, entry) ?
        SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int schedule_find(const char *name, solar_os_schedule_entry_v1_t *entry)
{
    if (entry) *entry = (solar_os_schedule_entry_v1_t){0};
    return solar_os_native_result(solar_os_schedule_get_by_name(name, entry));
}

static int schedule_remove(const char *name)
{
    return solar_os_native_result(solar_os_schedule_remove(name));
}

static int schedule_set_enabled(const char *name, bool enabled)
{
    return solar_os_native_result(solar_os_schedule_set_enabled(name, enabled));
}

static int schedule_run(const char *name)
{
    return solar_os_native_result(solar_os_schedule_run(name));
}

static int schedule_remaining(const char *name, uint32_t *seconds)
{
    if (seconds) *seconds = 0;
    return solar_os_native_result(solar_os_schedule_remaining_seconds(name, seconds));
}

static const solar_os_native_schedule_api_v1_t schedule_api = {
    .abi_version = SOLAR_OS_NATIVE_SCHEDULE_ABI,
    .struct_size = sizeof(schedule_api),
    .add = schedule_add,
    .count = solar_os_schedule_count,
    .info = schedule_info,
    .find = schedule_find,
    .remove = schedule_remove,
    .set_enabled = schedule_set_enabled,
    .run = schedule_run,
    .remaining_seconds = schedule_remaining,
    .alarm_active = solar_os_schedule_alarm_active,
    .stop_alarm = solar_os_schedule_stop_alarm,
    .kind_name = solar_os_schedule_kind_name,
    .action_name = solar_os_schedule_action_name,
};
#endif

const void *solar_os_native_schedule_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_schedule_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
