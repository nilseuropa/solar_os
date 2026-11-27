#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"
#include "solar_os_remote_files.h"

#define SOLAR_OS_FTP_DEFAULT_PORT 21U
#define SOLAR_OS_FTP_NAME_MAX SOLAR_OS_REMOTE_FILE_NAME_MAX
#define SOLAR_OS_FTP_REPLY_MAX SOLAR_OS_REMOTE_FILE_ERROR_MAX
#define SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS SOLAR_OS_REMOTE_FILE_TIMEOUT_MS

typedef struct solar_os_ftp_session solar_os_ftp_session_t;

typedef solar_os_remote_file_options_t solar_os_ftp_options_t;

typedef solar_os_remote_file_entry_t solar_os_ftp_entry_t;
typedef solar_os_remote_file_entry_fn solar_os_ftp_entry_fn_t;
typedef solar_os_remote_file_cancel_fn solar_os_ftp_cancel_fn_t;
typedef solar_os_remote_file_progress_fn solar_os_ftp_progress_fn_t;

esp_err_t solar_os_ftp_connect(const solar_os_ftp_options_t *options,
                               solar_os_ftp_cancel_fn_t should_cancel,
                               void *cancel_user,
                               solar_os_ftp_session_t **session_out);
void solar_os_ftp_disconnect(solar_os_ftp_session_t *session);
const char *solar_os_ftp_last_reply(const solar_os_ftp_session_t *session);

esp_err_t solar_os_ftp_list(solar_os_ftp_session_t *session,
                            const char *path,
                            solar_os_ftp_entry_fn_t on_entry,
                            void *entry_user);
esp_err_t solar_os_ftp_download(solar_os_ftp_session_t *session,
                                const char *remote_path,
                                const char *local_path,
                                solar_os_ftp_progress_fn_t progress,
                                void *progress_user);
esp_err_t solar_os_ftp_upload(solar_os_ftp_session_t *session,
                              const char *local_path,
                              const char *remote_path,
                              solar_os_ftp_progress_fn_t progress,
                              void *progress_user);
esp_err_t solar_os_ftp_mkdir(solar_os_ftp_session_t *session,
                             const char *path);
esp_err_t solar_os_ftp_rmdir(solar_os_ftp_session_t *session,
                             const char *path);
esp_err_t solar_os_ftp_remove(solar_os_ftp_session_t *session,
                              const char *path);
esp_err_t solar_os_ftp_rename(solar_os_ftp_session_t *session,
                              const char *old_path,
                              const char *new_path);
