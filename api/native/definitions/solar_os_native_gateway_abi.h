#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_messaging_types_v1.h"

#define SOLAR_OS_NATIVE_GATEWAY_SERVICE "messages.gateway"
#define SOLAR_OS_NATIVE_GATEWAY_ABI 1U

#define SOLAR_OS_CHAT_URL_MAX 160
#define SOLAR_OS_CHAT_TOKEN_MAX 96
#define SOLAR_OS_CHAT_USER_MAX 64
#define SOLAR_OS_CHAT_DEVICE_MAX 64
#define SOLAR_OS_CHAT_CHANNEL_MAX 64
#define SOLAR_OS_CHAT_TEXT_MAX 4096
#define SOLAR_OS_CHAT_ERROR_MAX 96
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

typedef struct {
    uint32_t id;
    solar_os_chat_event_type_t type;
    uint64_t message_key;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
    uint64_t timestamp;
    uint32_t command_id;
    int code;
    bool truncated;
} solar_os_chat_event_t;

typedef struct {
    char name[SOLAR_OS_CHAT_CHANNEL_MAX];
    bool desired;
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
    int32_t last_esp_error;
    uint32_t config_revision;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    size_t queued_events;
    size_t queued_outbox;
    size_t stored_messages;
    size_t unread_messages;
    size_t persistent_capacity;
    size_t persistent_limit_bytes;
    bool persistent;
    bool persistent_inbox_backed;
    int32_t storage_error;
} solar_os_chat_status_t;

/* Use a Gateway provider ID from messages, or zero for the default.
 * Configuration and connection state are updated together. NULL settings
 * preserve the saved value. An empty token clears it.
 * Channel snapshots return the number copied. Events use a caller-owned
 * cursor initialized to zero and a bounded wait in milliseconds. */
typedef enum {
    SOLAR_OS_GATEWAY_KEEP_ENABLED = 0,
    SOLAR_OS_GATEWAY_ENABLE,
    SOLAR_OS_GATEWAY_DISABLE,
} solar_os_gateway_connection_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    int (*status)(solar_os_messaging_provider_id_t provider, solar_os_chat_status_t *status);
    int (*configure)(solar_os_messaging_provider_id_t provider, const char *url,
        const char *token, solar_os_gateway_connection_v1_t connection);
    size_t (*channels)(solar_os_messaging_provider_id_t provider, solar_os_chat_channel_t *out, size_t capacity);
    int (*delete_channel)(solar_os_messaging_provider_id_t provider, const char *name);
    int (*event_next)(solar_os_messaging_provider_id_t provider, uint32_t *cursor,
        solar_os_chat_event_t *event, uint32_t timeout_ms);
    const char *(*state_name)(solar_os_chat_state_t state);
    const char *(*event_type_name)(solar_os_chat_event_type_t type);
} solar_os_native_gateway_api_v1_t;

#define SOLAR_OS_NATIVE_GATEWAY_SYNC_SERVICE "messages.gateway_sync"
#define SOLAR_OS_NATIVE_GATEWAY_SYNC_ABI 1U

/* One Core supervisor runs the configured Gateway instances. Start refuses
 * a running or stopping supervisor. Stop waits for the children and can be
 * retried after a timeout. Running remains true until cleanup finishes. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*start)(void);
    int (*stop)(void);
    bool (*running)(void);
} solar_os_native_gateway_sync_api_v1_t;
