#pragma once

#include "solar_os_native_sftp_abi.h"

#define SOLAR_OS_NATIVE_REMOTE_FILES_SERVICE "remote.files"
#define SOLAR_OS_NATIVE_REMOTE_FILES_ABI 1U
#define SOLAR_OS_NATIVE_REMOTE_FILES_FTP 0U
#define SOLAR_OS_NATIVE_REMOTE_FILES_SFTP 1U

typedef solar_os_native_sftp_entry_v1_t solar_os_native_remote_file_entry_v1_t;
typedef solar_os_native_sftp_entry_fn solar_os_native_remote_file_entry_fn;

typedef struct {
    const char *host;
    const char *username;
    const char *password;
    uint32_t port, timeout_ms;
} solar_os_native_remote_file_options_v1_t;

/* Each context owns one connection and protocol. Connect consumes strings
 * during the call. The optional diagnostic buffer receives connection
 * errors when the protocol provides them. Operations are synchronous and
 * borrow visitor/progress callbacks for one call. Serialize all operations
 * other than cancel(). A close during an operation returns BUSY. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(uint32_t protocol, solar_os_native_handle_t *context);
    int (*close)(solar_os_native_handle_t context);
    int (*cancel)(solar_os_native_handle_t context, bool requested);
    int (*connect)(solar_os_native_handle_t context, const solar_os_native_remote_file_options_v1_t *options,
        char *diagnostic, size_t diagnostic_capacity);
    int (*disconnect)(solar_os_native_handle_t context);
    int (*last_error)(solar_os_native_handle_t context, char *text, size_t capacity);
    int (*list)(solar_os_native_handle_t context, const char *path,
        solar_os_native_remote_file_entry_fn visitor, void *user);
    int (*download)(solar_os_native_handle_t context, const char *remote, const char *local,
        solar_os_native_transfer_progress_fn progress, void *user);
    int (*upload)(solar_os_native_handle_t context, const char *local, const char *remote,
        solar_os_native_transfer_progress_fn progress, void *user);
    int (*mkdir)(solar_os_native_handle_t context, const char *path);
    int (*rmdir)(solar_os_native_handle_t context, const char *path);
    int (*remove)(solar_os_native_handle_t context, const char *path);
    int (*rename)(solar_os_native_handle_t context, const char *old_path, const char *new_path);
} solar_os_native_remote_files_api_v1_t;
