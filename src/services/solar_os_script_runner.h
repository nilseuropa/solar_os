#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "solar_os.h"

#include "solar_os_native_script_runner_abi.h"

typedef enum {
    SOLAR_OS_SCRIPT_INPUT_SOURCE,
    SOLAR_OS_SCRIPT_INPUT_FILE,
} solar_os_script_input_t;

typedef bool (*solar_os_script_cancel_fn)(void *user);

typedef struct {
    solar_os_context_t *context;
    solar_os_script_input_t input_type;
    const char *input;
    size_t input_len;
    const char *source_name;
    int argc;
    const char *const *argv;
    uint32_t timeout_ms;
    solar_os_script_cancel_fn cancel_requested;
    void *cancel_user;
    char *output;
    size_t output_size;
} solar_os_script_run_request_t;

typedef struct {
    esp_err_t status;
    bool success;
    bool cancelled;
    bool timed_out;
    bool output_truncated;
    size_t output_len;
    char error[SOLAR_OS_SCRIPT_ERROR_MAX];
} solar_os_script_run_result_t;

typedef struct {
    const solar_os_script_run_request_t *request;
    solar_os_script_run_result_t *result;
    TickType_t start_tick;
    TickType_t timeout_ticks;
} solar_os_script_run_control_t;

/*
 * Language adapters execute synchronously. Call them from an admitted worker
 * with at least the corresponding language app's declared stack budget.
 */
esp_err_t solar_os_script_run_begin(const solar_os_script_run_request_t *request,
                                    solar_os_script_run_result_t *result,
                                    solar_os_script_run_control_t *control);
/* Lower-level bookkeeping shared with package-provided interpreters. */
esp_err_t solar_os_script_control_begin(const solar_os_script_run_request_t *request,
    solar_os_script_run_result_t *result, solar_os_script_run_control_t *control);
void solar_os_script_run_output(solar_os_script_run_control_t *control,
                                const char *data,
                                size_t len);
void solar_os_script_run_error(solar_os_script_run_control_t *control,
                               esp_err_t status,
                               const char *message);
bool solar_os_script_run_should_cancel(solar_os_script_run_control_t *control);
