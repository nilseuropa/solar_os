#include "solar_os_controls_job.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_controls.h"
#include "solar_os_controls_runtime.h"
#include "solar_os_jobs.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

#define CONTROLS_JOB_TICK_MS 20U
#define CONTROLS_WORKER_STACK 4096U
#define CONTROLS_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)
#define CONTROLS_NOTIFY_SAMPLE (1U << 0)
#define CONTROLS_NOTIFY_STOP (1U << 1)

typedef struct {
    bool running;
    uint32_t ticks;
    uint32_t samples;
    uint32_t binding_updates;
    uint32_t sample_errors;
    uint32_t binding_errors;
    uint32_t sample_ms;
    TaskHandle_t worker_task;
    volatile bool worker_done;
} controls_job_state_t;

static controls_job_state_t controls_job;

static void controls_job_worker(void *arg);

static esp_err_t controls_job_start(solar_os_context_t *ctx,
                                    int argc,
                                    char **argv)
{
    (void)argv;
    if (argc != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    controls_job = (controls_job_state_t) {
        .running = true,
    };
    if (solar_os_task_create_pinned_internal(
            controls_job_worker,
            "controls_worker",
            CONTROLS_WORKER_STACK,
            NULL,
            CONTROLS_WORKER_PRIORITY,
            &controls_job.worker_task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        controls_job.running = false;
        return ESP_ERR_NO_MEM;
    }
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(io,
                                 "controls job started: %u controls, %u bindings\n",
                                 (unsigned)solar_os_control_count(),
                                 (unsigned)solar_os_control_binding_count());
    }
    (void)solar_os_jobs_note_resource("controls",
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "controls",
                                      "control mapper");
    return ESP_OK;
}

static void controls_job_stop(solar_os_context_t *ctx)
{
    if (!controls_job.running && controls_job.worker_task == NULL) {
        return;
    }
    controls_job.running = false;
    if (controls_job.worker_task != NULL) {
        (void)xTaskNotify(controls_job.worker_task,
                          CONTROLS_NOTIFY_STOP,
                          eSetBits);
    }
    if (!solar_os_task_wait_done(controls_job.worker_task,
                                 &controls_job.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return;
    }

    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(
            io,
            "controls job stopped: %u samples, %u bindings, %u errors\n",
            (unsigned)controls_job.samples,
            (unsigned)controls_job.binding_updates,
            (unsigned)(controls_job.sample_errors +
                       controls_job.binding_errors));
    }
    controls_job.worker_task = NULL;
    controls_job.worker_done = false;
}

static void controls_job_worker(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification & CONTROLS_NOTIFY_STOP) != 0U) {
            break;
        }
        if ((notification & CONTROLS_NOTIFY_SAMPLE) != 0U &&
            controls_job.running) {
            solar_os_control_pass_t pass;
            const esp_err_t err = solar_os_controls_sample(
                controls_job.sample_ms, CONTROLS_JOB_TICK_MS, &pass);
            controls_job.samples += pass.succeeded;
            controls_job.sample_errors += pass.errors + (err != ESP_OK);
        }
    }

    controls_job.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static bool controls_job_event(solar_os_context_t *ctx,
                               const solar_os_event_t *event)
{
    (void)ctx;
    if (!controls_job.running || event == NULL ||
        event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }
    controls_job.ticks++;
    controls_job.sample_ms = event->data.tick_ms;
    if (controls_job.worker_task == NULL ||
        xTaskNotify(controls_job.worker_task,
                    CONTROLS_NOTIFY_SAMPLE,
                    eSetBits) != pdPASS) {
        controls_job.sample_errors++;
    }
    solar_os_control_pass_t pass;
    const esp_err_t err = solar_os_controls_apply_bindings(&pass);
    controls_job.binding_updates += pass.succeeded;
    controls_job.binding_errors += pass.errors + (err != ESP_OK);
    return false;
}

const solar_os_job_t solar_os_controls_job = {
    .name = "controls",
    .summary = "map scalar streams to native parameters and MIDI controls",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = controls_job_start,
    .stop = controls_job_stop,
    .event = controls_job_event,
    .worker_stack_bytes = CONTROLS_WORKER_STACK,
    .tick_interval_ms = CONTROLS_JOB_TICK_MS,
    .tick_deadline_ms = 2U,
};
