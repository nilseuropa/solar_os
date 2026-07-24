#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_SSH_HOST_MAX 96
#define SOLAR_OS_SSH_USERNAME_MAX 64
#define SOLAR_OS_SSH_PASSWORD_MAX 96
#define SOLAR_OS_SSH_EVENT_DATA_MAX 256
#define SOLAR_OS_SSH_EVENT_MESSAGE_MAX 128
#define SOLAR_OS_SSH_TASK_STACK 12288U

typedef struct solar_os_ssh_session solar_os_ssh_session_t;

typedef enum {
    SOLAR_OS_SSH_EVENT_STATUS,
    SOLAR_OS_SSH_EVENT_CONNECTED,
    SOLAR_OS_SSH_EVENT_OUTPUT,
    SOLAR_OS_SSH_EVENT_ERROR,
    SOLAR_OS_SSH_EVENT_DISCONNECTED,
} solar_os_ssh_event_type_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    uint16_t cols;
    uint16_t rows;
} solar_os_ssh_config_t;

typedef struct {
    solar_os_ssh_event_type_t type;
    size_t len;
    char data[SOLAR_OS_SSH_EVENT_DATA_MAX];
    char message[SOLAR_OS_SSH_EVENT_MESSAGE_MAX];
} solar_os_ssh_event_t;

esp_err_t solar_os_ssh_start(const solar_os_ssh_config_t *config,
                             solar_os_ssh_session_t **session);
bool solar_os_ssh_stop(solar_os_ssh_session_t *session);
esp_err_t solar_os_ssh_send(solar_os_ssh_session_t *session, const char *data, size_t len);
bool solar_os_ssh_poll(solar_os_ssh_session_t *session, solar_os_ssh_event_t *event);
