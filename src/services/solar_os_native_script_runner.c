#include "solar_os_native_script_runner.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_RUNNER
#include "solar_os_native_support.h"
#include "solar_os_script_lifecycle.h"
#include "solar_os_script_runner.h"

struct solar_os_native_script_control {
    solar_os_script_run_control_t run;
    int32_t reported;
};

static int script_run(const solar_os_native_script_options_v1_t *options, solar_os_native_script_body_fn body,
    void *user, solar_os_native_script_result_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_script_result_v1_t){.status = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT};
    if (!options || !body) return out->status;
    const solar_os_script_run_request_t request = {.timeout_ms = options->timeout_ms,
        .cancel_requested = options->cancelled, .cancel_user = options->cancel_user,
        .output = options->output, .output_size = options->output_capacity};
    solar_os_script_run_result_t result;
    solar_os_native_script_control_v1_t control = {0};
    const esp_err_t error = solar_os_script_control_begin(&request, &result, &control.run);
    if (error != ESP_OK) {
        out->status = solar_os_native_result(error);
        return out->status;
    }
    int status = SOLAR_OS_NATIVE_OK;
    if (!solar_os_script_run_should_cancel(&control.run)) status = body(&control, user);
    (void)solar_os_script_run_should_cancel(&control.run);
    if (result.timed_out) status = SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    else if (result.cancelled) status = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    else if (!status) status = control.reported;
    out->status = status;
    out->success = status == SOLAR_OS_NATIVE_OK;
    out->cancelled = result.cancelled;
    out->timed_out = result.timed_out;
    out->output_truncated = result.output_truncated;
    out->output_len = result.output_len;
    memcpy(out->error, result.error, sizeof(out->error));
    return status;
}

static void script_output(solar_os_native_script_control_v1_t *control, const char *text, size_t bytes)
{
    if (control) solar_os_script_run_output(&control->run, text, bytes);
}

static void script_report_error(solar_os_native_script_control_v1_t *control, int32_t status, const char *message)
{
    if (!control) return;
    control->reported = status ? status : SOLAR_OS_NATIVE_ERROR_FAILED;
    solar_os_script_run_error(&control->run, ESP_FAIL, message);
}

static bool script_should_cancel(solar_os_native_script_control_v1_t *control)
{
    return control && solar_os_script_run_should_cancel(&control->run);
}

static const solar_os_native_script_runner_api_v1_t script_runner_api = {
    .abi_version = SOLAR_OS_NATIVE_SCRIPT_RUNNER_ABI, .struct_size = sizeof(script_runner_api),
    .run = script_run, .output = script_output, .report_error = script_report_error,
    .should_cancel = script_should_cancel, .wait_stopped = solar_os_script_wait_for_stop,
};
#endif

const void *solar_os_native_script_runner_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_script_runner_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
