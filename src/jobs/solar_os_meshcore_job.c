#include "solar_os_meshcore_job.h"
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_meshcore_instances.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

static struct {
    volatile bool stop_requested, task_done;
    TaskHandle_t task;
} worker;

static void instance_worker(void *arg)
{
    (void)arg;
    while (!worker.stop_requested) {
        solar_os_meshcore_instances_poll(1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    solar_os_meshcore_instances_stop(1);
    worker.task_done = true;
    solar_os_task_delete_internal(NULL);
}
static esp_err_t start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (worker.task && !worker.task_done) return ESP_ERR_INVALID_STATE;
    int first = argc && argv && !strcmp(argv[0], "meshcore") ? 1 : 0;
    esp_err_t error = argc == first ? solar_os_meshcore_instances_init() :
        solar_os_meshcore_instances_bind(1, argc - first, (const char *const *)(argv + first));
    if (error != ESP_OK) return error;
    memset(&worker, 0, sizeof(worker));
    if (solar_os_task_create_pinned_internal(instance_worker, "meshcore", SOLAR_OS_MESHCORE_WORKER_STACK, NULL,
        tskIDLE_PRIORITY + 2, &worker.task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
static void stop(solar_os_context_t *ctx)
{
    (void)ctx; worker.stop_requested = true;
    solar_os_meshcore_instances_cancel(1);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(6500);
    while (worker.task && !worker.task_done && (int32_t)(deadline - xTaskGetTickCount()) > 0) vTaskDelay(1);
    if (worker.task_done) worker.task = NULL;
}
static void detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx); if (!io) return;
    solar_os_messaging_provider_id_t cursor = 0; solar_os_messaging_provider_status_t status;
    while (solar_os_meshcore_instances_next(1, &cursor, &status))
        solar_os_shell_io_printf(io, "  %s (%" PRIu32 "): %s %s\n", status.name, cursor,
            status.connected ? "connected" : status.running ? "running" : "stopped", status.detail);
}
void solar_os_meshcore_job_get_status(solar_os_meshcore_status_t *status)
{ if (status) (void)solar_os_meshcore_get_status(status); }

const solar_os_job_t solar_os_meshcore_job = {
    .name = "meshcore", .summary = "Maintain configured MeshCore radio instances",
    .start = start, .stop = stop, .detail = detail, .worker_stack_bytes = SOLAR_OS_MESHCORE_WORKER_STACK,
};
