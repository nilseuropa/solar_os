#include "solar_os_native_ssh_exec.h"

#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_ssh_exec_abi.h"

#if SOLAR_OS_PACKAGE_SERVICE_SSH
#include <errno.h>
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_ssh.h"
#include "solar_os_ssh_transport.h"

#define SSH_EXEC_TIMEOUT_MS 30000U
#define SSH_EXEC_COMMAND_MAX 4096U
#define SSH_EXEC_KIND SOLAR_OS_NATIVE_HANDLE_SSH_EXEC

typedef struct {
    solar_os_ssh_transport_t transport;
    LIBSSH2_CHANNEL *channel;
    bool attempted, running, finishing_input;
    solar_os_native_ssh_exec_status_v1_t status;
} ssh_exec_t;

typedef struct {
    const solar_os_native_ssh_exec_io_v1_t *io;
    int64_t deadline;
    int result;
} ssh_exec_call_t;

static void reset_deadline(ssh_exec_call_t *call)
{
    const uint32_t timeout = call->io && call->io->timeout_ms ? call->io->timeout_ms : SSH_EXEC_TIMEOUT_MS;
    call->deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
}

static bool stopped(void *user)
{
    ssh_exec_call_t *call = user;
    if (call->result) return true;
    if (call->io && call->io->cancelled && call->io->cancelled(call->io->user)) {
        call->result = SOLAR_OS_NATIVE_ERROR_CANCELLED;
    } else if (esp_timer_get_time() >= call->deadline) {
        call->result = SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    }
    return call->result != SOLAR_OS_NATIVE_OK;
}

static void diagnostic(void *user, const char *message)
{
    const ssh_exec_call_t *call = user;
    if (call->io && call->io->diagnostic) call->io->diagnostic(call->io->user, message);
}

static bool wait_socket(ssh_exec_t *state, ssh_exec_call_t *call)
{
    const solar_os_ssh_transport_config_t config = { .user = call, .should_stop = stopped };
    if (solar_os_ssh_transport_wait_socket(&config, state->transport.socket_fd, state->transport.session) < 0) {
        if (!call->result) call->result = SOLAR_OS_NATIVE_ERROR_IO;
        return false;
    }
    return !stopped(call);
}

static int channel_error(ssh_exec_call_t *call, const char *message)
{
    if (call->result) return call->result;
    diagnostic(call, message);
    return SOLAR_OS_NATIVE_ERROR_IO;
}

static LIBSSH2_RECV_FUNC(closed_recv)
{
    (void)socket; (void)buffer; (void)length; (void)flags; (void)abstract;
    return 0;
}

static LIBSSH2_SEND_FUNC(closed_send)
{
    (void)socket; (void)buffer; (void)length; (void)flags; (void)abstract;
    return -EPIPE;
}

static int exec_destroy(void *value)
{
    ssh_exec_t *state = value;
    state->running = false;
    if (state->channel && libssh2_channel_free(state->channel) == LIBSSH2_ERROR_EAGAIN) {
        /* A wait for remote close or pending write must not strand the channel.
         * Stop all socket I/O before retrying the library's local cleanup. */
        LIBSSH2_SESSION *session = state->transport.session;
        libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_RECV, (libssh2_cb_generic *)closed_recv);
        libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_SEND, (libssh2_cb_generic *)closed_send);
        if (libssh2_channel_free(state->channel) == LIBSSH2_ERROR_EAGAIN) return SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    solar_os_ssh_transport_close(&state->transport, NULL);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int exec_create(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    ssh_exec_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.ssh.exec");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->transport.socket_fd = -1;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(SSH_EXEC_KIND, state, exec_destroy, owner, out) :
        solar_os_native_handle_create(SSH_EXEC_KIND, state, exec_destroy, out);
    if (result) solar_os_memory_free(state);
    return result;
}

static bool text_fits(const char *text, size_t limit)
{
    return text && *text && strnlen(text, limit) < limit;
}

static int start_channel(ssh_exec_t *state, const solar_os_native_ssh_exec_options_v1_t *options,
    ssh_exec_call_t *call)
{
    const solar_os_ssh_transport_config_t config = {
        .host = options->host, .port = options->port ? options->port : 22,
        .username = options->username, .password = options->password,
        .user = call, .should_stop = stopped, .error = diagnostic, .include_error_code = true,
    };
    reset_deadline(call);
    if (stopped(call)) return call->result;
    const esp_err_t error = solar_os_ssh_transport_open(&config, &state->transport);
    if (error) return stopped(call) ? call->result : solar_os_native_result(error);
    reset_deadline(call);
    while (!stopped(call)) {
        state->channel = libssh2_channel_open_session(state->transport.session);
        if (state->channel) break;
        if (libssh2_session_last_errno(state->transport.session) != LIBSSH2_ERROR_EAGAIN || !wait_socket(state, call)) break;
    }
    if (!state->channel) return channel_error(call, "cannot open SSH command channel");
    while (!stopped(call)) {
        const int result = libssh2_channel_exec(state->channel, options->command);
        if (!result) {
            state->running = true;
            return SOLAR_OS_NATIVE_OK;
        }
        if (result != LIBSSH2_ERROR_EAGAIN || !wait_socket(state, call)) break;
    }
    return channel_error(call, "remote SSH command rejected");
}

static int exec_start(solar_os_native_handle_t id, const solar_os_native_ssh_exec_options_v1_t *options,
    const solar_os_native_ssh_exec_io_v1_t *io)
{
    if (!options || options->port > UINT16_MAX || !text_fits(options->host, SOLAR_OS_SSH_HOST_MAX) ||
        !text_fits(options->username, SOLAR_OS_SSH_USERNAME_MAX) || !text_fits(options->command, SSH_EXEC_COMMAND_MAX) ||
        (options->password && strnlen(options->password, SOLAR_OS_SSH_PASSWORD_MAX) >= SOLAR_OS_SSH_PASSWORD_MAX)) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    ssh_exec_t *state = solar_os_native_handle_acquire(id, SSH_EXEC_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!state->attempted) {
        state->attempted = true;
        ssh_exec_call_t call = { .io = io };
        result = start_channel(state, options, &call);
    }
    solar_os_native_handle_release(id);
    return result;
}

static bool channel_read_pending(ssize_t size)
{
    return size >= 0 || size == LIBSSH2_ERROR_EAGAIN;
}

static int read_channel(ssh_exec_t *state, void *bytes, size_t capacity, size_t *size, ssh_exec_call_t *call)
{
    if (state->status.completed) return SOLAR_OS_NATIVE_OK;
    reset_deadline(call);
    while (!stopped(call)) {
        unsigned char stderr_bytes[256];
        const ssize_t err_size = libssh2_channel_read_stderr(state->channel, (char *)stderr_bytes, sizeof(stderr_bytes));
        if (!channel_read_pending(err_size)) break;
        if (err_size > 0 && call->io && call->io->stderr_data) {
            call->io->stderr_data(call->io->user, stderr_bytes, (size_t)err_size);
        }
        const ssize_t received = libssh2_channel_read(state->channel, bytes, capacity);
        if (received > 0) {
            *size = (size_t)received;
            return SOLAR_OS_NATIVE_OK;
        }
        if (!channel_read_pending(received)) break;
        if (err_size > 0) continue;
        if (libssh2_channel_eof(state->channel)) {
            const int result = libssh2_channel_wait_closed(state->channel);
            if (result == 0) {
                size_t signal_size = 0;
                libssh2_channel_get_exit_signal(state->channel, NULL, &signal_size, NULL, NULL, NULL, NULL);
                state->status.exited_by_signal = signal_size != 0;
                state->status.exit_status = libssh2_channel_get_exit_status(state->channel);
                state->status.completed = true;
                return SOLAR_OS_NATIVE_OK;
            }
            if (result != LIBSSH2_ERROR_EAGAIN) break;
        }
        if (!wait_socket(state, call)) break;
    }
    return channel_error(call, "SSH command read failed");
}

static int exec_read(solar_os_native_handle_t id, void *bytes, size_t capacity, size_t *size,
    const solar_os_native_ssh_exec_io_v1_t *io)
{
    if (!size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size = 0;
    if (!bytes || !capacity || capacity > PTRDIFF_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    ssh_exec_t *state = solar_os_native_handle_acquire(id, SSH_EXEC_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    ssh_exec_call_t call = { .io = io };
    const int result = state->running ? read_channel(state, bytes, capacity, size, &call) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release(id);
    return result;
}

static int write_channel(ssh_exec_t *state, const char *bytes, size_t size, size_t *written, ssh_exec_call_t *call)
{
    if (!state->running || state->finishing_input || state->status.completed) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    reset_deadline(call);
    while (*written < size && !stopped(call)) {
        const ssize_t sent = libssh2_channel_write(state->channel, bytes + *written, size - *written);
        if (sent > 0) {
            *written += (size_t)sent;
            reset_deadline(call);
        } else if (!channel_read_pending(sent) || !wait_socket(state, call)) {
            break;
        }
    }
    if (*written == size) return SOLAR_OS_NATIVE_OK;
    /* libssh2 can require the exact same buffer after EAGAIN. Close preserves
     * the host's ownership and never retries a caller's borrowed write. */
    state->running = false;
    return channel_error(call, "SSH command write failed");
}

static int exec_write(solar_os_native_handle_t id, const void *bytes, size_t size, size_t *written,
    const solar_os_native_ssh_exec_io_v1_t *io)
{
    if (!written) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *written = 0;
    if ((!bytes && size) || size > PTRDIFF_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    ssh_exec_t *state = solar_os_native_handle_acquire(id, SSH_EXEC_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    ssh_exec_call_t call = { .io = io };
    const int result = write_channel(state, bytes, size, written, &call);
    solar_os_native_handle_release(id);
    return result;
}

static int finish_input(ssh_exec_t *state, ssh_exec_call_t *call)
{
    if (!state->running) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (state->status.stdin_finished || state->status.completed) return SOLAR_OS_NATIVE_OK;
    state->finishing_input = true;
    reset_deadline(call);
    while (!stopped(call)) {
        const int result = libssh2_channel_send_eof(state->channel);
        if (!result) {
            state->status.stdin_finished = true;
            return SOLAR_OS_NATIVE_OK;
        }
        if (result != LIBSSH2_ERROR_EAGAIN || !wait_socket(state, call)) break;
    }
    return channel_error(call, "cannot finish SSH command input");
}

static int exec_finish_input(solar_os_native_handle_t id, const solar_os_native_ssh_exec_io_v1_t *io)
{
    ssh_exec_t *state = solar_os_native_handle_acquire(id, SSH_EXEC_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    ssh_exec_call_t call = { .io = io };
    const int result = finish_input(state, &call);
    solar_os_native_handle_release(id);
    return result;
}

static int exec_status(solar_os_native_handle_t id, solar_os_native_ssh_exec_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_ssh_exec_status_v1_t){0};
    ssh_exec_t *state = solar_os_native_handle_acquire(id, SSH_EXEC_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = state->status;
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int exec_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SSH_EXEC_KIND);
}

static const solar_os_native_ssh_exec_api_v1_t ssh_exec_api = {
    .abi_version = SOLAR_OS_NATIVE_SSH_EXEC_ABI, .struct_size = sizeof(ssh_exec_api),
    .create = exec_create, .start = exec_start, .read = exec_read, .write = exec_write,
    .finish_input = exec_finish_input, .status = exec_status, .close = exec_close,
};
#endif

const void *solar_os_native_ssh_exec_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ssh_exec_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
