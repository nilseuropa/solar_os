#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_SSH_EXEC_SERVICE "ssh.exec"
#define SOLAR_OS_NATIVE_SSH_EXEC_ABI 1U

typedef struct {
    const char *host, *username, *password, *command;
    uint32_t port;
} solar_os_native_ssh_exec_options_v1_t;

typedef struct {
    uint32_t timeout_ms;
    solar_os_native_cancel_fn cancelled;
    void (*diagnostic)(void *user, const char *text);
    void (*stderr_data)(void *user, const void *bytes, size_t size);
    void *user;
} solar_os_native_ssh_exec_io_v1_t;

typedef struct {
    uint32_t stdin_finished, completed;
    int32_t exit_status;
    uint32_t exited_by_signal;
} solar_os_native_ssh_exec_status_v1_t;

/* One non-PTY command per handle, with the system's SSH authentication and
 * host key policy. Connection strings and callbacks are borrowed for a call.
 * Serialize operations on a handle and avoid reentering it from a callback.
 * Timeout zero selects the default 30 seconds. Reads deliver stderr to the
 * callback when provided and return stdout as binary bytes. A successful
 * read with zero bytes indicates command completion. Check status for the
 * exit result. When a write fails, written counts confirmed bytes and the
 * channel must be closed. An unsuccessful start also requires close. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(solar_os_native_handle_t *handle);
    int (*start)(solar_os_native_handle_t handle, const solar_os_native_ssh_exec_options_v1_t *options,
        const solar_os_native_ssh_exec_io_v1_t *io);
    int (*read)(solar_os_native_handle_t handle, void *bytes, size_t capacity, size_t *size,
        const solar_os_native_ssh_exec_io_v1_t *io);
    int (*write)(solar_os_native_handle_t handle, const void *bytes, size_t size, size_t *written,
        const solar_os_native_ssh_exec_io_v1_t *io);
    int (*finish_input)(solar_os_native_handle_t handle, const solar_os_native_ssh_exec_io_v1_t *io);
    int (*status)(solar_os_native_handle_t handle, solar_os_native_ssh_exec_status_v1_t *status);
    int (*close)(solar_os_native_handle_t handle);
} solar_os_native_ssh_exec_api_v1_t;
