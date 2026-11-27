#include "solar_os_sc2_sync_job.h"
#include "solar_os_jobs.h"
#include "solar_os_sc2_sync.h"

static esp_err_t sc2_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argv;
    if (argc > 1) return ESP_ERR_INVALID_ARG;
    const esp_err_t error = solar_os_sc2_sync_start();
    if (error == ESP_OK) {
        (void)solar_os_jobs_note_resource("sc2-sync", SOLAR_OS_JOB_RESOURCE_NET, "SC2 relay", "sc2 messaging instance");
    }
    return error;
}
static void sc2_sync_stop(solar_os_context_t *ctx)
{ (void)ctx; solar_os_sc2_sync_stop(); }
const solar_os_job_t solar_os_sc2_sync_job = {
    .name = "sc2-sync",
    .summary = "connect sc2 messaging instances to their relay",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = sc2_sync_start,
    .stop = sc2_sync_stop,
    .worker_stack_bytes = SOLAR_OS_SC2_SYNC_STACK,
};
