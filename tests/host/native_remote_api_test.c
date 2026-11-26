#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_native_client.h"
#include "solar_os_native_remote.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"
#include "solar_os_ftp.h"
#include "solar_os_ssh.h"
#include "solar_os_scp.h"
#include "solar_os_sftpsync.h"
#include "solar_os_sftp.h"
#include "solar_os_ssh_keys.h"
struct solar_os_ssh_session { char host[96]; };
struct solar_os_scp_session { unsigned marker; };
struct solar_os_sftpsync_session { unsigned marker; };
struct solar_os_sftp_session { solar_os_sftp_cancel_fn_t cancel; void *user; };
struct solar_os_ftp_session { solar_os_ftp_cancel_fn_t cancel; void *user; };
static bool close_timeout, send_event = true;
static unsigned allocations, visits, progress_calls;
static const solar_os_native_sftp_api_v1_t *sftp;
static solar_os_native_handle_t sftp_context;
static solar_os_sftp_cancel_fn_t saved_cancel;
static void *saved_user;
static bool fail_ftp_connect, application_active;
static const solar_os_app_t application = {0};

const solar_os_app_t *solar_os_application_descriptor(void)
{
    return application_active ? &application : NULL;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p = calloc(n, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
esp_err_t solar_os_ssh_start(const solar_os_ssh_config_t *config, solar_os_ssh_session_t **out)
{
    assert(!strcmp(config->host, "peer") && config->port == 65535 && config->cols == 65535 && config->rows == 44);
    *out = calloc(1, sizeof(**out)); assert(*out); strcpy((*out)->host, config->host); return ESP_OK;
}
esp_err_t solar_os_ssh_stop_wait(solar_os_ssh_session_t *state, uint32_t timeout)
{ assert(state && timeout == 42); if (close_timeout) return ESP_ERR_TIMEOUT; free(state); return ESP_OK; }
esp_err_t solar_os_ssh_send(solar_os_ssh_session_t *state, const char *bytes, size_t size)
{ assert(!strcmp(state->host, "peer") && size == 3 && !memcmp(bytes, "abc", 3)); return ESP_OK; }
bool solar_os_ssh_poll(solar_os_ssh_session_t *state, solar_os_ssh_event_t *out)
{
    assert(!strcmp(state->host, "peer")); if (!send_event) return false;
    *out = (solar_os_ssh_event_t){.type = SOLAR_OS_SSH_EVENT_OUTPUT, .len = 3}; memcpy(out->data, "a\0b", 3); strcpy(out->message, "ready"); return true;
}
esp_err_t solar_os_ssh_hosts_visit(solar_os_ssh_host_visitor_t visitor, void *user) { visitor("::1", "peer", user); return ESP_OK; }
esp_err_t solar_os_ssh_keys_default_paths(char *private_path, size_t private_capacity, char *public_path, size_t public_capacity)
{ snprintf(private_path, private_capacity, "/key"); snprintf(public_path, public_capacity, "/key.pub"); return ESP_OK; }
bool solar_os_ssh_keys_default_exists(void) { return true; }
esp_err_t solar_os_ssh_keys_get_status(solar_os_ssh_key_status_t *out)
{ *out = (solar_os_ssh_key_status_t){.private_key_exists = true, .public_key_exists = true, .private_key_size = 1234}; strcpy(out->private_key_path, "/key"); return ESP_OK; }
esp_err_t solar_os_ssh_keys_read_public(char *out, size_t capacity, size_t *size)
{ assert(capacity >= 8); strcpy(out, "ssh-rsa"); *size = 7; return ESP_OK; }
esp_err_t solar_os_ssh_keys_generate_rsa(uint32_t bits, bool overwrite) { assert(bits == 3072 && overwrite); return ESP_OK; }
esp_err_t solar_os_ssh_keys_remove_default(void) { return ESP_OK; }
esp_err_t solar_os_scp_start(const solar_os_scp_config_t *options, solar_os_scp_session_t **out)
{
    assert(options->direction == SOLAR_OS_SCP_DOWNLOAD && options->remote_glob && !strcmp(options->local_path, "/local"));
    *out = calloc(1, sizeof(**out)); assert(*out); return ESP_OK;
}
esp_err_t solar_os_scp_stop_wait(solar_os_scp_session_t *state, uint32_t timeout) { assert(timeout == 42); free(state); return ESP_OK; }
bool solar_os_scp_poll(solar_os_scp_session_t *state, solar_os_scp_event_t *out)
{ assert(state); *out = (solar_os_scp_event_t){.type = SOLAR_OS_SCP_EVENT_PROGRESS, .transferred = UINT64_C(5000000000), .total = UINT64_C(6000000000)}; return true; }
esp_err_t solar_os_sftpsync_start(const solar_os_sftpsync_config_t *options, solar_os_sftpsync_session_t **out)
{
    assert(options->direction == SOLAR_OS_SFTPSYNC_DOWNLOAD && options->recursive && options->dry_run);
    *out = calloc(1, sizeof(**out)); assert(*out); return ESP_OK;
}
esp_err_t solar_os_sftpsync_stop_wait(solar_os_sftpsync_session_t *state, uint32_t timeout) { assert(timeout == 42); free(state); return ESP_OK; }
bool solar_os_sftpsync_poll(solar_os_sftpsync_session_t *state, solar_os_sftpsync_event_t *out)
{ assert(state); *out = (solar_os_sftpsync_event_t){.type = SOLAR_OS_SFTPSYNC_EVENT_DONE, .files_changed = 7, .file_size_known = true, .file_size = UINT64_C(6000000000)}; strcpy(out->message, "done"); return true; }
esp_err_t solar_os_sftp_connect(const solar_os_sftp_options_t *options, solar_os_sftp_cancel_fn_t cancel, void *user, solar_os_sftp_session_t **out)
{
    assert(!strcmp(options->host, "peer") && options->timeout_ms == 42);
    saved_cancel = cancel; saved_user = user;
    if (cancel(user)) return ESP_ERR_INVALID_STATE;
    *out = calloc(1, sizeof(**out)); assert(*out); (*out)->cancel = cancel; (*out)->user = user; return ESP_OK;
}
void solar_os_sftp_disconnect(solar_os_sftp_session_t *state) { assert(state); free(state); }
const char *solar_os_sftp_last_error(const solar_os_sftp_session_t *state) { return state ? "last error" : "not connected"; }
esp_err_t solar_os_sftp_list(solar_os_sftp_session_t *state, const char *path, solar_os_sftp_entry_fn_t visitor, void *user)
{
    assert(state && !strcmp(path, "/remote"));
    solar_os_sftp_entry_t entry = {.size = UINT64_C(5000000000), .is_directory = true}; strcpy(entry.name, "entry");
    (void)visitor(&entry, user); return ESP_OK;
}
esp_err_t solar_os_sftp_download(solar_os_sftp_session_t *state, const char *remote, const char *local, solar_os_sftp_progress_fn_t progress, void *user)
{
    assert(!strcmp(remote, "/remote") && !strcmp(local, "/local"));
    if (state->cancel(state->user)) return ESP_ERR_INVALID_STATE;
    progress(UINT64_C(6000000000), user); return ESP_OK;
}
esp_err_t solar_os_sftp_upload(solar_os_sftp_session_t *state, const char *local, const char *remote, solar_os_sftp_progress_fn_t progress, void *user)
{ return solar_os_sftp_download(state, remote, local, progress, user); }
#define PATH_OP(name) esp_err_t solar_os_sftp_##name(solar_os_sftp_session_t *state, const char *path) { assert(state && !strcmp(path, "/remote")); return ESP_OK; }
PATH_OP(mkdir) PATH_OP(rmdir) PATH_OP(remove)
#undef PATH_OP
esp_err_t solar_os_sftp_rename(solar_os_sftp_session_t *state, const char *first, const char *second)
{ assert(state && !strcmp(first, "/remote") && !strcmp(second, "/new")); return ESP_OK; }

esp_err_t solar_os_ftp_connect(const solar_os_ftp_options_t *options, solar_os_ftp_cancel_fn_t cancel,
    void *user, solar_os_ftp_session_t **out)
{
    assert(!strcmp(options->host, "ftp-peer") && options->port == 21 && options->timeout_ms == 73);
    if (cancel(user)) return ESP_ERR_INVALID_STATE;
    if (fail_ftp_connect) {
        snprintf(options->error, options->error_len, "connection refused");
        return ESP_FAIL;
    }
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->cancel = cancel;
    (*out)->user = user;
    return ESP_OK;
}
void solar_os_ftp_disconnect(solar_os_ftp_session_t *state) { assert(state); free(state); }
const char *solar_os_ftp_last_reply(const solar_os_ftp_session_t *state) { return state ? "FTP reply" : ""; }
esp_err_t solar_os_ftp_list(solar_os_ftp_session_t *state, const char *path, solar_os_ftp_entry_fn_t visitor, void *user)
{
    assert(state && !strcmp(path, "/remote"));
    if (state->cancel(state->user)) return ESP_ERR_INVALID_STATE;
    const solar_os_ftp_entry_t entry = {.name = "ftp-file", .size = 17};
    return visitor(&entry, user) ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_ftp_download(solar_os_ftp_session_t *state, const char *remote, const char *local,
    solar_os_ftp_progress_fn_t transfer_progress, void *user)
{
    assert(state && !strcmp(remote, "/remote") && !strcmp(local, "/local"));
    if (state->cancel(state->user)) return ESP_ERR_INVALID_STATE;
    if (transfer_progress) transfer_progress(UINT64_C(6000000000), user);
    return ESP_OK;
}
esp_err_t solar_os_ftp_upload(solar_os_ftp_session_t *state, const char *local, const char *remote,
    solar_os_ftp_progress_fn_t transfer_progress, void *user)
{ return solar_os_ftp_download(state, remote, local, transfer_progress, user); }
#define FTP_PATH_OPERATION(name) \
esp_err_t solar_os_ftp_##name(solar_os_ftp_session_t *state, const char *path) \
{ assert(state && !strcmp(path, "/remote")); return ESP_OK; }
FTP_PATH_OPERATION(mkdir)
FTP_PATH_OPERATION(rmdir)
FTP_PATH_OPERATION(remove)
#undef FTP_PATH_OPERATION
esp_err_t solar_os_ftp_rename(solar_os_ftp_session_t *state, const char *first, const char *second)
{ assert(state && !strcmp(first, "/remote") && !strcmp(second, "/new")); return ESP_OK; }

static bool visit(const solar_os_native_sftp_entry_v1_t *entry, void *user)
{
    assert(user == &visits && !strcmp(entry->name, "entry") && entry->size == UINT64_C(5000000000) && entry->is_directory);
    assert(sftp->close(sftp_context) == SOLAR_OS_NATIVE_ERROR_BUSY); ++visits; return true;
}
static void progress(uint64_t bytes, void *user) { assert(bytes == UINT64_C(6000000000) && user == &progress_calls); ++progress_calls; }

static bool visit_ftp(const solar_os_native_remote_file_entry_v1_t *entry, void *user)
{
    assert(!strcmp(entry->name, "ftp-file") && entry->size == 17 && !entry->is_directory);
    bool *visited = user;
    *visited = true;
    return true;
}

static void test_ftp(void)
{
    const solar_os_native_remote_files_api_v1_t *api = solar_os_native_remote_get_service(
        SOLAR_OS_NATIVE_REMOTE_FILES_SERVICE, SOLAR_OS_NATIVE_REMOTE_FILES_ABI, sizeof(*api));
    assert(api && api->list == sftp->list && api->upload == sftp->upload);
    solar_os_native_handle_t context;
    assert(api->create(2, &context) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !context);
    application_active = true;
    assert(!api->create(SOLAR_OS_NATIVE_REMOTE_FILES_FTP, &context));
    application_active = false;
    const solar_os_native_remote_file_options_v1_t options = {"ftp-peer", "anonymous", NULL, 21, 73};
    char diagnostic[32];
    fail_ftp_connect = true;
    assert(api->connect(context, &options, diagnostic, sizeof(diagnostic)) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!strcmp(diagnostic, "connection refused"));
    fail_ftp_connect = false;
    assert(!api->connect(context, &options, diagnostic, sizeof(diagnostic)) && !*diagnostic);
    assert(!api->last_error(context, diagnostic, sizeof(diagnostic)) && !strcmp(diagnostic, "FTP reply"));
    bool visited = false;
    assert(!api->list(context, "/remote", visit_ftp, &visited) && visited);
    assert(!api->mkdir(context, "/remote"));
    assert(!api->rmdir(context, "/remote"));
    assert(!api->remove(context, "/remote"));
    assert(!api->rename(context, "/remote", "/new"));
    assert(!api->cancel(context, true));
    assert(api->download(context, "/remote", "/local", progress, &progress_calls) ==
           SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(!saved_cancel(saved_user)); /* The SFTP connection has its own cancel flag. */
    assert(!api->disconnect(context));
    assert(!api->cancel(context, false));
    assert(!api->connect(context, &options, NULL, 0));
    assert(!solar_os_native_handle_close_owner(&application));
    assert(api->close(context) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

int main(void)
{
    const solar_os_native_ssh_api_v1_t *ssh = solar_os_native_remote_get_service("ssh", 1, sizeof(*ssh)); assert(ssh);
    sftp = solar_os_native_remote_get_service("sftp", 1, sizeof(*sftp)); assert(sftp);
    solar_os_native_ssh_options_v1_t options = {"peer", "user", "secret", 65535, 65535, 44};
    solar_os_native_handle_t session; options.port = 65536;
    assert(ssh->start(&options, &session) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !session); options.port = 65535;
    assert(!ssh->start(&options, &session)); assert(!ssh->send(session, "abc", 3));
    solar_os_native_ssh_event_v1_t event; bool available;
    assert(!ssh->poll(session, &event, &available) && available && event.type == SOLAR_OS_NATIVE_SSH_OUTPUT && event.data_size == 3 && !memcmp(event.data, "a\0b", 3));
    send_event = false; assert(!ssh->poll(session, &event, &available) && !available && !event.data_size);
    close_timeout = true; assert(ssh->close(session, 42) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    close_timeout = false; assert(!ssh->close(session, 42)); assert(ssh->close(session, 42) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_native_transfer_options_v1_t transfer = {options, "/local", "/remote", SOLAR_OS_NATIVE_TRANSFER_DOWNLOAD, SOLAR_OS_NATIVE_TRANSFER_GLOB};
    assert(!ssh->scp_start(&transfer, &session));
    assert(ssh->close(session, 42) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_native_transfer_event_v1_t transfer_event;
    assert(!ssh->scp_poll(session, &transfer_event, &available) && available && transfer_event.type == SOLAR_OS_NATIVE_TRANSFER_PROGRESS);
    assert(transfer_event.transferred == UINT64_C(5000000000) && transfer_event.total == UINT64_C(6000000000)); assert(!ssh->scp_close(session, 42));
    transfer.flags = SOLAR_OS_NATIVE_TRANSFER_RECURSIVE | SOLAR_OS_NATIVE_TRANSFER_DRY_RUN;
    assert(!ssh->sync_start(&transfer, &session)); assert(!ssh->sync_poll(session, &transfer_event, &available));
    assert(available && transfer_event.files_changed == 7 && transfer_event.file_size_known && transfer_event.file_size == UINT64_C(6000000000));
    assert(!ssh->sync_close(session, 42));
    solar_os_native_ssh_keys_v1_t keys; assert(!ssh->key_status(&keys) && keys.private_exists && keys.private_size == 1234 && !strcmp(keys.private_path, "/key"));
    assert(!sftp->create(&sftp_context)); assert(sftp->mkdir(sftp_context, "/remote") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!sftp->cancel(sftp_context, true)); assert(sftp->connect(sftp_context, &options, 42) == SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(!sftp->cancel(sftp_context, false)); assert(!sftp->connect(sftp_context, &options, 42));
    assert(sftp->connect(sftp_context, &options, 42) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!sftp->list(sftp_context, "/remote", visit, &visits) && visits == 1);
    assert(!sftp->download(sftp_context, "/remote", "/local", progress, &progress_calls) && progress_calls == 1);
    test_ftp();
    assert(!sftp->cancel(sftp_context, true));
    assert(sftp->upload(sftp_context, "/local", "/remote", progress, &progress_calls) == SOLAR_OS_NATIVE_ERROR_CANCELLED && progress_calls == 1);
    assert(!sftp->close(sftp_context)); assert(saved_cancel(saved_user));
    assert(!allocations && !solar_os_native_handle_count());
    puts("native SSH, SCP, sync and shared FTP/SFTP handles, cancellation and cleanup passed");
    return 0;
}
