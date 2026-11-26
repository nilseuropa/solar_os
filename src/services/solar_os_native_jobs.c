#include "solar_os_native_jobs.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_jobs_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_jobs.h"

static int jobs_snapshot(bool found, solar_os_job_inspection_v1_t *entry)
{
    if (!entry) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!found) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    entry->status.last_error = solar_os_native_result(entry->status.last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int jobs_info(size_t index, solar_os_job_inspection_v1_t *entry)
{
    return jobs_snapshot(solar_os_jobs_inspect(index, entry), entry);
}

static int jobs_find(const char *name, solar_os_job_inspection_v1_t *entry)
{
    if (!name || !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return jobs_snapshot(solar_os_jobs_inspect_by_name(name, entry), entry);
}

static int jobs_find_owner(const char *owner, solar_os_job_inspection_v1_t *entry)
{
    if (!owner || !*owner) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return jobs_snapshot(solar_os_jobs_inspect_by_owner(owner, entry), entry);
}

static int jobs_start(const char *name, int argc, char **argv)
{
    if (!name || !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_jobs_start(solar_os_application_context(), name, argc, argv));
}

static int jobs_stop(const char *name)
{
    if (!name || !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_jobs_stop(solar_os_application_context(), name));
}

static int jobs_print_detail(const char *name)
{
    solar_os_context_t *context = solar_os_application_context();
    if (!context) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return solar_os_native_result(solar_os_jobs_print_detail(context, name));
}

static const char *jobs_kind_name(uint32_t kind)
{
    return solar_os_job_kind_name((solar_os_job_kind_t)kind);
}

static const solar_os_native_jobs_api_v1_t jobs_api = {
    .abi_version = SOLAR_OS_NATIVE_JOBS_ABI,
    .struct_size = sizeof(jobs_api),
    .count = solar_os_jobs_count,
    .info = jobs_info,
    .find = jobs_find,
    .find_owner = jobs_find_owner,
    .start = jobs_start,
    .stop = jobs_stop,
    .error_detail = solar_os_jobs_get_error_detail,
    .print_detail = jobs_print_detail,
    .state_name = solar_os_job_state_name,
    .kind_name = jobs_kind_name,
    .resource_type_name = solar_os_job_resource_type_name,
    .start_disposition_name = solar_os_job_start_disposition_name,
    .start_reason_name = solar_os_job_start_reason_name,
};
#endif

const void *solar_os_native_jobs_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_jobs_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
