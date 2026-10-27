#include "solar_os_irc_sync_job.h"
#include "solar_os_irc.h"

static esp_err_t irc_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argv;
    if (argc > 1) return ESP_ERR_INVALID_ARG;
    return solar_os_irc_start();
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
