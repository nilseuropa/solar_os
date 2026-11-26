#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_REMOTE_FILE_NAME_MAX 96U
#define SOLAR_OS_REMOTE_FILE_ERROR_MAX 192U
#define SOLAR_OS_REMOTE_FILE_TIMEOUT_MS 10000U

typedef enum {
    SOLAR_OS_REMOTE_FILE_FTP,
    SOLAR_OS_REMOTE_FILE_SFTP,
} solar_os_remote_file_protocol_t;

typedef struct {
    char name[SOLAR_OS_REMOTE_FILE_NAME_MAX];
    uint64_t size;
    bool is_directory;
} solar_os_remote_file_entry_t;

typedef bool (*solar_os_remote_file_entry_fn)(const solar_os_remote_file_entry_t *entry, void *user);
typedef bool (*solar_os_remote_file_cancel_fn)(void *user);
typedef void (*solar_os_remote_file_progress_fn)(uint64_t bytes, void *user);

typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    uint32_t timeout_ms;
    char *error;
    size_t error_len;
} solar_os_remote_file_options_t;

/* Caller owns this synchronous connection context. Keep the cancellation
 * callback and its user data valid until disconnect. */
typedef struct {
    solar_os_remote_file_protocol_t protocol;
    void *connection;
} solar_os_remote_files_t;

esp_err_t solar_os_remote_files_connect(solar_os_remote_files_t *session,
    solar_os_remote_file_protocol_t protocol, const solar_os_remote_file_options_t *options,
    solar_os_remote_file_cancel_fn should_cancel, void *cancel_user);
void solar_os_remote_files_disconnect(solar_os_remote_files_t *session);
const char *solar_os_remote_files_error(const solar_os_remote_files_t *session);
esp_err_t solar_os_remote_files_list(solar_os_remote_files_t *session, const char *path,
    solar_os_remote_file_entry_fn on_entry, void *user);
esp_err_t solar_os_remote_files_download(solar_os_remote_files_t *session, const char *remote,
    const char *local, solar_os_remote_file_progress_fn progress, void *user);
esp_err_t solar_os_remote_files_upload(solar_os_remote_files_t *session, const char *local,
    const char *remote, solar_os_remote_file_progress_fn progress, void *user);
esp_err_t solar_os_remote_files_mkdir(solar_os_remote_files_t *session, const char *path);
esp_err_t solar_os_remote_files_rmdir(solar_os_remote_files_t *session, const char *path);
esp_err_t solar_os_remote_files_remove(solar_os_remote_files_t *session, const char *path);
esp_err_t solar_os_remote_files_rename(solar_os_remote_files_t *session, const char *from, const char *to);
