#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_ssh_exec.h"
#include "solar_os_native_ssh_exec_abi.h"
#include "solar_os_ssh_transport.h"

struct ssh_session { unsigned closed; };
struct ssh_channel { unsigned started; };
static LIBSSH2_SESSION session;
static LIBSSH2_CHANNEL channel;
static const solar_os_app_t application;
static int64_t now;
static unsigned opens, waits, writes, eof_sends, exits, closes, allocations;
static unsigned stderr_chunks, stdout_chunks, diagnostics, refuses_free;
static bool cancel, cancel_on_wait, wait_for_output, reject_auth;
static solar_os_native_handle_t handle;
static const solar_os_native_ssh_exec_api_v1_t *api;

const solar_os_app_t *solar_os_application_descriptor(void) { return &application; }
int64_t esp_timer_get_time(void) { return now; }

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "native.ssh.exec"));
    ++allocations;
    return calloc(count, size);
}

void solar_os_memory_free(void *data)
{
    assert(allocations && data);
    --allocations;
    free(data);
}

esp_err_t solar_os_ssh_transport_open(const solar_os_ssh_transport_config_t *config, solar_os_ssh_transport_t *t)
{
    assert(!config->allow_unverified_host_key_without_storage);
    assert(config->port == 22 && !strcmp(config->host, "peer") && !strcmp(config->username, "alice"));
    if (reject_auth) {
        config->error(config->user, "public key authentication failed");
        return ESP_FAIL;
    }
    *t = (solar_os_ssh_transport_t){ .session = &session, .socket_fd = 5, .libssh2_ready = true };
    ++opens;
    return ESP_OK;
}

void solar_os_ssh_transport_close(solar_os_ssh_transport_t *t, const char *message)
{
    assert(!message);
    if (t->session) {
        assert(t->session == &session && t->socket_fd == 5);
        assert(!channel.started);
        ++closes;
    }
}

int solar_os_ssh_transport_wait_socket(const solar_os_ssh_transport_config_t *config, int fd, LIBSSH2_SESSION *s)
{
    assert(s == &session && fd == 5);
    ++waits;
    now += 1000;
    if (cancel_on_wait) cancel = true;
    return config->should_stop(config->user) ? -1 : 0;
}

LIBSSH2_CHANNEL *libssh2_channel_open_session(LIBSSH2_SESSION *s)
{
    assert(s == &session);
    return waits ? &channel : NULL;
}

int libssh2_session_last_errno(LIBSSH2_SESSION *s)
{
    assert(s == &session);
    return LIBSSH2_ERROR_EAGAIN;
}

int libssh2_channel_exec(LIBSSH2_CHANNEL *c, const char *command)
{
    assert(c == &channel && !strcmp(command, "command"));
    return c->started++ ? 0 : LIBSSH2_ERROR_EAGAIN;
}

ssize_t libssh2_channel_read(LIBSSH2_CHANNEL *c, char *bytes, size_t capacity)
{
    assert(c == &channel && capacity >= 3);
    if (wait_for_output) return LIBSSH2_ERROR_EAGAIN;
    if (!stdout_chunks++) {
        memcpy(bytes, "a\0b", 3);
        return 3;
    }
    return 0;
}

ssize_t libssh2_channel_read_stderr(LIBSSH2_CHANNEL *c, char *bytes, size_t capacity)
{
    assert(c == &channel && capacity >= 3);
    if (!wait_for_output && stderr_chunks < 2) {
        memcpy(bytes, "c\0d", 3);
        return 3;
    }
    return LIBSSH2_ERROR_EAGAIN;
}

ssize_t libssh2_channel_write(LIBSSH2_CHANNEL *c, const char *bytes, size_t size)
{
    assert(c == &channel);
    if (writes++ == 0) {
        assert(size == 4 && !memcmp(bytes, "z\0xy", 4));
        return 2;
    }
    assert(size == 2 && !memcmp(bytes, "xy", 2));
    return writes == 2 ? LIBSSH2_ERROR_EAGAIN : 2;
}

int libssh2_channel_eof(LIBSSH2_CHANNEL *c) { assert(c == &channel); return !wait_for_output; }
int libssh2_channel_send_eof(LIBSSH2_CHANNEL *c)
{ assert(c == &channel); return eof_sends++ ? 0 : LIBSSH2_ERROR_EAGAIN; }
int libssh2_channel_wait_closed(LIBSSH2_CHANNEL *c)
{ assert(c == &channel); return exits++ ? 0 : LIBSSH2_ERROR_EAGAIN; }
int libssh2_channel_get_exit_status(LIBSSH2_CHANNEL *c) { assert(c == &channel && exits == 2); return 12; }

int libssh2_channel_get_exit_signal(LIBSSH2_CHANNEL *c, char **signal, size_t *size,
    char **message, size_t *message_size, char **lang, size_t *lang_size)
{
    assert(c == &channel && !signal && !message && !message_size && !lang && !lang_size);
    *size = 4;
    return 0;
}

int libssh2_channel_free(LIBSSH2_CHANNEL *c)
{
    assert(c == &channel);
    if (refuses_free) {
        --refuses_free;
        return LIBSSH2_ERROR_EAGAIN;
    }
    c->started = 0;
    return 0;
}

libssh2_cb_generic *libssh2_session_callback_set2(LIBSSH2_SESSION *s, int type, libssh2_cb_generic *callback)
{
    assert(s == &session);
    ++s->closed;
    if (type == LIBSSH2_CALLBACK_SEND) {
        typedef LIBSSH2_SEND_FUNC(send_fn);
        assert(((send_fn *)callback)(5, "x", 1, 0, NULL) == -EPIPE);
    } else {
        typedef LIBSSH2_RECV_FUNC(recv_fn);
        assert(type == LIBSSH2_CALLBACK_RECV && ((recv_fn *)callback)(5, NULL, 0, 0, NULL) == 0);
    }
    return NULL;
}

static bool cancelled(void *user) { assert(user == &now); return cancel; }

static void diagnostic(void *user, const char *message)
{
    assert(user == &now && !strcmp(message, "public key authentication failed"));
    ++diagnostics;
}

static void stderr_data(void *user, const void *bytes, size_t size)
{
    assert(user == &now && size == 3 && !memcmp(bytes, "c\0d", size));
    ++stderr_chunks;
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
}

int main(void)
{
    api = solar_os_native_ssh_exec_get_service("ssh.exec", 1, sizeof(*api));
    assert(api && !solar_os_native_ssh_exec_get_service("ssh.exec", 2, sizeof(*api)));
    const solar_os_native_ssh_exec_options_v1_t options = { .host = "peer", .username = "alice", .command = "command" };
    solar_os_native_ssh_exec_io_v1_t io = { .timeout_ms = 10, .cancelled = cancelled,
        .diagnostic = diagnostic, .stderr_data = stderr_data, .user = &now };
    solar_os_native_ssh_exec_status_v1_t status;
    char output[8];
    size_t size;

    assert(!api->create(&handle) && !api->start(handle, &options, &io));
    assert(opens == 1 && waits == 2);
    assert(api->start(handle, &options, &io) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->write(handle, "z\0xy", 4, &size, &io) && size == 4 && writes == 3);
    assert(!api->finish_input(handle, &io) && !api->finish_input(handle, &io) && eof_sends == 2);
    assert(!api->status(handle, &status) && status.stdin_finished && !status.completed);
    assert(api->write(handle, "z", 1, &size, &io) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && size == 0);
    assert(!api->read(handle, output, sizeof(output), &size, &io) && size == 3 && !memcmp(output, "a\0b", 3));
    assert(!api->read(handle, output, sizeof(output), &size, &io) && size == 0 && stderr_chunks == 2);
    assert(!api->status(handle, &status) && status.completed && status.exit_status == 12 && status.exited_by_signal);
    assert(!api->read(handle, output, sizeof(output), &size, &io) && size == 0 && exits == 2);
    assert(!api->close(handle));
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);

    assert(!api->create(&handle) && !api->start(handle, &options, &io));
    wait_for_output = true;
    io.timeout_ms = 2;
    assert(api->read(handle, output, sizeof(output), &size, &io) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && size == 0);
    io.timeout_ms = 10;
    writes = 0;
    cancel_on_wait = true;
    assert(api->write(handle, "z\0xy", 4, &size, &io) == SOLAR_OS_NATIVE_ERROR_CANCELLED && size == 2);
    assert(api->write(handle, "xy", 2, &size, &io) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && size == 0);
    refuses_free = 2;
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY && allocations == 1);
    assert(!solar_os_native_handle_close_owner(&application) && session.closed == 2);
    assert(closes == 2 && !allocations);

    cancel = false;
    reject_auth = true;
    assert(!api->create(&handle) && api->start(handle, &options, &io) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(diagnostics == 1 && !api->close(handle) && !allocations);
    assert(solar_os_native_handle_count() == 0);
    puts("SSH exec: authenticated transport, binary I/O, EOF, exit status, cancellation, retry and owner cleanup passed");
    return 0;
}
