#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_chat_protocol.h"

#define SOLAR_OS_CHAT_ERROR_MAX 96
#define SOLAR_OS_CHAT_STORE_CAPACITY 80
#define SOLAR_OS_CHAT_OUTBOX_CAPACITY 16
#define SOLAR_OS_CHAT_CHANNEL_CAPACITY 32

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
    SOLAR_OS_CHAT_EVENT_COMMAND_SENT,
    SOLAR_OS_CHAT_EVENT_RAW,
} solar_os_chat_event_type_t;

typedef enum {
    SOLAR_OS_CHAT_COMMAND_JOIN,
    SOLAR_OS_CHAT_COMMAND_LEAVE,
    SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL,
    SOLAR_OS_CHAT_COMMAND_MESSAGE,
} solar_os_chat_command_type_t;

typedef struct {
    uint32_t id;
    solar_os_chat_event_type_t type;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
    uint64_t timestamp;
    uint32_t command_id;
    int code;
    bool truncated;
} solar_os_chat_event_t;

typedef struct {
    uint32_t id;
    solar_os_chat_command_type_t type;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
} solar_os_chat_command_t;

typedef struct {
    bool configured;
    bool enabled;
    uint32_t revision;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
} solar_os_chat_config_t;

typedef struct {
    char name[SOLAR_OS_CHAT_CHANNEL_MAX];
    bool joined;
} solar_os_chat_channel_t;

typedef struct {
    bool initialized;
    bool configured;
    bool enabled;
    bool running;
    bool connected;
    bool token_set;
    solar_os_chat_state_t state;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
    char last_error[SOLAR_OS_CHAT_ERROR_MAX];
    esp_err_t last_esp_error;
    uint32_t config_revision;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    size_t queued_events;
    size_t queued_outbox;
} solar_os_chat_status_t;

esp_err_t solar_os_chat_init(void);
esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token,
                                  const char *user,
                                  const char *device);
/* Connect/disconnect set desired synchronization state; the sync job owns I/O. */
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
esp_err_t solar_os_chat_read_event_after(uint32_t *cursor,
                                         solar_os_chat_event_t *event);
esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status);
esp_err_t solar_os_chat_get_config(solar_os_chat_config_t *config);
size_t solar_os_chat_channel_snapshot(solar_os_chat_channel_t *channels,
                                      size_t max_channels);

/* Synchronizer-facing store/outbox API. */
esp_err_t solar_os_chat_sync_set_status(bool running,
                                        bool connected,
                                        esp_err_t error,
                                        const char *message);
esp_err_t solar_os_chat_sync_publish(const solar_os_chat_event_t *event);
esp_err_t solar_os_chat_outbox_peek(solar_os_chat_command_t *command);
esp_err_t solar_os_chat_outbox_ack(uint32_t id);

const char *solar_os_chat_state_name(solar_os_chat_state_t state);
const char *solar_os_chat_event_type_name(solar_os_chat_event_type_t type);
