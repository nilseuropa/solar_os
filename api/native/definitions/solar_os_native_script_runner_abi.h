#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_SCRIPT_RUNNER_SERVICE "script.runner"
#define SOLAR_OS_NATIVE_SCRIPT_RUNNER_ABI 1U
#define SOLAR_OS_SCRIPT_ERROR_MAX 160U
#define SOLAR_OS_SCRIPT_SOURCE_MAX_BYTES (512U * 1024U)

typedef struct solar_os_native_script_control solar_os_native_script_control_v1_t;
typedef bool (*solar_os_native_script_predicate_fn)(void *user);
typedef int (*solar_os_native_script_body_fn)(solar_os_native_script_control_v1_t *control, void *user);

typedef struct {
    uint32_t timeout_ms; /* Zero leaves the deadline unset. */
    solar_os_native_script_predicate_fn cancelled;
    void *cancel_user;
    char *output;
    size_t output_capacity;
} solar_os_native_script_options_v1_t;

typedef struct {
    int32_t status;
    bool success, cancelled, timed_out, output_truncated;
    size_t output_len;
    char error[SOLAR_OS_SCRIPT_ERROR_MAX];
} solar_os_native_script_result_v1_t;

/* Runtime packages provide the interpreter, its inputs and bindings. Run the
 * interpreter synchronously in an admitted calling task. The body borrows
 * its control only until return and handles language unwinding locally.
 * Poll inside interruptible VM work. Returned error and timing metadata
 * include the last reported message. Captured output is terminated when a
 * buffer is supplied. Output length counts bytes copied, including any
 * embedded zero bytes in the interpreter's output. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*run)(const solar_os_native_script_options_v1_t *options, solar_os_native_script_body_fn body,
        void *user, solar_os_native_script_result_v1_t *result);
    void (*output)(solar_os_native_script_control_v1_t *control, const char *text, size_t bytes);
    void (*report_error)(solar_os_native_script_control_v1_t *control, int32_t status, const char *message);
    bool (*should_cancel)(solar_os_native_script_control_v1_t *control);
    bool (*wait_stopped)(solar_os_native_script_predicate_fn stopped, void *user, uint32_t timeout_ms, uint32_t poll_ms);
} solar_os_native_script_runner_api_v1_t;
