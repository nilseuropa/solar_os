#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_chat_protocol.h"
#include "solar_os_messaging.h"

#define SOLAR_OS_CHAT_STORE_CAPACITY 80
#define SOLAR_OS_CHAT_MESSAGE_CAPACITY SOLAR_OS_MESSAGING_MESSAGE_CAPACITY
#define SOLAR_OS_CHAT_OUTBOX_CAPACITY 16

typedef enum {
    SOLAR_OS_CHAT_COMMAND_JOIN,
    SOLAR_OS_CHAT_COMMAND_LEAVE,
    SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL,
    SOLAR_OS_CHAT_COMMAND_MESSAGE,
} solar_os_chat_command_type_t;

typedef struct {
    uint32_t id;
    uint32_t inbox_id;
    uint64_t message_key;
    uint64_t timestamp;
    bool unread;
    bool truncated;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
} solar_os_chat_message_t;

typedef bool (*solar_os_chat_message_visitor_t)(
    const solar_os_chat_message_t *message,
    void *user);

typedef struct {
    uint32_t id;
    solar_os_chat_command_type_t type;
    uint64_t cursor;
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

esp_err_t solar_os_chat_init(void);
esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token);
/* Connect/disconnect set desired state; gateway-sync owns transport I/O. */
esp_err_t solar_os_chat_connect(const char *url,
                                const char *token);
esp_err_t solar_os_chat_disconnect(void);
esp_err_t solar_os_chat_join(const char *channel);
esp_err_t solar_os_chat_leave(const char *channel);
esp_err_t solar_os_chat_delete_channel(const char *channel);
esp_err_t solar_os_chat_send(const char *channel, const char *text);
esp_err_t solar_os_chat_read_event(solar_os_chat_event_t *event, uint32_t timeout_ms);
esp_err_t solar_os_chat_read_event_after(uint32_t *cursor,
                                         solar_os_chat_event_t *event);
size_t solar_os_chat_message_visit(solar_os_chat_message_visitor_t visitor,
                                   void *user,
                                   uint32_t *event_cursor);
esp_err_t solar_os_chat_mark_message_read(uint64_t message_key, bool read);
esp_err_t solar_os_chat_mark_channel_read(const char *channel);
esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status);
esp_err_t solar_os_chat_get_config(solar_os_chat_config_t *config);
size_t solar_os_chat_channel_snapshot(solar_os_chat_channel_t *channels,
                                      size_t max_channels);
uint64_t solar_os_chat_channel_cursor(const char *channel);

/* Synchronizer-facing store/outbox API. */
esp_err_t solar_os_gateway_sync_set_status(bool running,
                                        bool connected,
                                        esp_err_t error,
                                        const char *message);
esp_err_t solar_os_gateway_sync_publish(const solar_os_chat_event_t *event,
                                     bool *inserted,
                                     uint64_t *message_key);
uint64_t solar_os_chat_context_id(void);
esp_err_t solar_os_chat_outbox_peek(solar_os_chat_command_t *command);
esp_err_t solar_os_chat_outbox_ack(uint32_t id);

const char *solar_os_chat_state_name(solar_os_chat_state_t state);
const char *solar_os_chat_event_type_name(solar_os_chat_event_type_t type);
