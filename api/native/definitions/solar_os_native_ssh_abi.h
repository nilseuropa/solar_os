#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_SSH_SERVICE "ssh"
#define SOLAR_OS_NATIVE_SSH_ABI 1U
#define SOLAR_OS_NATIVE_SSH_STATUS 0U
#define SOLAR_OS_NATIVE_SSH_CONNECTED 1U
#define SOLAR_OS_NATIVE_SSH_OUTPUT 2U
#define SOLAR_OS_NATIVE_SSH_ERROR 3U
#define SOLAR_OS_NATIVE_SSH_DISCONNECTED 4U
#define SOLAR_OS_NATIVE_TRANSFER_UPLOAD 0U
#define SOLAR_OS_NATIVE_TRANSFER_DOWNLOAD 1U
#define SOLAR_OS_NATIVE_TRANSFER_STATUS 0U
#define SOLAR_OS_NATIVE_TRANSFER_PROGRESS 1U
#define SOLAR_OS_NATIVE_TRANSFER_ERROR 2U
#define SOLAR_OS_NATIVE_TRANSFER_DONE 3U
#define SOLAR_OS_NATIVE_TRANSFER_GLOB 1U
#define SOLAR_OS_NATIVE_TRANSFER_RECURSIVE 2U
#define SOLAR_OS_NATIVE_TRANSFER_DRY_RUN 4U
typedef struct {
    const char *host, *username, *password;
    uint32_t port, columns, rows;
} solar_os_native_ssh_options_v1_t;
typedef struct { uint32_t type, data_size; char data[256], message[128]; } solar_os_native_ssh_event_v1_t;
typedef struct {
    solar_os_native_ssh_options_v1_t ssh;
    const char *local_path, *remote_path;
    uint32_t direction, flags;
} solar_os_native_transfer_options_v1_t;
typedef struct {
    uint64_t transferred, total, files_changed, file_transferred, file_size;
    uint32_t type, file_size_known;
    char message[160];
} solar_os_native_transfer_event_v1_t;
typedef struct {
    uint32_t private_exists, public_exists, private_size, public_size;
    char private_path[160], public_path[160];
} solar_os_native_ssh_keys_v1_t;
typedef bool (*solar_os_native_ssh_host_visit_fn)(const char *address, const char *alias, void *user);
/* Starts copy options before returning. Poll copies a queued event, with
 * available=false when the queue is empty. Close cancels and joins; timeout
 * leaves the handle valid for retry. Serialize operations on a handle.
 * SCP and sync methods are optional independently of interactive SSH.
 * Key generation needs a caller worker stack suitable for the OS RSA service.
 * Host visitors are synchronous and may not retain the borrowed strings. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*start)(const solar_os_native_ssh_options_v1_t *options, solar_os_native_handle_t *session);
    int (*close)(solar_os_native_handle_t session, uint32_t timeout_ms);
    int (*send)(solar_os_native_handle_t session, const void *bytes, size_t size);
    int (*poll)(solar_os_native_handle_t session, solar_os_native_ssh_event_v1_t *event, bool *available);
    int (*hosts)(solar_os_native_ssh_host_visit_fn visitor, void *user);
    int (*key_paths)(char *private_path, size_t private_capacity, char *public_path, size_t public_capacity);
    bool (*key_exists)(void);
    int (*key_status)(solar_os_native_ssh_keys_v1_t *status);
    int (*key_read_public)(char *text, size_t capacity, size_t *size);
    int (*key_generate)(uint32_t bits, bool overwrite);
    int (*key_remove)(void);
    int (*scp_start)(const solar_os_native_transfer_options_v1_t *options, solar_os_native_handle_t *session);
    int (*scp_close)(solar_os_native_handle_t session, uint32_t timeout_ms);
    int (*scp_poll)(solar_os_native_handle_t session, solar_os_native_transfer_event_v1_t *event, bool *available);
    int (*sync_start)(const solar_os_native_transfer_options_v1_t *options, solar_os_native_handle_t *session);
    int (*sync_close)(solar_os_native_handle_t session, uint32_t timeout_ms);
    int (*sync_poll)(solar_os_native_handle_t session, solar_os_native_transfer_event_v1_t *event, bool *available);
} solar_os_native_ssh_api_v1_t;
