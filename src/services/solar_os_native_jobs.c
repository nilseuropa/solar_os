#include "solar_os_native_jobs.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_jobs_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_jobs.h"
#include "solar_os_native_core.h"

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

static int jobs_owner_name(const char *name, char *owner, size_t capacity)
{
    return solar_os_native_result(solar_os_jobs_owner_name(name, owner, capacity));
}

static int jobs_note_resource(const char *name, uint32_t generation,
    solar_os_job_resource_type_t type, const char *resource, const char *detail)
{
    return solar_os_native_result(solar_os_jobs_note_run_resource(name, generation, type, resource, detail));
}

static int jobs_clear_resources(const char *name, uint32_t generation)
{
    return solar_os_native_result(solar_os_jobs_clear_run_resources(name, generation));
}

static int jobs_claim_port(const char *name, uint32_t generation, const char *port, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    esp_err_t error = solar_os_jobs_check_generation(name, generation);
    if (error != ESP_OK) return solar_os_native_result(error);
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    error = solar_os_jobs_owner_name(name, owner, sizeof(owner));
    if (error != ESP_OK) return solar_os_native_result(error);
    const int claimed = solar_os_native_port_claim(port, owner, out);
    if (claimed != SOLAR_OS_NATIVE_OK) return claimed;

    /* A stop may have won while the device's claim callback ran. Update the
     * same generation or release only the port token just obtained. */
    const int noted = jobs_note_resource(name, generation, SOLAR_OS_JOB_RESOURCE_PORT, port, "rw");
    if (noted != SOLAR_OS_NATIVE_OK && !solar_os_native_port_close(*out)) *out = 0;
    return noted;
}

static int jobs_complete(const char *name, uint32_t generation, int32_t result)
{
    const esp_err_t error = solar_os_native_callback_result(result);
    return solar_os_native_result(solar_os_jobs_mark_stopped(name, generation, error));
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
    .owner_name = jobs_owner_name,
    .note_resource = jobs_note_resource,
    .clear_resources = jobs_clear_resources,
    .claim_port = jobs_claim_port,
    .complete = jobs_complete,
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
