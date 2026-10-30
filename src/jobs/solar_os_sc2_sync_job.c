#include "solar_os_sc2_sync_job.h"
#include "solar_os_sc2_sync.h"

static esp_err_t sc2_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argv;
    if (argc > 1) return ESP_ERR_INVALID_ARG;
    return solar_os_sc2_sync_start();
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
