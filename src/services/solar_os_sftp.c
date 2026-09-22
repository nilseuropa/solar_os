#include "solar_os_sftp.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libssh2.h"
#include "libssh2_sftp.h"
#include "solar_os_memory.h"
#include "solar_os_ssh.h"
#include "solar_os_ssh_transport.h"
#include "solar_os_storage.h"

#define SFTP_IO_CHUNK 4096U

struct solar_os_sftp_session {
    solar_os_ssh_transport_t transport;
    LIBSSH2_SFTP *sftp;
    solar_os_sftp_cancel_fn_t should_cancel;
    void *cancel_user;
    uint16_t port;
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char password[SOLAR_OS_SSH_PASSWORD_MAX];
    char last_error[SOLAR_OS_SFTP_ERROR_MAX];
};

static const char *TAG = "solar_os_sftp";

static bool sftp_cancelled(const solar_os_sftp_session_t *session)
{
    return session == NULL ||
        (session->should_cancel != NULL && session->should_cancel(session->cancel_user));
}

static bool sftp_transport_should_stop(void *user)
{
    return sftp_cancelled((const solar_os_sftp_session_t *)user);
}

static void sftp_set_error(solar_os_sftp_session_t *session, const char *message)
{
    if (session != NULL) {
        strlcpy(session->last_error,
                message != NULL ? message : "SFTP operation failed",
                sizeof(session->last_error));
    }
}

static void sftp_transport_error(void *user, const char *message)
{
    sftp_set_error((solar_os_sftp_session_t *)user, message);
}

static solar_os_ssh_transport_config_t sftp_transport_config(
    solar_os_sftp_session_t *session)
{
    return (solar_os_ssh_transport_config_t){
        .host = session->host,
        .port = session->port,
        .username = session->username,
        .password = session->password,
        .log_tag = TAG,
        .user = session,
        .should_stop = sftp_transport_should_stop,
        .error = sftp_transport_error,
        .include_username_in_auth_status = true,
        .allow_unverified_host_key_without_storage = true,
        .include_error_code = true,
    };
}

static int sftp_wait(solar_os_sftp_session_t *session)
{
    solar_os_ssh_transport_config_t config = sftp_transport_config(session);
    return solar_os_ssh_transport_wait_socket(&config,
                                              session->transport.socket_fd,
                                              session->transport.session);
}

static esp_err_t sftp_failed(solar_os_sftp_session_t *session, const char *operation)
{
    if (sftp_cancelled(session)) {
        sftp_set_error(session, "cancelled");
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned long code = session->sftp != NULL ?
        libssh2_sftp_last_error(session->sftp) : 0U;
    char message[SOLAR_OS_SFTP_ERROR_MAX];
    snprintf(message, sizeof(message), "%s failed (SFTP %lu)", operation, code);
    sftp_set_error(session, message);
    return ESP_FAIL;
}

static LIBSSH2_SFTP_HANDLE *sftp_open(solar_os_sftp_session_t *session,
                                      const char *path,
                                      unsigned long flags,
                                      long mode,
                                      int open_type)
{
    while (!sftp_cancelled(session)) {
        LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(session->sftp,
                                                           path,
                                                           strlen(path),
                                                           flags,
                                                           mode,
                                                           open_type);
        if (handle != NULL) {
            return handle;
        }
        if (libssh2_session_last_errno(session->transport.session) !=
            LIBSSH2_ERROR_EAGAIN) {
            return NULL;
        }
        (void)sftp_wait(session);
    }
    return NULL;
}

static esp_err_t sftp_close_handle(solar_os_sftp_session_t *session,
                                    LIBSSH2_SFTP_HANDLE *handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }
    int rc;
    do {
        rc = libssh2_sftp_close(handle);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
    return rc == 0 ? ESP_OK : sftp_failed(session, "close");
}

static esp_err_t sftp_staging_paths(const char *path,
                                     char *staged,
                                     size_t staged_len,
                                     char *backup,
                                     size_t backup_len)
{
    const int staged_written = snprintf(staged, staged_len, "%s.solaros-sftp-part", path);
    const int backup_written = snprintf(backup, backup_len, "%s.solaros-sftp-backup", path);
    return staged_written >= 0 && (size_t)staged_written < staged_len &&
        backup_written >= 0 && (size_t)backup_written < backup_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t sftp_remote_temp_path(const char *path, char *temp, size_t temp_len)
{
    const int written = snprintf(temp, temp_len, "%s.solaros-sftp-part", path);
    return written >= 0 && (size_t)written < temp_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t sftp_unlink_internal(solar_os_sftp_session_t *session,
                                      const char *path,
                                      bool report_error)
{
    int rc;
    do {
        rc = libssh2_sftp_unlink(session->sftp, path);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
    return rc == 0 ? ESP_OK : report_error ? sftp_failed(session, "remove") : ESP_FAIL;
}

static esp_err_t sftp_rename_internal(solar_os_sftp_session_t *session,
                                      const char *old_path,
                                      const char *new_path,
                                      bool replace)
{
    int rc;
    do {
        rc = libssh2_sftp_posix_rename(session->sftp, old_path, new_path);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
    if (rc == 0) {
        return ESP_OK;
    }
    if (sftp_cancelled(session)) {
        return sftp_failed(session, "rename");
    }
    if (rc != (int)LIBSSH2_FX_OP_UNSUPPORTED &&
        libssh2_sftp_last_error(session->sftp) != LIBSSH2_FX_OP_UNSUPPORTED) {
        return sftp_failed(session, "rename");
    }

    if (replace) {
        (void)sftp_unlink_internal(session, new_path, false);
    }
    do {
        rc = libssh2_sftp_rename_ex(session->sftp,
                                    old_path,
                                    strlen(old_path),
                                    new_path,
                                    strlen(new_path),
                                    0);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
    return rc == 0 ? ESP_OK : sftp_failed(session, "rename");
}

esp_err_t solar_os_sftp_connect(const solar_os_sftp_options_t *options,
                                solar_os_sftp_cancel_fn_t should_cancel,
                                void *cancel_user,
                                solar_os_sftp_session_t **session_out)
{
    if (options == NULL || session_out == NULL || options->host == NULL ||
        options->host[0] == '\0' || options->username == NULL ||
        options->username[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *session_out = NULL;
    solar_os_sftp_session_t *session = solar_os_memory_calloc(
        1, sizeof(*session), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sftp.session");
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    session->transport.socket_fd = -1;
    session->should_cancel = should_cancel;
    session->cancel_user = cancel_user;
    session->port = options->port != 0U ? options->port : SOLAR_OS_SFTP_DEFAULT_PORT;
    strlcpy(session->host, options->host, sizeof(session->host));
    strlcpy(session->username, options->username, sizeof(session->username));
    strlcpy(session->password,
            options->password != NULL ? options->password : "",
            sizeof(session->password));

    solar_os_ssh_transport_config_t config = sftp_transport_config(session);
    esp_err_t err = solar_os_ssh_transport_open(&config, &session->transport);
    while (err == ESP_OK && !sftp_cancelled(session)) {
        session->sftp = libssh2_sftp_init(session->transport.session);
        if (session->sftp != NULL) {
            break;
        }
        if (libssh2_session_last_errno(session->transport.session) !=
            LIBSSH2_ERROR_EAGAIN) {
            err = sftp_failed(session, "SFTP startup");
            break;
        }
        (void)sftp_wait(session);
    }
    if (err == ESP_OK && session->sftp == NULL) {
        err = sftp_failed(session, "SFTP startup");
    }
    if (err != ESP_OK) {
        solar_os_sftp_disconnect(session);
        return err;
    }
    *session_out = session;
    return ESP_OK;
}

void solar_os_sftp_disconnect(solar_os_sftp_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->sftp != NULL) {
        int rc;
        do {
            rc = libssh2_sftp_shutdown(session->sftp);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                (void)sftp_wait(session);
            }
        } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
        session->sftp = NULL;
    }
    solar_os_ssh_transport_close(&session->transport, "SolarOS SFTP shutdown");
    memset(session->password, 0, sizeof(session->password));
    solar_os_memory_free(session);
}

const char *solar_os_sftp_last_error(const solar_os_sftp_session_t *session)
{
    return session != NULL ? session->last_error : "SFTP session unavailable";
}

esp_err_t solar_os_sftp_list(solar_os_sftp_session_t *session,
                             const char *path,
                             solar_os_sftp_entry_fn_t on_entry,
                             void *entry_user)
{
    if (session == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    LIBSSH2_SFTP_HANDLE *directory = sftp_open(session,
                                               path,
                                               0,
                                               0,
                                               LIBSSH2_SFTP_OPENDIR);
    if (directory == NULL) {
        return sftp_failed(session, "directory open");
    }
    esp_err_t err = ESP_OK;
    while (!sftp_cancelled(session)) {
        char name[256];
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        const int read_len = libssh2_sftp_readdir(directory,
                                                  name,
                                                  sizeof(name) - 1U,
                                                  &attrs);
        if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
            continue;
        }
        if (read_len == 0) {
            break;
        }
        if (read_len < 0) {
            err = sftp_failed(session, "directory read");
            break;
        }
        name[read_len] = '\0';
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
            (size_t)read_len >= SOLAR_OS_SFTP_NAME_MAX) {
            continue;
        }
        solar_os_sftp_entry_t entry = {0};
        strlcpy(entry.name, name, sizeof(entry.name));
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0) {
            entry.is_directory = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        }
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0) {
            entry.size = attrs.filesize;
        }
        if (on_entry != NULL && !on_entry(&entry, entry_user)) {
            err = ESP_ERR_NO_MEM;
            break;
        }
    }
    if (sftp_cancelled(session) && err == ESP_OK) {
        err = sftp_failed(session, "directory read");
    }
    const esp_err_t close_err = sftp_close_handle(session, directory);
    return err == ESP_OK ? close_err : err;
}

esp_err_t solar_os_sftp_download(solar_os_sftp_session_t *session,
                                 const char *remote_path,
                                 const char *local_path,
                                 solar_os_sftp_progress_fn_t progress,
                                 void *progress_user)
{
    if (session == NULL || remote_path == NULL || remote_path[0] == '\0' ||
        local_path == NULL || local_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat active_info;
    if (stat(local_path, &active_info) == 0 && !S_ISREG(active_info.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    char staged[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = sftp_staging_paths(local_path,
                                       staged,
                                       sizeof(staged),
                                       backup,
                                       sizeof(backup));
    if (err != ESP_OK) {
        return err;
    }
    LIBSSH2_SFTP_HANDLE *remote = sftp_open(session,
                                            remote_path,
                                            LIBSSH2_FXF_READ,
                                            0,
                                            LIBSSH2_SFTP_OPENFILE);
    if (remote == NULL) {
        return sftp_failed(session, "download open");
    }
    FILE *local = fopen(staged, "wb");
    if (local == NULL) {
        (void)sftp_close_handle(session, remote);
        sftp_set_error(session, "local staging file create failed");
        return ESP_FAIL;
    }
    uint8_t *buffer = solar_os_memory_alloc(SFTP_IO_CHUNK,
                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                             "sftp.download");
    if (buffer == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    uint64_t total = 0U;
    while (err == ESP_OK && !sftp_cancelled(session)) {
        const ssize_t read_len = libssh2_sftp_read(remote, buffer, SFTP_IO_CHUNK);
        if (read_len > 0) {
            if (fwrite(buffer, 1, (size_t)read_len, local) != (size_t)read_len) {
                sftp_set_error(session, "local file write failed");
                err = ESP_FAIL;
                break;
            }
            total += (uint64_t)read_len;
            if (progress != NULL) {
                progress(total, progress_user);
            }
        } else if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        } else if (read_len == 0) {
            break;
        } else {
            err = sftp_failed(session, "download");
        }
    }
    if (sftp_cancelled(session) && err == ESP_OK) {
        err = sftp_failed(session, "download");
    }
    solar_os_memory_free(buffer);
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(local);
    }
    if (fclose(local) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    const esp_err_t close_err = sftp_close_handle(session, remote);
    if (err == ESP_OK) {
        err = close_err;
    }
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(staged, local_path, backup);
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(staged);
    }
    return err;
}

esp_err_t solar_os_sftp_upload(solar_os_sftp_session_t *session,
                               const char *local_path,
                               const char *remote_path,
                               solar_os_sftp_progress_fn_t progress,
                               void *progress_user)
{
    if (session == NULL || local_path == NULL || local_path[0] == '\0' ||
        remote_path == NULL || remote_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *local = fopen(local_path, "rb");
    if (local == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char remote_temp[SOLAR_OS_STORAGE_PATH_MAX] = {0};
    esp_err_t err = sftp_remote_temp_path(remote_path,
                                          remote_temp,
                                          sizeof(remote_temp));
    LIBSSH2_SFTP_HANDLE *remote = err == ESP_OK ?
        sftp_open(session,
                  remote_temp,
                  LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                  0644,
                  LIBSSH2_SFTP_OPENFILE) : NULL;
    if (err == ESP_OK && remote == NULL) {
        err = sftp_failed(session, "upload open");
    }
    uint8_t *buffer = err == ESP_OK ?
        solar_os_memory_alloc(SFTP_IO_CHUNK,
                              SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                              "sftp.upload") : NULL;
    if (err == ESP_OK && buffer == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    uint64_t total = 0U;
    while (err == ESP_OK && !sftp_cancelled(session)) {
        const size_t read_len = fread(buffer, 1, SFTP_IO_CHUNK, local);
        size_t offset = 0U;
        while (offset < read_len && !sftp_cancelled(session)) {
            const ssize_t written = libssh2_sftp_write(remote,
                                                        buffer + offset,
                                                        read_len - offset);
            if (written > 0) {
                offset += (size_t)written;
                total += (uint64_t)written;
                if (progress != NULL) {
                    progress(total, progress_user);
                }
            } else if (written == LIBSSH2_ERROR_EAGAIN) {
                (void)sftp_wait(session);
            } else {
                err = sftp_failed(session, "upload");
                break;
            }
        }
        if (read_len < SFTP_IO_CHUNK) {
            if (ferror(local)) {
                sftp_set_error(session, "local file read failed");
                err = ESP_FAIL;
            }
            break;
        }
    }
    if (sftp_cancelled(session) && err == ESP_OK) {
        err = sftp_failed(session, "upload");
    }
    solar_os_memory_free(buffer);
    fclose(local);
    const esp_err_t close_err = sftp_close_handle(session, remote);
    if (err == ESP_OK) {
        err = close_err;
    }
    if (err == ESP_OK) {
        err = sftp_rename_internal(session, remote_temp, remote_path, true);
    }
    if (err != ESP_OK && remote_temp[0] != '\0') {
        (void)sftp_unlink_internal(session, remote_temp, false);
    }
    return err;
}

esp_err_t solar_os_sftp_mkdir(solar_os_sftp_session_t *session, const char *path)
{
    if (session == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int rc;
    do {
        rc = libssh2_sftp_mkdir(session->sftp, path, 0777);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
    return rc == 0 ? ESP_OK : sftp_failed(session, "mkdir");
}

esp_err_t solar_os_sftp_rmdir(solar_os_sftp_session_t *session, const char *path)
{
    if (session == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int rc;
    do {
        rc = libssh2_sftp_rmdir(session->sftp, path);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)sftp_wait(session);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN && !sftp_cancelled(session));
    return rc == 0 ? ESP_OK : sftp_failed(session, "rmdir");
}

esp_err_t solar_os_sftp_remove(solar_os_sftp_session_t *session, const char *path)
{
    if (session == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return sftp_unlink_internal(session, path, true);
}

esp_err_t solar_os_sftp_rename(solar_os_sftp_session_t *session,
                               const char *old_path,
                               const char *new_path)
{
    if (session == NULL || old_path == NULL || old_path[0] == '\0' ||
        new_path == NULL || new_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return sftp_rename_internal(session, old_path, new_path, true);
}
