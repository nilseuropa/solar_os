#include "solar_os_remote_files.h"

#include "solar_os_ftp.h"
#include "solar_os_sftp.h"

esp_err_t solar_os_remote_files_connect(solar_os_remote_files_t *session,
    solar_os_remote_file_protocol_t protocol, const solar_os_remote_file_options_t *options,
    solar_os_remote_file_cancel_fn should_cancel, void *cancel_user)
{
    if (!session || !options) return ESP_ERR_INVALID_ARG;
    if (session->connection) return ESP_ERR_INVALID_STATE;
    esp_err_t error;
    if (protocol == SOLAR_OS_REMOTE_FILE_FTP) {
        solar_os_ftp_session_t *ftp = NULL;
        error = solar_os_ftp_connect(options, should_cancel, cancel_user, &ftp);
        if (error == ESP_OK) session->connection = ftp;
    } else if (protocol == SOLAR_OS_REMOTE_FILE_SFTP) {
        const solar_os_sftp_options_t settings = {
            .host = options->host, .port = options->port, .username = options->username,
            .password = options->password, .timeout_ms = options->timeout_ms,
        };
        solar_os_sftp_session_t *sftp = NULL;
        error = solar_os_sftp_connect(&settings, should_cancel, cancel_user, &sftp);
        if (error == ESP_OK) session->connection = sftp;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) session->protocol = protocol;
    return error;
}

void solar_os_remote_files_disconnect(solar_os_remote_files_t *session)
{
    if (!session || !session->connection) return;
    if (session->protocol == SOLAR_OS_REMOTE_FILE_FTP) {
        solar_os_ftp_disconnect(session->connection);
    } else {
        solar_os_sftp_disconnect(session->connection);
    }
    session->connection = NULL;
}

const char *solar_os_remote_files_error(const solar_os_remote_files_t *session)
{
    if (!session) return "";
    return session->protocol == SOLAR_OS_REMOTE_FILE_FTP ? solar_os_ftp_last_reply(session->connection) :
        solar_os_sftp_last_error(session->connection);
}

#define FILE_CALL(session, operation, ...) \
    ((session)->protocol == SOLAR_OS_REMOTE_FILE_FTP ? \
        solar_os_ftp_##operation((session)->connection, __VA_ARGS__) : \
        solar_os_sftp_##operation((session)->connection, __VA_ARGS__))

esp_err_t solar_os_remote_files_list(solar_os_remote_files_t *session, const char *path,
    solar_os_remote_file_entry_fn on_entry, void *user)
{
    if (!session || !session->connection) return ESP_ERR_INVALID_STATE;
    return FILE_CALL(session, list, path, on_entry, user);
}

esp_err_t solar_os_remote_files_download(solar_os_remote_files_t *session, const char *remote,
    const char *local, solar_os_remote_file_progress_fn progress, void *user)
{
    if (!session || !session->connection) return ESP_ERR_INVALID_STATE;
    return FILE_CALL(session, download, remote, local, progress, user);
}

esp_err_t solar_os_remote_files_upload(solar_os_remote_files_t *session, const char *local,
    const char *remote, solar_os_remote_file_progress_fn progress, void *user)
{
    if (!session || !session->connection) return ESP_ERR_INVALID_STATE;
    return FILE_CALL(session, upload, local, remote, progress, user);
}

#define FILE_PATH_OPERATION(operation) \
esp_err_t solar_os_remote_files_##operation(solar_os_remote_files_t *session, const char *path) \
{ \
    if (!session || !session->connection) return ESP_ERR_INVALID_STATE; \
    return FILE_CALL(session, operation, path); \
}
FILE_PATH_OPERATION(mkdir)
FILE_PATH_OPERATION(rmdir)
FILE_PATH_OPERATION(remove)
#undef FILE_PATH_OPERATION

esp_err_t solar_os_remote_files_rename(solar_os_remote_files_t *session, const char *from, const char *to)
{
    if (!session || !session->connection) return ESP_ERR_INVALID_STATE;
    return FILE_CALL(session, rename, from, to);
}
#undef FILE_CALL
