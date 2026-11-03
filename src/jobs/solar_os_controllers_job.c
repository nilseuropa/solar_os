#include "solar_os_controllers_job.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_controllers.h"
#include "solar_os_jobs.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

#define CONTROLLERS_STACK 4096U
static TaskHandle_t worker_task;
static volatile bool worker_done;
static esp_err_t cleanup_error;

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t notification = 0;
        TickType_t delay = pdMS_TO_TICKS(10);
        if (delay == 0) delay = 1;
        (void)xTaskNotifyWait(0, UINT32_MAX, &notification, delay);
        if (notification != 0) break;
        solar_os_controllers_tick(solar_os_time_uptime_ms());
    }
    cleanup_error = solar_os_controllers_stop_all();
    worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argv;
    if (argc != 1) return ESP_ERR_INVALID_ARG;
    if (worker_task != NULL) return ESP_ERR_INVALID_STATE;
    worker_done = false; cleanup_error = ESP_OK;
    if (solar_os_task_create_pinned_internal(worker, "controllers", CONTROLLERS_STACK,
        NULL, tskIDLE_PRIORITY + 1, &worker_task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) return ESP_ERR_NO_MEM;
    (void)solar_os_jobs_note_resource("controllers", SOLAR_OS_JOB_RESOURCE_CUSTOM,
        "controllers", "feedback loops");
    return ESP_OK;
}

static void stop(solar_os_context_t *ctx)
{
    if (worker_task == NULL) return;
    if (!worker_done) (void)xTaskNotify(worker_task, 1U, eSetBits);
    if (!solar_os_task_wait_done(worker_task, &worker_done, SOLAR_OS_TASK_STOP_WAIT_MS)) return;
    worker_task = NULL;
    if (cleanup_error != ESP_OK) solar_os_shell_io_printf(solar_os_context_shell_io(ctx),
        "controllers: output cleanup failed: %s; retry controller stop <name>\n",
        esp_err_to_name(cleanup_error));
}

const solar_os_job_t solar_os_controllers_job = {
    .name = "controllers", .summary = "run feedback controllers",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND, .start = start, .stop = stop,
    .worker_stack_bytes = CONTROLLERS_STACK,
};
