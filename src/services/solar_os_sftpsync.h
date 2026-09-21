#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_ssh.h"
#include "solar_os_storage.h"

#define SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX 160
#define SOLAR_OS_SFTPSYNC_TASK_STACK 24576U

typedef struct solar_os_sftpsync_session solar_os_sftpsync_session_t;

typedef enum {
    SOLAR_OS_SFTPSYNC_UPLOAD,
    SOLAR_OS_SFTPSYNC_DOWNLOAD,
} solar_os_sftpsync_direction_t;

typedef enum {
    SOLAR_OS_SFTPSYNC_EVENT_STATUS,
    SOLAR_OS_SFTPSYNC_EVENT_PROGRESS,
    SOLAR_OS_SFTPSYNC_EVENT_ERROR,
    SOLAR_OS_SFTPSYNC_EVENT_DONE,
} solar_os_sftpsync_event_type_t;

typedef struct {
    solar_os_sftpsync_direction_t direction;
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    const char *local_path;
    const char *remote_path;
    bool recursive;
    bool dry_run;
} solar_os_sftpsync_config_t;

typedef struct {
    solar_os_sftpsync_event_type_t type;
    uint64_t transferred;
    size_t files_changed;
    uint64_t file_transferred;
    uint64_t file_size;
    bool file_size_known;
    char message[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
} solar_os_sftpsync_event_t;

esp_err_t solar_os_sftpsync_start(const solar_os_sftpsync_config_t *config,
                               solar_os_sftpsync_session_t **session);
bool solar_os_sftpsync_stop(solar_os_sftpsync_session_t *session);
bool solar_os_sftpsync_poll(solar_os_sftpsync_session_t *session,
                         solar_os_sftpsync_event_t *event);
