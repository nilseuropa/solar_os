#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_ssh.h"
#include "solar_os_storage.h"

#define SOLAR_OS_SCP_EVENT_MESSAGE_MAX 128
#define SOLAR_OS_SCP_TASK_STACK 24576U

typedef struct solar_os_scp_session solar_os_scp_session_t;

typedef enum {
    SOLAR_OS_SCP_UPLOAD,
    SOLAR_OS_SCP_DOWNLOAD,
} solar_os_scp_direction_t;

typedef enum {
    SOLAR_OS_SCP_EVENT_STATUS,
    SOLAR_OS_SCP_EVENT_PROGRESS,
    SOLAR_OS_SCP_EVENT_ERROR,
    SOLAR_OS_SCP_EVENT_DONE,
} solar_os_scp_event_type_t;

typedef struct {
    solar_os_scp_direction_t direction;
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    const char *local_path;
    const char *remote_path;
    bool remote_glob;
} solar_os_scp_config_t;

typedef struct {
    solar_os_scp_event_type_t type;
    uint64_t transferred;
    uint64_t total;
    char message[SOLAR_OS_SCP_EVENT_MESSAGE_MAX];
} solar_os_scp_event_t;

esp_err_t solar_os_scp_start(const solar_os_scp_config_t *config,
                             solar_os_scp_session_t **session);
bool solar_os_scp_stop(solar_os_scp_session_t *session);
bool solar_os_scp_poll(solar_os_scp_session_t *session, solar_os_scp_event_t *event);
