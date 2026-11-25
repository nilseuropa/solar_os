#pragma once
#include "solar_os_native_ssh_abi.h"
#define SOLAR_OS_NATIVE_SFTP_SERVICE "sftp"
#define SOLAR_OS_NATIVE_SFTP_ABI 1U
typedef struct { char name[96]; uint64_t size; uint32_t is_directory; } solar_os_native_sftp_entry_v1_t;
typedef bool (*solar_os_native_sftp_entry_fn)(const solar_os_native_sftp_entry_v1_t *entry, void *user);
typedef void (*solar_os_native_transfer_progress_fn)(uint64_t bytes, void *user);
/* Create before connecting. Every operation is synchronous, and list/progress
 * callbacks run only on the caller's task until return. The OS owns connection
 * state and copied credentials. Serialize operations except cancel(), which
 * is cross-task safe. Reset cancellation only after the interrupted call exits.
 * Connect, file transfers and RSA operations need an OS SSH/TLS worker stack.
 * close() refuses while an operation is active. Failed connect retains the
 * context for reconnect or close. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(solar_os_native_handle_t *context);
    int (*close)(solar_os_native_handle_t context);
    int (*cancel)(solar_os_native_handle_t context, bool requested);
    int (*connect)(solar_os_native_handle_t context, const solar_os_native_ssh_options_v1_t *options, uint32_t timeout_ms);
    int (*disconnect)(solar_os_native_handle_t context);
    int (*last_error)(solar_os_native_handle_t context, char *text, size_t capacity);
    int (*list)(solar_os_native_handle_t context, const char *path, solar_os_native_sftp_entry_fn visitor, void *user);
    int (*download)(solar_os_native_handle_t context, const char *remote, const char *local, solar_os_native_transfer_progress_fn progress, void *user);
    int (*upload)(solar_os_native_handle_t context, const char *local, const char *remote, solar_os_native_transfer_progress_fn progress, void *user);
    int (*mkdir)(solar_os_native_handle_t context, const char *path);
    int (*rmdir)(solar_os_native_handle_t context, const char *path);
    int (*remove)(solar_os_native_handle_t context, const char *path);
    int (*rename)(solar_os_native_handle_t context, const char *old_path, const char *new_path);
} solar_os_native_sftp_api_v1_t;
