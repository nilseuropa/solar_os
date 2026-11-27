#include "solar_os_gateway_sync_job.h"

#include <string.h>

#include "solar_os_gateway_sync.h"
#include "solar_os_log.h"

static esp_err_t gateway_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc > 1 || (argc == 1 && argv && argv[0] && strcmp(argv[0], SOLAR_OS_GATEWAY_SYNC_JOB_NAME))) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_gateway_sync_start();
}

static void gateway_sync_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (solar_os_gateway_sync_stop() == ESP_ERR_TIMEOUT) {
        SOLAR_OS_LOGW("gateway_sync", "Gateway manager is still stopping child transports");
    }
}

const solar_os_job_t solar_os_gateway_sync_job = {
    .name = SOLAR_OS_GATEWAY_SYNC_JOB_NAME,
    .summary = "synchronize configured Gateway instances",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = gateway_sync_start,
    .stop = gateway_sync_stop,
    .worker_stack_bytes = SOLAR_OS_GATEWAY_SYNC_WORKER_STACK,
};
