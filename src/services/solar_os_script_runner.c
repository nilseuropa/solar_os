#include "solar_os_script_runner.h"
#include "solar_os_script_lifecycle.h"

#include <string.h>

#include "freertos/task.h"

esp_err_t solar_os_script_run_begin(const solar_os_script_run_request_t *request,
                                    solar_os_script_run_result_t *result,
                                    solar_os_script_run_control_t *control)
{
    if (!request || !request->context || !request->input || request->argc < 0 ||
        request->argc > SOLAR_OS_APP_ARG_MAX || (request->argc > 0 && !request->argv)) {
        if (result) *result = (solar_os_script_run_result_t){.status = ESP_ERR_INVALID_ARG};
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_script_control_begin(request, result, control);
}

esp_err_t solar_os_script_control_begin(const solar_os_script_run_request_t *request,
    solar_os_script_run_result_t *result, solar_os_script_run_control_t *control)
{
    if (result) *result = (solar_os_script_run_result_t){.status = ESP_ERR_INVALID_ARG};
    if (!request || !result || !control || (request->output_size && !request->output)) {
        return ESP_ERR_INVALID_ARG;
    }

    result->status = ESP_OK;
    if (request->output != NULL && request->output_size > 0) {
        request->output[0] = '\0';
    }

    memset(control, 0, sizeof(*control));
    control->request = request;
    control->result = result;
    control->start_tick = xTaskGetTickCount();
    if (request->timeout_ms > 0) {
        control->timeout_ticks = solar_os_script_ticks(request->timeout_ms);
        if (control->timeout_ticks == 0) {
            control->timeout_ticks = 1;
        }
    }
    return ESP_OK;
}

void solar_os_script_run_output(solar_os_script_run_control_t *control,
                                const char *data,
                                size_t len)
{
    if (control == NULL || control->request == NULL || control->result == NULL ||
        data == NULL || len == 0) {
        return;
    }

    const solar_os_script_run_request_t *request = control->request;
    solar_os_script_run_result_t *result = control->result;
    const size_t capacity = request->output_size;
    if (request->output == NULL || capacity == 0) {
        result->output_truncated = true;
        return;
    }

    const size_t available = result->output_len < capacity
        ? capacity - result->output_len - 1U
        : 0;
    const size_t copy_len = len < available ? len : available;
    if (copy_len > 0) {
        memcpy(&request->output[result->output_len], data, copy_len);
        result->output_len += copy_len;
        request->output[result->output_len] = '\0';
    }
    if (copy_len != len) {
        result->output_truncated = true;
    }
}

void solar_os_script_run_error(solar_os_script_run_control_t *control,
                               esp_err_t status,
                               const char *message)
{
    if (control == NULL || control->result == NULL) {
        return;
    }

    solar_os_script_run_result_t *result = control->result;
    result->status = status != ESP_OK ? status : ESP_FAIL;
    result->success = false;
    if (message != NULL) {
        strlcpy(result->error, message, sizeof(result->error));
    }
}

bool solar_os_script_run_should_cancel(solar_os_script_run_control_t *control)
{
    if (control == NULL || control->request == NULL || control->result == NULL) {
        return false;
    }

    solar_os_script_run_result_t *result = control->result;
    if (result->cancelled || result->timed_out) {
        return true;
    }

    const solar_os_script_run_request_t *request = control->request;
    if (request->cancel_requested != NULL &&
        request->cancel_requested(request->cancel_user)) {
        result->cancelled = true;
        return true;
    }

    if (control->timeout_ticks > 0 &&
        (xTaskGetTickCount() - control->start_tick) >= control->timeout_ticks) {
        result->timed_out = true;
        return true;
    }
    return false;
}
