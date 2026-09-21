#include "solar_os_gesture_listener_job.h"

#include <stdint.h>

#include "solar_os_input_actions.h"
#include "solar_os_jobs.h"
#include "solar_os_shell_io.h"

static void gesture_listener_totals(uint32_t *triggered, uint32_t *dropped)
{
    *triggered = 0U;
    *dropped = 0U;
    const size_t count = solar_os_input_actions_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_input_action_binding_t binding;
        if (!solar_os_input_actions_get(i, &binding)) {
            continue;
        }
        *triggered += binding.trigger_count;
        *dropped += binding.dropped_count;
    }
}

static esp_err_t gesture_listener_job_start(solar_os_context_t *ctx,
                                            int argc,
                                            char **argv)
{
    (void)argv;
    if (argc != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = solar_os_input_actions_start();
    if (err != ESP_OK) {
        return err;
    }
    (void)solar_os_jobs_note_resource("gesture-listener",
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "gesture-observer",
                                      "gesture bindings");
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(io,
                                 "gesture-listener started: %u bindings\n",
                                 (unsigned)solar_os_input_actions_count());
    }
    return ESP_OK;
}

static void gesture_listener_job_stop(solar_os_context_t *ctx)
{
    solar_os_input_actions_stop();
    uint32_t triggered = 0U;
    uint32_t dropped = 0U;
    gesture_listener_totals(&triggered, &dropped);
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(io,
                                 "gesture-listener stopped: %lu triggered, %lu dropped\n",
                                 (unsigned long)triggered,
                                 (unsigned long)dropped);
    }
}

static void gesture_listener_job_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    uint32_t triggered = 0U;
    uint32_t dropped = 0U;
    gesture_listener_totals(&triggered, &dropped);
    solar_os_shell_io_printf(io,
                             "  bindings: configured=%u triggered=%lu dropped=%lu\n",
                             (unsigned)solar_os_input_actions_count(),
                             (unsigned long)triggered,
                             (unsigned long)dropped);
}

const solar_os_job_t solar_os_gesture_listener_job = {
    .name = "gesture-listener",
    .summary = "run gesture-to-command bindings",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = gesture_listener_job_start,
    .stop = gesture_listener_job_stop,
    .worker_stack_bytes = SOLAR_OS_INPUT_ACTION_WORKER_STACK,
    .detail = gesture_listener_job_detail,
};
