#include "solar_os_native_remote.h"
#include "solar_os_native_ssh_abi.h"
#include "solar_os_native_remote_files_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include <stdatomic.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_SSH || SOLAR_OS_PACKAGE_SERVICE_SFTP
#include "solar_os_ssh.h"
static bool text_fits(const char *value, size_t capacity, bool required)
{ return value ? (!required || *value) && strlen(value) < capacity : !required; }
static int ssh_options(const solar_os_native_ssh_options_v1_t *in, solar_os_ssh_config_t *out)
{
    if (!in || !text_fits(in->host, SOLAR_OS_SSH_HOST_MAX, true) ||
        !text_fits(in->username, SOLAR_OS_SSH_USERNAME_MAX, true) ||
        !text_fits(in->password, SOLAR_OS_SSH_PASSWORD_MAX, false) ||
        in->port > UINT16_MAX || in->columns > UINT16_MAX || in->rows > UINT16_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_ssh_config_t){in->host, in->port, in->username, in->password, in->columns, in->rows};
    return SOLAR_OS_NATIVE_OK;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SSH
#include "solar_os_scp.h"
#include "solar_os_ssh_keys.h"
#if SOLAR_OS_PACKAGE_SERVICE_SFTPSYNC
#include "solar_os_sftpsync.h"
#endif
typedef struct { void *session; uint32_t kind, timeout; } native_remote_worker_t;
static int worker_destroy(void *value)
{
    native_remote_worker_t *state = value; esp_err_t error = ESP_OK;
    if (state->session) {
        switch (state->kind) {
        case SOLAR_OS_NATIVE_HANDLE_SSH: error = solar_os_ssh_stop_wait(state->session, state->timeout); break;
        case SOLAR_OS_NATIVE_HANDLE_SCP: error = solar_os_scp_stop_wait(state->session, state->timeout); break;
#if SOLAR_OS_PACKAGE_SERVICE_SFTPSYNC
        case SOLAR_OS_NATIVE_HANDLE_SFTPSYNC: error = solar_os_sftpsync_stop_wait(state->session, state->timeout); break;
#endif
        default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        }
    }
    if (error) return solar_os_native_result(error);
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int worker_create(uint32_t kind, solar_os_native_handle_t *out, native_remote_worker_t **state)
{
    *out = 0; *state = solar_os_memory_calloc(1, sizeof(**state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.ssh");
    if (!*state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    (*state)->kind = kind; (*state)->timeout = 2000;
    int result = solar_os_native_handle_create(kind, *state, worker_destroy, out);
    if (result) { worker_destroy(*state); *state = NULL; }
    return result;
}
static int worker_close(solar_os_native_handle_t id, uint32_t kind, uint32_t timeout)
{
    if (timeout > 60000) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_remote_worker_t *state = solar_os_native_handle_acquire(id, kind);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    state->timeout = timeout; solar_os_native_handle_release(id);
    return solar_os_native_handle_close_kind(id, kind);
}
static int ssh_start(const solar_os_native_ssh_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_ssh_config_t config; int result = ssh_options(in, &config);
    if (result) return result;
    native_remote_worker_t *state; result = worker_create(SOLAR_OS_NATIVE_HANDLE_SSH, out, &state);
    if (!result) {
        solar_os_ssh_session_t *session = NULL; result = solar_os_native_result(solar_os_ssh_start(&config, &session));
        state->session = session;
        if (result && !solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_SSH)) *out = 0;
    }
    return result;
}
static int ssh_close(solar_os_native_handle_t id, uint32_t timeout) { return worker_close(id, SOLAR_OS_NATIVE_HANDLE_SSH, timeout); }
static int ssh_send(solar_os_native_handle_t id, const void *bytes, size_t size)
{
    if (!bytes && size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_remote_worker_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SSH);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_ssh_send(state->session, bytes, size));
    solar_os_native_handle_release(id); return result;
}
static int ssh_poll(solar_os_native_handle_t id, solar_os_native_ssh_event_v1_t *out, bool *available)
{
    if (!out || !available) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); *available = false;
    native_remote_worker_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SSH);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_ssh_event_t value = {0}; *available = solar_os_ssh_poll(state->session, &value);
    int result = SOLAR_OS_NATIVE_OK;
    if (*available) {
        switch (value.type) {
        case SOLAR_OS_SSH_EVENT_STATUS: out->type = SOLAR_OS_NATIVE_SSH_STATUS; break;
        case SOLAR_OS_SSH_EVENT_CONNECTED: out->type = SOLAR_OS_NATIVE_SSH_CONNECTED; break;
        case SOLAR_OS_SSH_EVENT_OUTPUT: out->type = SOLAR_OS_NATIVE_SSH_OUTPUT; break;
        case SOLAR_OS_SSH_EVENT_ERROR: out->type = SOLAR_OS_NATIVE_SSH_ERROR; break;
        case SOLAR_OS_SSH_EVENT_DISCONNECTED: out->type = SOLAR_OS_NATIVE_SSH_DISCONNECTED; break;
        default: result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE; break;
        }
        if (value.len > sizeof(out->data)) result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        else { out->data_size = value.len; memcpy(out->data, value.data, value.len); memcpy(out->message, value.message, sizeof(out->message)); }
    }
    solar_os_native_handle_release(id); return result;
}
static int ssh_hosts(solar_os_native_ssh_host_visit_fn visitor, void *user)
{ return visitor ? solar_os_native_result(solar_os_ssh_hosts_visit(visitor, user)) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; }
static int key_paths(char *private_path, size_t private_capacity, char *public_path, size_t public_capacity)
{ return solar_os_native_result(solar_os_ssh_keys_default_paths(private_path, private_capacity, public_path, public_capacity)); }
static int key_status(solar_os_native_ssh_keys_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_ssh_key_status_t value = {0};
    int result = solar_os_native_result(solar_os_ssh_keys_get_status(&value));
    if (!result) {
        out->private_exists = value.private_key_exists; out->public_exists = value.public_key_exists;
        out->private_size = value.private_key_size; out->public_size = value.public_key_size;
        _Static_assert(sizeof(out->private_path) == sizeof(value.private_key_path), "key path capacity");
        _Static_assert(sizeof(out->public_path) == sizeof(value.public_key_path), "key path capacity");
        memcpy(out->private_path, value.private_key_path, sizeof(out->private_path));
        memcpy(out->public_path, value.public_key_path, sizeof(out->public_path));
    }
    return result;
}
static int key_read_public(char *text, size_t capacity, size_t *size) { return solar_os_native_result(solar_os_ssh_keys_read_public(text, capacity, size)); }
static int key_generate(uint32_t bits, bool overwrite) { return solar_os_native_result(solar_os_ssh_keys_generate_rsa(bits, overwrite)); }
static int key_remove(void) { return solar_os_native_result(solar_os_ssh_keys_remove_default()); }
static int transfer_options(const solar_os_native_transfer_options_v1_t *in, solar_os_ssh_config_t *ssh, bool sync)
{
    if (!in || !text_fits(in->local_path, SOLAR_OS_STORAGE_PATH_MAX, true) ||
        !text_fits(in->remote_path, SOLAR_OS_STORAGE_PATH_MAX, true) || in->direction > SOLAR_OS_NATIVE_TRANSFER_DOWNLOAD ||
        (in->flags & ~(sync ? SOLAR_OS_NATIVE_TRANSFER_RECURSIVE | SOLAR_OS_NATIVE_TRANSFER_DRY_RUN : SOLAR_OS_NATIVE_TRANSFER_GLOB)))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return ssh_options(&in->ssh, ssh);
}
static int scp_start(const solar_os_native_transfer_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_ssh_config_t ssh; int result = transfer_options(in, &ssh, false);
    if (result) return result;
    native_remote_worker_t *state; result = worker_create(SOLAR_OS_NATIVE_HANDLE_SCP, out, &state);
    if (!result) {
        solar_os_scp_config_t options = {.direction = in->direction == SOLAR_OS_NATIVE_TRANSFER_UPLOAD ? SOLAR_OS_SCP_UPLOAD : SOLAR_OS_SCP_DOWNLOAD,
            .host = ssh.host, .port = ssh.port, .username = ssh.username, .password = ssh.password,
            .local_path = in->local_path, .remote_path = in->remote_path, .remote_glob = (in->flags & SOLAR_OS_NATIVE_TRANSFER_GLOB) != 0};
        solar_os_scp_session_t *session = NULL; result = solar_os_native_result(solar_os_scp_start(&options, &session)); state->session = session;
        if (result && !solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_SCP)) *out = 0;
    }
    return result;
}
static int scp_close(solar_os_native_handle_t id, uint32_t timeout) { return worker_close(id, SOLAR_OS_NATIVE_HANDLE_SCP, timeout); }
static int scp_poll(solar_os_native_handle_t id, solar_os_native_transfer_event_v1_t *out, bool *available)
{
    if (!out || !available) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); *available = false;
    native_remote_worker_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SCP);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_scp_event_t value = {0}; *available = solar_os_scp_poll(state->session, &value);
    int result = SOLAR_OS_NATIVE_OK;
    if (*available) {
        switch (value.type) {
        case SOLAR_OS_SCP_EVENT_STATUS: out->type = SOLAR_OS_NATIVE_TRANSFER_STATUS; break;
        case SOLAR_OS_SCP_EVENT_PROGRESS: out->type = SOLAR_OS_NATIVE_TRANSFER_PROGRESS; break;
        case SOLAR_OS_SCP_EVENT_ERROR: out->type = SOLAR_OS_NATIVE_TRANSFER_ERROR; break;
        case SOLAR_OS_SCP_EVENT_DONE: out->type = SOLAR_OS_NATIVE_TRANSFER_DONE; break;
        default: result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE; break;
        }
        out->transferred = value.transferred; out->total = value.total;
        memcpy(out->message, value.message, sizeof(value.message));
    }
    solar_os_native_handle_release(id); return result;
}
#if SOLAR_OS_PACKAGE_SERVICE_SFTPSYNC
static int sync_start(const solar_os_native_transfer_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_ssh_config_t ssh; int result = transfer_options(in, &ssh, true);
    if (result) return result;
    native_remote_worker_t *state; result = worker_create(SOLAR_OS_NATIVE_HANDLE_SFTPSYNC, out, &state);
    if (!result) {
        solar_os_sftpsync_config_t options = {.direction = in->direction == SOLAR_OS_NATIVE_TRANSFER_UPLOAD ? SOLAR_OS_SFTPSYNC_UPLOAD : SOLAR_OS_SFTPSYNC_DOWNLOAD,
            .host = ssh.host, .port = ssh.port, .username = ssh.username, .password = ssh.password,
            .local_path = in->local_path, .remote_path = in->remote_path, .recursive = (in->flags & SOLAR_OS_NATIVE_TRANSFER_RECURSIVE) != 0,
            .dry_run = (in->flags & SOLAR_OS_NATIVE_TRANSFER_DRY_RUN) != 0};
        solar_os_sftpsync_session_t *session = NULL; result = solar_os_native_result(solar_os_sftpsync_start(&options, &session)); state->session = session;
        if (result && !solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_SFTPSYNC)) *out = 0;
    }
    return result;
}
static int sync_close(solar_os_native_handle_t id, uint32_t timeout) { return worker_close(id, SOLAR_OS_NATIVE_HANDLE_SFTPSYNC, timeout); }
static int sync_poll(solar_os_native_handle_t id, solar_os_native_transfer_event_v1_t *out, bool *available)
{
    if (!out || !available) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); *available = false;
    native_remote_worker_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SFTPSYNC);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_sftpsync_event_t value = {0}; *available = solar_os_sftpsync_poll(state->session, &value);
    int result = SOLAR_OS_NATIVE_OK;
    if (*available) {
        switch (value.type) {
        case SOLAR_OS_SFTPSYNC_EVENT_STATUS: out->type = SOLAR_OS_NATIVE_TRANSFER_STATUS; break;
        case SOLAR_OS_SFTPSYNC_EVENT_PROGRESS: out->type = SOLAR_OS_NATIVE_TRANSFER_PROGRESS; break;
        case SOLAR_OS_SFTPSYNC_EVENT_ERROR: out->type = SOLAR_OS_NATIVE_TRANSFER_ERROR; break;
        case SOLAR_OS_SFTPSYNC_EVENT_DONE: out->type = SOLAR_OS_NATIVE_TRANSFER_DONE; break;
        default: result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE; break;
        }
        out->transferred = value.transferred; out->files_changed = value.files_changed; out->file_transferred = value.file_transferred;
        out->file_size = value.file_size; out->file_size_known = value.file_size_known;
        _Static_assert(sizeof(out->message) == sizeof(value.message), "sync message capacity");
        memcpy(out->message, value.message, sizeof(out->message));
    }
    solar_os_native_handle_release(id); return result;
}
#endif
static const solar_os_native_ssh_api_v1_t ssh_api = {
    .abi_version = SOLAR_OS_NATIVE_SSH_ABI, .struct_size = sizeof(ssh_api), .start = ssh_start, .close = ssh_close, .send = ssh_send, .poll = ssh_poll,
    .hosts = ssh_hosts, .key_paths = key_paths, .key_exists = solar_os_ssh_keys_default_exists, .key_status = key_status,
    .key_read_public = key_read_public, .key_generate = key_generate, .key_remove = key_remove,
    .scp_start = scp_start, .scp_close = scp_close, .scp_poll = scp_poll,
#if SOLAR_OS_PACKAGE_SERVICE_SFTPSYNC
    .sync_start = sync_start, .sync_close = sync_close, .sync_poll = sync_poll,
#endif
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_REMOTE_FILES
#include "solar_os.h"
#include "solar_os_remote_files.h"
typedef struct { solar_os_remote_files_t session; atomic_bool cancelled; } native_remote_files_t;
static bool files_cancelled(void *user)
{
    uint32_t id = (uint32_t)(uintptr_t)user;
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return true;
    bool result = atomic_load(&state->cancelled); solar_os_native_handle_release(id); return result;
}
static int files_result(native_remote_files_t *state, esp_err_t error)
{ return error && atomic_load(&state->cancelled) ? SOLAR_OS_NATIVE_ERROR_CANCELLED : solar_os_native_result(error); }
static int files_destroy(void *value)
{
    native_remote_files_t *state = value; atomic_store(&state->cancelled, true);
    solar_os_remote_files_disconnect(&state->session);
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int files_create(uint32_t protocol, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (protocol > SOLAR_OS_NATIVE_REMOTE_FILES_SFTP) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_remote_files_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.remote.files");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->session.protocol = protocol == SOLAR_OS_NATIVE_REMOTE_FILES_FTP ?
        SOLAR_OS_REMOTE_FILE_FTP : SOLAR_OS_REMOTE_FILE_SFTP;
    atomic_init(&state->cancelled, false);
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES;
    int result = owner ? solar_os_native_handle_create_owned(kind, state, files_destroy, owner, out) :
        solar_os_native_handle_create(kind, state, files_destroy, out);
    if (result) files_destroy(state);
    return result;
}

static int sftp_create(solar_os_native_handle_t *out)
{ return files_create(SOLAR_OS_NATIVE_REMOTE_FILES_SFTP, out); }
static int files_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES); }
static int files_cancel(solar_os_native_handle_t id, bool requested)
{
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    atomic_store(&state->cancelled, requested); solar_os_native_handle_release(id); return SOLAR_OS_NATIVE_OK;
}
static int files_connect(solar_os_native_handle_t id, const solar_os_native_remote_file_options_v1_t *in,
    char *diagnostic, size_t capacity)
{
    if ((diagnostic == NULL) != (capacity == 0)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (diagnostic) *diagnostic = '\0';
    if (!in || !in->host || !*in->host || !in->username || in->port > UINT16_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (state->session.connection) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    if (state->session.protocol == SOLAR_OS_REMOTE_FILE_SFTP &&
        (!text_fits(in->host, SOLAR_OS_SSH_HOST_MAX, true) ||
         !text_fits(in->username, SOLAR_OS_SSH_USERNAME_MAX, true) ||
         !text_fits(in->password, SOLAR_OS_SSH_PASSWORD_MAX, false))) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    const solar_os_remote_file_options_t options = {
        .host = in->host, .port = (uint16_t)in->port, .username = in->username,
        .password = !in->password && state->session.protocol == SOLAR_OS_REMOTE_FILE_FTP ? "" : in->password,
        .timeout_ms = in->timeout_ms,
        .error = diagnostic, .error_len = capacity,
    };
    const esp_err_t error = solar_os_remote_files_connect(&state->session, state->session.protocol,
        &options, files_cancelled, (void *)(uintptr_t)id);
    int result = files_result(state, error);
    solar_os_native_handle_release(id);
    return result;
}

static int sftp_connect(solar_os_native_handle_t id, const solar_os_native_ssh_options_v1_t *in, uint32_t timeout)
{
    solar_os_ssh_config_t ssh;
    int result = ssh_options(in, &ssh);
    if (result) return result;
    const solar_os_native_remote_file_options_v1_t options = {
        .host = ssh.host, .port = ssh.port, .username = ssh.username, .password = ssh.password, .timeout_ms = timeout,
    };
    return files_connect(id, &options, NULL, 0);
}
static int files_disconnect(solar_os_native_handle_t id)
{
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    bool cancelled = atomic_exchange(&state->cancelled, true);
    solar_os_remote_files_disconnect(&state->session);
    atomic_store(&state->cancelled, cancelled);
    solar_os_native_handle_release(id); return SOLAR_OS_NATIVE_OK;
}
static int files_last_error(solar_os_native_handle_t id, char *text, size_t capacity)
{
    if (!text || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *text = 0; native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const char *error = solar_os_remote_files_error(&state->session); size_t size = strlen(error) + 1;
    int result = size <= capacity ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (!result) memcpy(text, error, size);
    solar_os_native_handle_release(id); return result;
}
typedef struct { solar_os_native_remote_file_entry_fn visitor; void *user; } native_file_visitor_t;
static bool files_entry(const solar_os_remote_file_entry_t *in, void *user)
{
    native_file_visitor_t *bridge = user;
    solar_os_native_remote_file_entry_v1_t out = {.size = in->size, .is_directory = in->is_directory};
    _Static_assert(sizeof(out.name) == sizeof(in->name), "remote file name capacity");
    memcpy(out.name, in->name, sizeof(out.name));
    return bridge->visitor(&out, bridge->user);
}
static int files_list(solar_os_native_handle_t id, const char *path,
    solar_os_native_remote_file_entry_fn visitor, void *user)
{
    if (!visitor) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    native_file_visitor_t bridge = {visitor, user};
    int result = state->session.connection ?
        files_result(state, solar_os_remote_files_list(&state->session, path, files_entry, &bridge)) :
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release(id); return result;
}
static int files_transfer(solar_os_native_handle_t id, const char *first, const char *second,
    solar_os_native_transfer_progress_fn progress, void *user, bool upload)
{
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (state->session.connection) {
        const esp_err_t error = upload ? solar_os_remote_files_upload(&state->session, first, second, progress, user) :
            solar_os_remote_files_download(&state->session, first, second, progress, user);
        result = files_result(state, error);
    }
    solar_os_native_handle_release(id); return result;
}
static int files_download(solar_os_native_handle_t id, const char *remote, const char *local, solar_os_native_transfer_progress_fn progress, void *user)
{ return files_transfer(id, remote, local, progress, user, false); }
static int files_upload(solar_os_native_handle_t id, const char *local, const char *remote, solar_os_native_transfer_progress_fn progress, void *user)
{ return files_transfer(id, local, remote, progress, user, true); }
#define FILES_PATH_OPERATION(name) \
static int files_##name(solar_os_native_handle_t id, const char *path) { \
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES); \
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; \
    int result = state->session.connection ? files_result(state, solar_os_remote_files_##name(&state->session, path)) : \
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE; \
    solar_os_native_handle_release(id); return result; \
}
FILES_PATH_OPERATION(mkdir)
FILES_PATH_OPERATION(rmdir)
FILES_PATH_OPERATION(remove)
#undef FILES_PATH_OPERATION
static int files_rename(solar_os_native_handle_t id, const char *first, const char *second)
{
    native_remote_files_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = state->session.connection ?
        files_result(state, solar_os_remote_files_rename(&state->session, first, second)) :
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release(id); return result;
}
static const solar_os_native_remote_files_api_v1_t files_api = {
    .abi_version = SOLAR_OS_NATIVE_REMOTE_FILES_ABI, .struct_size = sizeof(files_api), .create = files_create,
    .close = files_close, .cancel = files_cancel, .connect = files_connect, .disconnect = files_disconnect,
    .last_error = files_last_error, .list = files_list, .download = files_download, .upload = files_upload,
    .mkdir = files_mkdir, .rmdir = files_rmdir, .remove = files_remove, .rename = files_rename,
};
static const solar_os_native_sftp_api_v1_t sftp_api = {
    .abi_version = SOLAR_OS_NATIVE_SFTP_ABI, .struct_size = sizeof(sftp_api), .create = sftp_create, .close = files_close,
    .cancel = files_cancel, .connect = sftp_connect, .disconnect = files_disconnect, .last_error = files_last_error,
    .list = files_list, .download = files_download, .upload = files_upload, .mkdir = files_mkdir, .rmdir = files_rmdir, .remove = files_remove, .rename = files_rename,
};
#endif
const void *solar_os_native_remote_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_remote_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
