#include "solar_os_sftpsync.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "libssh2.h"
#include "libssh2_sftp.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_ssh_transport.h"
#include "solar_os_task.h"

#define SOLAR_OS_SFTPSYNC_DEFAULT_PORT 22
#define SOLAR_OS_SFTPSYNC_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define SOLAR_OS_SFTPSYNC_EVENT_QUEUE_LEN 24
#define SOLAR_OS_SFTPSYNC_BUFFER_SIZE 4096
#define SOLAR_OS_SFTPSYNC_MAX_DEPTH 8
#define SOLAR_OS_SFTPSYNC_PROGRESS_STEP 32768U

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SOLAR_OS_SFTPSYNC_TASK_STACK);

struct solar_os_sftpsync_session {
    solar_os_sftpsync_direction_t direction;
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char password[SOLAR_OS_SSH_PASSWORD_MAX];
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    uint16_t port;
    bool recursive;
    bool dry_run;
    QueueHandle_t events;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool task_done;
    volatile bool detached;
    uint64_t transferred;
    size_t files_changed;
    uint64_t file_transferred;
    uint64_t file_size;
    bool file_size_known;
    char file_path[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
};

typedef struct {
    solar_os_sftpsync_session_t *session;
    solar_os_ssh_transport_t *transport;
    LIBSSH2_SFTP *sftp;
    char *buffer;
} sftpsync_worker_t;

static const char *TAG = "solar_os_sftpsync";

static bool sftpsync_should_stop(const solar_os_sftpsync_session_t *session)
{
    return session == NULL || session->stop_requested;
}

static void sftpsync_send_event(solar_os_sftpsync_session_t *session,
                             solar_os_sftpsync_event_type_t type,
                             const char *message)
{
    if (session == NULL || session->events == NULL) {
        return;
    }
    solar_os_sftpsync_event_t event = {
        .type = type,
        .transferred = session->transferred,
        .files_changed = session->files_changed,
        .file_transferred = session->file_transferred,
        .file_size = session->file_size,
        .file_size_known = session->file_size_known,
    };
    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    } else if (type == SOLAR_OS_SFTPSYNC_EVENT_PROGRESS) {
        strlcpy(event.message, session->file_path, sizeof(event.message));
    }
    (void)xQueueSend(session->events, &event, pdMS_TO_TICKS(50));
}

static void sftpsync_status(solar_os_sftpsync_session_t *session, const char *message)
{
    sftpsync_send_event(session, SOLAR_OS_SFTPSYNC_EVENT_STATUS, message);
}

static void sftpsync_error(solar_os_sftpsync_session_t *session, const char *message)
{
    SOLAR_OS_LOGE(TAG, "%s", message != NULL ? message : "failed");
    sftpsync_send_event(session, SOLAR_OS_SFTPSYNC_EVENT_ERROR, message);
}

static void sftpsync_begin_file(solar_os_sftpsync_session_t *session,
                                const char *path,
                                uint64_t size,
                                bool size_known)
{
    session->file_transferred = 0U;
    session->file_size = size;
    session->file_size_known = size_known;
    strlcpy(session->file_path, path != NULL ? path : "", sizeof(session->file_path));
    sftpsync_send_event(session, SOLAR_OS_SFTPSYNC_EVENT_PROGRESS, NULL);
}

static void sftpsync_file_progress(solar_os_sftpsync_session_t *session)
{
    sftpsync_send_event(session, SOLAR_OS_SFTPSYNC_EVENT_PROGRESS, NULL);
}

static bool sftpsync_transport_should_stop(void *user)
{
    return sftpsync_should_stop((solar_os_sftpsync_session_t *)user);
}

static void sftpsync_transport_status(void *user, const char *message)
{
    sftpsync_status((solar_os_sftpsync_session_t *)user, message);
}

static void sftpsync_transport_error(void *user, const char *message)
{
    sftpsync_error((solar_os_sftpsync_session_t *)user, message);
}

static solar_os_ssh_transport_config_t sftpsync_transport_config(solar_os_sftpsync_session_t *session)
{
    return (solar_os_ssh_transport_config_t){
        .host = session->host,
        .port = session->port,
        .username = session->username,
        .password = session->password,
        .log_tag = TAG,
        .user = session,
        .should_stop = sftpsync_transport_should_stop,
        .status = sftpsync_transport_status,
        .error = sftpsync_transport_error,
        .include_username_in_auth_status = true,
        .report_password_success = true,
        .report_publickey_success = true,
        .allow_unverified_host_key_without_storage = true,
        .include_error_code = true,
    };
}

static int sftpsync_wait(sftpsync_worker_t *worker)
{
    solar_os_ssh_transport_config_t config = sftpsync_transport_config(worker->session);
    return solar_os_ssh_transport_wait_socket(&config,
                                              worker->transport->socket_fd,
                                              worker->transport->session);
}

static bool sftpsync_join_path(char *out,
                            size_t out_len,
                            const char *parent,
                            const char *name)
{
    if (out == NULL || out_len == 0 || parent == NULL || name == NULL) {
        return false;
    }
    const size_t parent_len = strlen(parent);
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 parent,
                                 parent_len > 0 && parent[parent_len - 1] == '/' ? "" : "/",
                                 name);
    return written >= 0 && (size_t)written < out_len;
}

static const char *sftpsync_basename(const char *path)
{
    const char *end = path + strlen(path);
    while (end > path && end[-1] == '/') {
        end--;
    }
    const char *base = end;
    while (base > path && base[-1] != '/') {
        base--;
    }
    return base;
}

static bool sftpsync_format_action(char *message,
                                size_t message_len,
                                const char *action,
                                const char *path)
{
    const int written = snprintf(message, message_len, "%s %s", action, path);
    return written >= 0 && (size_t)written < message_len;
}

/* 1: exists, 0: not found, -1: error. */
static int sftpsync_remote_stat(sftpsync_worker_t *worker,
                             const char *path,
                             LIBSSH2_SFTP_ATTRIBUTES *attrs)
{
    memset(attrs, 0, sizeof(*attrs));
    while (!sftpsync_should_stop(worker->session)) {
        const int rc = libssh2_sftp_lstat(worker->sftp, path, attrs);
        if (rc == 0) {
            return 1;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftpsync_wait(worker);
            continue;
        }
        const unsigned long sftp_error = libssh2_sftp_last_error(worker->sftp);
        if (sftp_error == LIBSSH2_FX_NO_SUCH_FILE ||
            sftp_error == LIBSSH2_FX_NO_SUCH_PATH) {
            return 0;
        }
        return -1;
    }
    return -1;
}

static bool sftpsync_remote_is_dir(const LIBSSH2_SFTP_ATTRIBUTES *attrs)
{
    return (attrs->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
        LIBSSH2_SFTP_S_ISDIR(attrs->permissions);
}

static bool sftpsync_remote_is_regular(const LIBSSH2_SFTP_ATTRIBUTES *attrs)
{
    return (attrs->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
        LIBSSH2_SFTP_S_ISREG(attrs->permissions);
}

static bool sftpsync_file_changed(const struct stat *local,
                               const LIBSSH2_SFTP_ATTRIBUTES *remote)
{
    if ((remote->flags & LIBSSH2_SFTP_ATTR_SIZE) == 0 ||
        remote->filesize != (libssh2_uint64_t)local->st_size) {
        return true;
    }
    return (remote->flags & LIBSSH2_SFTP_ATTR_ACMODTIME) == 0 ||
        remote->mtime != (unsigned long)local->st_mtime;
}

static LIBSSH2_SFTP_HANDLE *sftpsync_remote_open(sftpsync_worker_t *worker,
                                              const char *path,
                                              unsigned long flags,
                                              long mode,
                                              int open_type)
{
    while (!sftpsync_should_stop(worker->session)) {
        LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(worker->sftp,
                                                           path,
                                                           strlen(path),
                                                           flags,
                                                           mode,
                                                           open_type);
        if (handle != NULL) {
            return handle;
        }
        if (libssh2_session_last_errno(worker->transport->session) != LIBSSH2_ERROR_EAGAIN) {
            return NULL;
        }
        (void)sftpsync_wait(worker);
    }
    return NULL;
}

static void sftpsync_remote_close(sftpsync_worker_t *worker, LIBSSH2_SFTP_HANDLE *handle)
{
    if (handle == NULL) {
        return;
    }
    int rc = 0;
    while (!sftpsync_should_stop(worker->session) &&
           (rc = libssh2_sftp_close(handle)) == LIBSSH2_ERROR_EAGAIN) {
        (void)sftpsync_wait(worker);
    }
    (void)rc;
}

static bool sftpsync_remote_mkdir(sftpsync_worker_t *worker, const char *path, long mode)
{
    if (worker->session->dry_run) {
        char message[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
        if (sftpsync_format_action(message, sizeof(message), "mkdir", path)) {
            sftpsync_status(worker->session, message);
        }
        return true;
    }
    while (!sftpsync_should_stop(worker->session)) {
        const int rc = libssh2_sftp_mkdir(worker->sftp, path, mode);
        if (rc == 0) {
            return true;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftpsync_wait(worker);
            continue;
        }
        if (libssh2_sftp_last_error(worker->sftp) == LIBSSH2_FX_FILE_ALREADY_EXISTS) {
            return true;
        }
        return false;
    }
    return false;
}

static bool sftpsync_local_mkdir(solar_os_sftpsync_session_t *session,
                              const char *path,
                              mode_t mode)
{
    if (session->dry_run) {
        char message[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
        if (sftpsync_format_action(message, sizeof(message), "mkdir", path)) {
            sftpsync_status(session, message);
        }
        return true;
    }
    return mkdir(path, mode & 0777) == 0 || errno == EEXIST;
}

static bool sftpsync_remote_set_times(sftpsync_worker_t *worker,
                                   const char *path,
                                   time_t mtime)
{
    LIBSSH2_SFTP_ATTRIBUTES attrs = {
        .flags = LIBSSH2_SFTP_ATTR_ACMODTIME,
        .atime = (unsigned long)mtime,
        .mtime = (unsigned long)mtime,
    };
    int rc = 0;
    while (!sftpsync_should_stop(worker->session) &&
           (rc = libssh2_sftp_setstat(worker->sftp, path, &attrs)) == LIBSSH2_ERROR_EAGAIN) {
        (void)sftpsync_wait(worker);
    }
    return rc == 0;
}

static void sftpsync_remote_unlink(sftpsync_worker_t *worker, const char *path)
{
    int rc = libssh2_sftp_unlink(worker->sftp, path);
    while (rc == LIBSSH2_ERROR_EAGAIN && !sftpsync_should_stop(worker->session)) {
        (void)sftpsync_wait(worker);
        rc = libssh2_sftp_unlink(worker->sftp, path);
    }
}

static bool sftpsync_remote_rename(sftpsync_worker_t *worker,
                                const char *source,
                                const char *destination)
{
    int rc = 0;
    while (!sftpsync_should_stop(worker->session) &&
           (rc = libssh2_sftp_posix_rename(worker->sftp, source, destination)) ==
               LIBSSH2_ERROR_EAGAIN) {
        (void)sftpsync_wait(worker);
    }
    if (rc == 0 || sftpsync_should_stop(worker->session)) {
        return rc == 0;
    }
    if (rc != (int)LIBSSH2_FX_OP_UNSUPPORTED &&
        libssh2_sftp_last_error(worker->sftp) != LIBSSH2_FX_OP_UNSUPPORTED) {
        return false;
    }

    /* Servers without the OpenSSH POSIX rename extension need a v3 fallback. */
    sftpsync_remote_unlink(worker, destination);
    while (!sftpsync_should_stop(worker->session) &&
           (rc = libssh2_sftp_rename_ex(worker->sftp,
                                        source,
                                        strlen(source),
                                        destination,
                                        strlen(destination),
                                        0)) == LIBSSH2_ERROR_EAGAIN) {
        (void)sftpsync_wait(worker);
    }
    return rc == 0;
}

static bool sftpsync_temp_path(char *out, size_t out_len, const char *path)
{
    const int written = snprintf(out, out_len, "%s.solaros-sftpsync-part", path);
    return written >= 0 && (size_t)written < out_len;
}

static bool sftpsync_upload_file(sftpsync_worker_t *worker,
                              const char *local_path,
                              const char *remote_path,
                              const struct stat *local_st)
{
    LIBSSH2_SFTP_ATTRIBUTES remote_attrs;
    const int remote_state = sftpsync_remote_stat(worker, remote_path, &remote_attrs);
    if (remote_state < 0) {
        sftpsync_error(worker->session, "remote stat failed");
        return false;
    }
    if (remote_state == 1 && sftpsync_remote_is_regular(&remote_attrs) &&
        !sftpsync_file_changed(local_st, &remote_attrs)) {
        return true;
    }

    worker->session->files_changed++;
    if (worker->session->dry_run) {
        char message[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
        if (sftpsync_format_action(message, sizeof(message), "send", remote_path)) {
            sftpsync_status(worker->session, message);
        }
        return true;
    }

    sftpsync_begin_file(worker->session,
                        remote_path,
                        (uint64_t)local_st->st_size,
                        true);

    FILE *local = fopen(local_path, "rb");
    if (local == NULL) {
        sftpsync_error(worker->session, "local file open failed");
        return false;
    }
    char remote_temp[SOLAR_OS_STORAGE_PATH_MAX];
    if (!sftpsync_temp_path(remote_temp, sizeof(remote_temp), remote_path)) {
        fclose(local);
        sftpsync_error(worker->session, "remote path too long");
        return false;
    }
    LIBSSH2_SFTP_HANDLE *remote = sftpsync_remote_open(worker,
                                                    remote_temp,
                                                    LIBSSH2_FXF_WRITE |
                                                        LIBSSH2_FXF_CREAT |
                                                        LIBSSH2_FXF_TRUNC,
                                                    local_st->st_mode & 0777,
                                                    LIBSSH2_SFTP_OPENFILE);
    if (remote == NULL) {
        fclose(local);
        sftpsync_error(worker->session, "remote file open failed");
        return false;
    }

    bool ok = true;
    size_t read_len = 0;
    uint64_t next_progress = 0U;
    while (!sftpsync_should_stop(worker->session) &&
           (read_len = fread(worker->buffer, 1, SOLAR_OS_SFTPSYNC_BUFFER_SIZE, local)) > 0) {
        size_t offset = 0;
        while (offset < read_len && !sftpsync_should_stop(worker->session)) {
            const ssize_t written = libssh2_sftp_write(remote,
                                                        worker->buffer + offset,
                                                        read_len - offset);
            if (written > 0) {
                offset += (size_t)written;
                worker->session->transferred += (uint64_t)written;
                worker->session->file_transferred += (uint64_t)written;
            } else if (written == LIBSSH2_ERROR_EAGAIN) {
                (void)sftpsync_wait(worker);
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        if (worker->session->file_transferred >= next_progress) {
            sftpsync_file_progress(worker->session);
            next_progress = worker->session->file_transferred +
                SOLAR_OS_SFTPSYNC_PROGRESS_STEP;
        }
    }
    if (ferror(local)) {
        ok = false;
    }
    if (sftpsync_should_stop(worker->session)) {
        ok = false;
    }
    fclose(local);
    sftpsync_remote_close(worker, remote);
    if (ok) {
        ok = sftpsync_remote_set_times(worker, remote_temp, local_st->st_mtime) &&
            sftpsync_remote_rename(worker, remote_temp, remote_path);
    }
    if (ok) {
        worker->session->file_transferred = worker->session->file_size;
        sftpsync_file_progress(worker->session);
    }
    if (!ok) {
        sftpsync_remote_unlink(worker, remote_temp);
        sftpsync_error(worker->session, "upload failed");
    }
    return ok && !sftpsync_should_stop(worker->session);
}

static bool sftpsync_download_file(sftpsync_worker_t *worker,
                                const char *remote_path,
                                const char *local_path,
                                const LIBSSH2_SFTP_ATTRIBUTES *remote_st)
{
    struct stat local_st;
    if (stat(local_path, &local_st) == 0 && S_ISREG(local_st.st_mode) &&
        !sftpsync_file_changed(&local_st, remote_st)) {
        return true;
    }

    worker->session->files_changed++;
    if (worker->session->dry_run) {
        char message[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
        if (sftpsync_format_action(message, sizeof(message), "recv", local_path)) {
            sftpsync_status(worker->session, message);
        }
        return true;
    }
    const bool size_known = (remote_st->flags & LIBSSH2_SFTP_ATTR_SIZE) != 0;
    sftpsync_begin_file(worker->session,
                        local_path,
                        size_known ? remote_st->filesize : 0U,
                        size_known);

    char local_temp[SOLAR_OS_STORAGE_PATH_MAX];
    if (!sftpsync_temp_path(local_temp, sizeof(local_temp), local_path)) {
        sftpsync_error(worker->session, "local path too long");
        return false;
    }
    LIBSSH2_SFTP_HANDLE *remote = sftpsync_remote_open(worker,
                                                    remote_path,
                                                    LIBSSH2_FXF_READ,
                                                    0,
                                                    LIBSSH2_SFTP_OPENFILE);
    if (remote == NULL) {
        sftpsync_error(worker->session, "remote file open failed");
        return false;
    }
    FILE *local = fopen(local_temp, "wb");
    if (local == NULL) {
        sftpsync_remote_close(worker, remote);
        sftpsync_error(worker->session, "local file create failed");
        return false;
    }

    bool ok = true;
    uint64_t remaining = (remote_st->flags & LIBSSH2_SFTP_ATTR_SIZE) != 0 ?
        remote_st->filesize : UINT64_MAX;
    uint64_t next_progress = 0U;
    while (!sftpsync_should_stop(worker->session) && remaining > 0) {
        size_t want = SOLAR_OS_SFTPSYNC_BUFFER_SIZE;
        if (remaining < want) {
            want = (size_t)remaining;
        }
        const ssize_t read_len = libssh2_sftp_read(remote, worker->buffer, want);
        if (read_len > 0) {
            if (fwrite(worker->buffer, 1, (size_t)read_len, local) != (size_t)read_len) {
                ok = false;
                break;
            }
            worker->session->transferred += (uint64_t)read_len;
            worker->session->file_transferred += (uint64_t)read_len;
            if (remaining != UINT64_MAX) {
                remaining -= (uint64_t)read_len;
            }
            if (worker->session->file_transferred >= next_progress) {
                sftpsync_file_progress(worker->session);
                next_progress = worker->session->file_transferred +
                    SOLAR_OS_SFTPSYNC_PROGRESS_STEP;
            }
        } else if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)sftpsync_wait(worker);
        } else if (read_len == 0) {
            break;
        } else {
            ok = false;
            break;
        }
    }
    if (fclose(local) != 0) {
        ok = false;
    }
    if (sftpsync_should_stop(worker->session)) {
        ok = false;
    }
    sftpsync_remote_close(worker, remote);
    if (ok && (remote_st->flags & LIBSSH2_SFTP_ATTR_ACMODTIME) != 0) {
        const struct utimbuf times = {
            .actime = (time_t)remote_st->mtime,
            .modtime = (time_t)remote_st->mtime,
        };
        ok = utime(local_temp, &times) == 0;
    }
    if (ok) {
        ok = rename(local_temp, local_path) == 0;
    }
    if (ok) {
        if (worker->session->file_size_known) {
            worker->session->file_transferred = worker->session->file_size;
        } else {
            worker->session->file_size = worker->session->file_transferred;
            worker->session->file_size_known = true;
        }
        sftpsync_file_progress(worker->session);
    }
    if (!ok) {
        (void)unlink(local_temp);
        sftpsync_error(worker->session, "download failed");
    }
    return ok && !sftpsync_should_stop(worker->session);
}

static bool sftpsync_upload_path(sftpsync_worker_t *worker,
                              const char *local_path,
                              const char *remote_path,
                              unsigned depth)
{
    if (depth > SOLAR_OS_SFTPSYNC_MAX_DEPTH) {
        sftpsync_error(worker->session, "directory nesting too deep");
        return false;
    }
    struct stat local_st;
    if (stat(local_path, &local_st) != 0) {
        sftpsync_error(worker->session, "local path unavailable");
        return false;
    }
    if (S_ISREG(local_st.st_mode)) {
        return sftpsync_upload_file(worker, local_path, remote_path, &local_st);
    }
    if (!S_ISDIR(local_st.st_mode)) {
        sftpsync_status(worker->session, "skip non-regular local entry");
        return true;
    }
    if (!worker->session->recursive) {
        sftpsync_error(worker->session, "directory requires -r or -a");
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES remote_st;
    const int remote_state = sftpsync_remote_stat(worker, remote_path, &remote_st);
    if (remote_state < 0 ||
        (remote_state == 1 && !sftpsync_remote_is_dir(&remote_st)) ||
        (remote_state == 0 && !sftpsync_remote_mkdir(worker, remote_path, local_st.st_mode))) {
        sftpsync_error(worker->session, "remote directory unavailable");
        return false;
    }

    DIR *dir = opendir(local_path);
    if (dir == NULL) {
        sftpsync_error(worker->session, "local directory open failed");
        return false;
    }
    bool ok = true;
    struct dirent *entry = NULL;
    while (ok && !sftpsync_should_stop(worker->session) && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
#ifdef DT_LNK
        if (entry->d_type == DT_LNK) {
            sftpsync_status(worker->session, "skip local symbolic link");
            continue;
        }
#endif
        char local_child[SOLAR_OS_STORAGE_PATH_MAX];
        char remote_child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!sftpsync_join_path(local_child, sizeof(local_child), local_path, entry->d_name) ||
            !sftpsync_join_path(remote_child, sizeof(remote_child), remote_path, entry->d_name)) {
            sftpsync_error(worker->session, "path too long");
            ok = false;
            break;
        }
        ok = sftpsync_upload_path(worker, local_child, remote_child, depth + 1);
    }
    closedir(dir);
    return ok && !sftpsync_should_stop(worker->session);
}

static bool sftpsync_download_path(sftpsync_worker_t *worker,
                                const char *remote_path,
                                const char *local_path,
                                const LIBSSH2_SFTP_ATTRIBUTES *known_remote_st,
                                unsigned depth)
{
    if (depth > SOLAR_OS_SFTPSYNC_MAX_DEPTH) {
        sftpsync_error(worker->session, "directory nesting too deep");
        return false;
    }
    LIBSSH2_SFTP_ATTRIBUTES remote_st;
    if (known_remote_st != NULL) {
        remote_st = *known_remote_st;
    } else if (sftpsync_remote_stat(worker, remote_path, &remote_st) != 1) {
        sftpsync_error(worker->session, "remote path unavailable");
        return false;
    }
    if (sftpsync_remote_is_regular(&remote_st)) {
        return sftpsync_download_file(worker, remote_path, local_path, &remote_st);
    }
    if (!sftpsync_remote_is_dir(&remote_st)) {
        sftpsync_status(worker->session, "skip non-regular remote entry");
        return true;
    }
    if (!worker->session->recursive) {
        sftpsync_error(worker->session, "directory requires -r or -a");
        return false;
    }

    struct stat local_st;
    if (stat(local_path, &local_st) != 0) {
        if (!sftpsync_local_mkdir(worker->session, local_path, 0777)) {
            sftpsync_error(worker->session, "local directory create failed");
            return false;
        }
    } else if (!S_ISDIR(local_st.st_mode)) {
        sftpsync_error(worker->session, "local target is not a directory");
        return false;
    }

    LIBSSH2_SFTP_HANDLE *dir = sftpsync_remote_open(worker,
                                                 remote_path,
                                                 0,
                                                 0,
                                                 LIBSSH2_SFTP_OPENDIR);
    if (dir == NULL) {
        sftpsync_error(worker->session, "remote directory open failed");
        return false;
    }
    bool ok = true;
    while (ok && !sftpsync_should_stop(worker->session)) {
        char name[256];
        LIBSSH2_SFTP_ATTRIBUTES child_st;
        const int read_len = libssh2_sftp_readdir(dir, name, sizeof(name) - 1, &child_st);
        if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)sftpsync_wait(worker);
            continue;
        }
        if (read_len == 0) {
            break;
        }
        if (read_len < 0) {
            sftpsync_error(worker->session, "remote directory read failed");
            ok = false;
            break;
        }
        name[read_len] = '\0';
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
            continue;
        }
        char remote_child[SOLAR_OS_STORAGE_PATH_MAX];
        char local_child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!sftpsync_join_path(remote_child, sizeof(remote_child), remote_path, name) ||
            !sftpsync_join_path(local_child, sizeof(local_child), local_path, name)) {
            sftpsync_error(worker->session, "path too long");
            ok = false;
            break;
        }
        ok = sftpsync_download_path(worker,
                                 remote_child,
                                 local_child,
                                 &child_st,
                                 depth + 1);
    }
    sftpsync_remote_close(worker, dir);
    return ok && !sftpsync_should_stop(worker->session);
}

static LIBSSH2_SFTP *sftpsync_sftp_init(sftpsync_worker_t *worker)
{
    while (!sftpsync_should_stop(worker->session)) {
        LIBSSH2_SFTP *sftp = libssh2_sftp_init(worker->transport->session);
        if (sftp != NULL) {
            return sftp;
        }
        if (libssh2_session_last_errno(worker->transport->session) != LIBSSH2_ERROR_EAGAIN) {
            return NULL;
        }
        (void)sftpsync_wait(worker);
    }
    return NULL;
}

static void sftpsync_sftp_shutdown(sftpsync_worker_t *worker)
{
    if (worker->sftp == NULL) {
        return;
    }
    int rc = 0;
    while (!sftpsync_should_stop(worker->session) &&
           (rc = libssh2_sftp_shutdown(worker->sftp)) == LIBSSH2_ERROR_EAGAIN) {
        (void)sftpsync_wait(worker);
    }
    worker->sftp = NULL;
    (void)rc;
}

static void sftpsync_session_destroy(solar_os_sftpsync_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->events != NULL) {
        solar_os_queue_delete(session->events);
    }
    memset(session->password, 0, sizeof(session->password));
    solar_os_memory_free(session);
}

static void sftpsync_session_task(void *arg)
{
    solar_os_sftpsync_session_t *session = (solar_os_sftpsync_session_t *)arg;
    solar_os_ssh_transport_t transport = {.socket_fd = -1};
    solar_os_ssh_transport_config_t config = sftpsync_transport_config(session);
    sftpsync_worker_t worker = {
        .session = session,
        .transport = &transport,
    };
    bool ok = false;

    if (solar_os_ssh_transport_open(&config, &transport) != ESP_OK ||
        sftpsync_should_stop(session)) {
        goto done;
    }
    sftpsync_status(session, "starting SFTP");
    worker.sftp = sftpsync_sftp_init(&worker);
    if (worker.sftp == NULL) {
        sftpsync_error(session, "SFTP unavailable");
        goto done;
    }
    worker.buffer = solar_os_memory_alloc(SOLAR_OS_SFTPSYNC_BUFFER_SIZE,
                                          SOLAR_OS_MEMORY_TRANSIENT,
                                          "sftpsync.buffer");
    if (worker.buffer == NULL) {
        sftpsync_error(session, "transfer buffer allocation failed");
        goto done;
    }

    if (session->direction == SOLAR_OS_SFTPSYNC_UPLOAD) {
        struct stat local_st;
        LIBSSH2_SFTP_ATTRIBUTES remote_st;
        char remote_target[SOLAR_OS_STORAGE_PATH_MAX];
        strlcpy(remote_target, session->remote_path, sizeof(remote_target));
        if (stat(session->local_path, &local_st) != 0) {
            sftpsync_error(session, "local path unavailable");
            goto done;
        }
        if (S_ISREG(local_st.st_mode) &&
            sftpsync_remote_stat(&worker, remote_target, &remote_st) == 1 &&
            sftpsync_remote_is_dir(&remote_st) &&
            !sftpsync_join_path(remote_target,
                             sizeof(remote_target),
                             session->remote_path,
                             sftpsync_basename(session->local_path))) {
            sftpsync_error(session, "remote path too long");
            goto done;
        }
        ok = sftpsync_upload_path(&worker, session->local_path, remote_target, 0);
    } else {
        LIBSSH2_SFTP_ATTRIBUTES remote_st;
        char local_target[SOLAR_OS_STORAGE_PATH_MAX];
        strlcpy(local_target, session->local_path, sizeof(local_target));
        if (sftpsync_remote_stat(&worker, session->remote_path, &remote_st) != 1) {
            sftpsync_error(session, "remote path unavailable");
            goto done;
        }
        struct stat local_st;
        if (sftpsync_remote_is_regular(&remote_st) &&
            stat(local_target, &local_st) == 0 && S_ISDIR(local_st.st_mode) &&
            !sftpsync_join_path(local_target,
                             sizeof(local_target),
                             session->local_path,
                             sftpsync_basename(session->remote_path))) {
            sftpsync_error(session, "local path too long");
            goto done;
        }
        ok = sftpsync_download_path(&worker,
                                 session->remote_path,
                                 local_target,
                                 &remote_st,
                                 0);
    }

done:
    solar_os_memory_free(worker.buffer);
    sftpsync_sftp_shutdown(&worker);
    solar_os_ssh_transport_close(&transport, "SolarOS sftpsync shutdown");
    sftpsync_send_event(session,
                     SOLAR_OS_SFTPSYNC_EVENT_DONE,
                     ok ? (session->dry_run ? "dry run complete" : "done") : "failed");
    const bool detached = session->detached;
    session->task_done = true;
    if (detached) {
        sftpsync_session_destroy(session);
    }
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_sftpsync_start(const solar_os_sftpsync_config_t *config,
                               solar_os_sftpsync_session_t **session_out)
{
    if (config == NULL || session_out == NULL ||
        config->host == NULL || config->host[0] == '\0' ||
        config->username == NULL || config->username[0] == '\0' ||
        config->local_path == NULL || config->local_path[0] == '\0' ||
        config->remote_path == NULL || config->remote_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_sftpsync_session_t *session = solar_os_memory_calloc(
        1,
        sizeof(*session),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "sftpsync.session");
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    session->direction = config->direction;
    session->port = config->port != 0 ? config->port : SOLAR_OS_SFTPSYNC_DEFAULT_PORT;
    session->recursive = config->recursive;
    session->dry_run = config->dry_run;
    strlcpy(session->host, config->host, sizeof(session->host));
    strlcpy(session->username, config->username, sizeof(session->username));
    strlcpy(session->password, config->password != NULL ? config->password : "", sizeof(session->password));
    strlcpy(session->local_path, config->local_path, sizeof(session->local_path));
    strlcpy(session->remote_path, config->remote_path, sizeof(session->remote_path));
    session->events = solar_os_queue_create(SOLAR_OS_SFTPSYNC_EVENT_QUEUE_LEN,
                                             sizeof(solar_os_sftpsync_event_t));
    if (session->events == NULL) {
        sftpsync_session_destroy(session);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = solar_os_task_create_pinned_internal(
        sftpsync_session_task,
        "solar_os_sftpsync",
        SOLAR_OS_SFTPSYNC_TASK_STACK,
        session,
        SOLAR_OS_SFTPSYNC_TASK_PRIORITY,
        &session->task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        sftpsync_session_destroy(session);
        return ESP_ERR_NO_MEM;
    }
    *session_out = session;
    return ESP_OK;
}

bool solar_os_sftpsync_stop(solar_os_sftpsync_session_t *session)
{
    if (session == NULL) {
        return true;
    }
    session->stop_requested = true;
    if (!solar_os_task_wait_done(session->task,
                                 &session->task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        session->detached = true;
        SOLAR_OS_LOGW(TAG, "sftpsync task did not stop; detached for worker cleanup");
        return false;
    }
    sftpsync_session_destroy(session);
    return true;
}

bool solar_os_sftpsync_poll(solar_os_sftpsync_session_t *session,
                         solar_os_sftpsync_event_t *event)
{
    if (session == NULL || event == NULL || session->events == NULL) {
        return false;
    }
    return xQueueReceive(session->events, event, 0) == pdTRUE;
}
