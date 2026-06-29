#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_CHAT_URL_MAX 160
#define SOLAR_OS_CHAT_TOKEN_MAX 96
#define SOLAR_OS_CHAT_USER_MAX 64
#define SOLAR_OS_CHAT_DEVICE_MAX 64
#define SOLAR_OS_CHAT_CHANNEL_MAX 64
#define SOLAR_OS_CHAT_TEXT_MAX 4096
#define SOLAR_OS_CHAT_ERROR_MAX 96

typedef enum {
    SOLAR_OS_CHAT_STATE_DISCONNECTED,
    SOLAR_OS_CHAT_STATE_CONNECTING,
    SOLAR_OS_CHAT_STATE_CONNECTED,
} solar_os_chat_state_t;

typedef enum {
    SOLAR_OS_CHAT_EVENT_CONNECTED,
    SOLAR_OS_CHAT_EVENT_DISCONNECTED,
    SOLAR_OS_CHAT_EVENT_ERROR,
    SOLAR_OS_CHAT_EVENT_CHANNEL,
    SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED,
    SOLAR_OS_CHAT_EVENT_JOINED,
    SOLAR_OS_CHAT_EVENT_LEFT,
    SOLAR_OS_CHAT_EVENT_MESSAGE,
    SOLAR_OS_CHAT_EVENT_PRESENCE,
    SOLAR_OS_CHAT_EVENT_RAW,
} solar_os_chat_event_type_t;

typedef struct {
    bool initialized;
    bool configured;
    bool running;
    bool connected;
    bool token_set;
    solar_os_chat_state_t state;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
    char last_error[SOLAR_OS_CHAT_ERROR_MAX];
    esp_err_t last_esp_error;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    size_t queued_events;
} solar_os_chat_status_t;

typedef struct {
    solar_os_chat_event_type_t type;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
    uint64_t timestamp;
    int code;
    bool truncated;
} solar_os_chat_event_t;

esp_err_t solar_os_chat_init(void);
esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token,
                                  const char *user,
                                  const char *device);
esp_err_t solar_os_chat_connect(const char *url,
                                const char *token,
                                const char *user,
                                const char *device);
esp_err_t solar_os_chat_disconnect(void);
esp_err_t solar_os_chat_join(const char *channel);
esp_err_t solar_os_chat_leave(const char *channel);
esp_err_t solar_os_chat_delete_channel(const char *channel);
esp_err_t solar_os_chat_send(const char *channel, const char *text);
esp_err_t solar_os_chat_read_event(solar_os_chat_event_t *event, uint32_t timeout_ms);
esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status);
const char *solar_os_chat_state_name(solar_os_chat_state_t state);
const char *solar_os_chat_event_type_name(solar_os_chat_event_type_t type);
