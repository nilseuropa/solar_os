#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_storage.h"

#define SOLAR_OS_FTP_DEFAULT_PORT 21U
#define SOLAR_OS_FTP_NAME_MAX 96U
#define SOLAR_OS_FTP_REPLY_MAX 192U
#define SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS 10000U

typedef struct solar_os_ftp_session solar_os_ftp_session_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    uint32_t timeout_ms;
} solar_os_ftp_options_t;

typedef struct {
    char name[SOLAR_OS_FTP_NAME_MAX];
    uint64_t size;
    bool is_directory;
} solar_os_ftp_entry_t;

typedef bool (*solar_os_ftp_entry_fn_t)(const solar_os_ftp_entry_t *entry,
                                        void *user);
typedef bool (*solar_os_ftp_cancel_fn_t)(void *user);
typedef void (*solar_os_ftp_progress_fn_t)(uint64_t bytes, void *user);

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
