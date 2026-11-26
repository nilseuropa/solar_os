#include "solar_os_irc_sync_job.h"
#include "solar_os_irc.h"
#include "solar_os_jobs.h"

static esp_err_t irc_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argv;
    if (argc > 1) return ESP_ERR_INVALID_ARG;
    const esp_err_t error = solar_os_irc_start();
    if (error == ESP_OK) {
        (void)solar_os_jobs_note_resource("irc-sync", SOLAR_OS_JOB_RESOURCE_NET,
                                          "IRC TLS", "configured messaging instances");
    }
    return error;
}
static void irc_sync_stop(solar_os_context_t *ctx)
{ (void)ctx; solar_os_irc_stop(); }
const solar_os_job_t solar_os_irc_sync_job = {
    .name = "irc-sync",
    .summary = "maintain native IRC messaging instances",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = irc_sync_start,
    .stop = irc_sync_stop,
    .worker_stack_bytes = 8192,
};
