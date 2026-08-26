#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_messaging_types.h"

#define SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY 32U
#define SOLAR_OS_MESSAGING_MESSAGE_CAPACITY 64U
#define SOLAR_OS_MESSAGING_OUTBOX_CAPACITY 16U
#define SOLAR_OS_MESSAGING_EVENT_CAPACITY 80U
#define SOLAR_OS_MESSAGING_PROVIDER_CAPACITY 3U
#define SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX 64U
#define SOLAR_OS_MESSAGING_SENDER_MAX 40U
#define SOLAR_OS_MESSAGING_STORE_DIR ".messages"
#define SOLAR_OS_MESSAGING_STORE_FILE "messages.bin"

typedef enum {
    SOLAR_OS_MESSAGING_EVENT_PROVIDER = 0,
    SOLAR_OS_MESSAGING_EVENT_CONVERSATION,
    SOLAR_OS_MESSAGING_EVENT_CONVERSATION_REMOVED,
    SOLAR_OS_MESSAGING_EVENT_MESSAGE,
    SOLAR_OS_MESSAGING_EVENT_MESSAGE_REMOVED,
    SOLAR_OS_MESSAGING_EVENT_MESSAGES_CLEARED,
    SOLAR_OS_MESSAGING_EVENT_DELIVERY,
    SOLAR_OS_MESSAGING_EVENT_ERROR,
} solar_os_messaging_event_type_t;

typedef struct {
    solar_os_messaging_provider_id_t id;
    bool registered;
    bool running;
    bool connected;
    esp_err_t last_error;
    char name[SOLAR_OS_MESSAGING_LABEL_MAX];
    char detail[SOLAR_OS_MESSAGING_ERROR_MAX];
} solar_os_messaging_provider_status_t;

typedef struct {
    solar_os_conversation_id_t id;
    solar_os_messaging_provider_id_t provider;
    solar_os_conversation_kind_t kind;
    solar_os_contact_id_t contact_id;
    solar_os_endpoint_id_t endpoint_id;
    uint32_t group_ref;
    uint32_t unread_count;
    uint64_t last_message_ms;
    uint32_t security_flags;
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
} solar_os_messaging_conversation_t;

typedef struct {
    solar_os_message_key_t key;
    uint64_t provider_message_key;
    solar_os_conversation_id_t conversation_id;
    solar_os_contact_id_t contact_id;
    solar_os_endpoint_id_t endpoint_id;
    uint64_t timestamp_ms;
    uint64_t received_ms;
    solar_os_messaging_provider_id_t provider;
    solar_os_message_direction_t direction;
    solar_os_delivery_state_t delivery;
    uint32_t security_flags;
    uint32_t inbox_id;
    bool unread;
    bool truncated;
    char sender[SOLAR_OS_MESSAGING_SENDER_MAX];
    char body[SOLAR_OS_MESSAGING_BODY_MAX];
    char error[SOLAR_OS_MESSAGING_ERROR_MAX];
} solar_os_messaging_message_t;

typedef struct {
    solar_os_messaging_provider_id_t provider;
    const char *provider_key;
    solar_os_conversation_kind_t kind;
    const char *title;
    solar_os_contact_id_t contact_id;
    solar_os_endpoint_id_t endpoint_id;
    uint32_t group_ref;
    uint32_t security_flags;
} solar_os_messaging_conversation_upsert_t;

typedef struct {
    solar_os_messaging_provider_id_t provider;
    const char *conversation_key;
    solar_os_conversation_kind_t conversation_kind;
    const char *conversation_title;
    solar_os_contact_id_t contact_id;
    solar_os_endpoint_id_t endpoint_id;
    uint32_t group_ref;
    uint64_t provider_message_key;
    uint64_t timestamp_ms;
    uint32_t security_flags;
    const char *sender;
    const char *body;
    bool truncated;
} solar_os_messaging_inbound_t;

typedef struct {
    uint32_t id;
    solar_os_message_key_t message_key;
    solar_os_conversation_id_t conversation_id;
    solar_os_messaging_provider_id_t provider;
    uint8_t attempts;
    bool allow_untrusted;
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    char body[SOLAR_OS_MESSAGING_BODY_MAX];
} solar_os_messaging_outbound_t;

typedef struct {
    uint32_t id;
    solar_os_messaging_event_type_t type;
    solar_os_messaging_provider_id_t provider;
    solar_os_conversation_id_t conversation_id;
    solar_os_message_key_t message_key;
    solar_os_delivery_state_t delivery;
    esp_err_t error;
} solar_os_messaging_event_t;

typedef struct {
    bool initialized;
    bool rings_in_psram;
    bool persistent;
    bool inbox_backed;
    size_t conversations;
    size_t messages;
    size_t unread;
    size_t queued_outbox;
    size_t queued_events;
    size_t persistent_capacity;
    size_t persistent_limit_bytes;
    uint32_t dropped_messages;
    uint32_t dropped_outbox;
    uint32_t generation;
    esp_err_t storage_error;
} solar_os_messaging_status_t;

typedef bool (*solar_os_messaging_message_visitor_t)(
    const solar_os_messaging_message_t *message,
    void *user);

esp_err_t solar_os_messaging_init(void);
esp_err_t solar_os_messaging_get_status(solar_os_messaging_status_t *status);
esp_err_t solar_os_messaging_provider_register(
    solar_os_messaging_provider_id_t provider,
    const char *name);
esp_err_t solar_os_messaging_provider_set_status(
    solar_os_messaging_provider_id_t provider,
    bool running,
    bool connected,
    esp_err_t error,
    const char *detail);
esp_err_t solar_os_messaging_provider_get_status(
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_provider_status_t *status);
esp_err_t solar_os_messaging_conversation_upsert(
    const solar_os_messaging_conversation_upsert_t *request,
    solar_os_conversation_id_t *conversation_id);
esp_err_t solar_os_messaging_conversation_remove(
    solar_os_messaging_provider_id_t provider,
    const char *provider_key);
esp_err_t solar_os_messaging_conversation_get(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_conversation_t *conversation);
esp_err_t solar_os_messaging_direct_open(
    solar_os_contact_id_t contact_id,
    solar_os_conversation_id_t *conversation_id);
size_t solar_os_messaging_conversation_snapshot(
    solar_os_messaging_conversation_t *conversations,
    size_t max_conversations);
esp_err_t solar_os_messaging_publish_inbound(
    const solar_os_messaging_inbound_t *request,
    bool *inserted,
    solar_os_message_key_t *message_key);
esp_err_t solar_os_messaging_send(solar_os_conversation_id_t conversation_id,
                                  const char *body,
                                  bool allow_untrusted,
                                  solar_os_message_key_t *message_key);
size_t solar_os_messaging_message_visit(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_message_visitor_t visitor,
    void *user,
    uint32_t *event_cursor);
uint64_t solar_os_messaging_provider_cursor(
    solar_os_messaging_provider_id_t provider,
    const char *provider_key);
size_t solar_os_messaging_message_visit_consistent(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_message_visitor_t visitor,
    void *user,
    uint32_t *event_cursor,
    uint32_t *generation);
esp_err_t solar_os_messaging_message_get(solar_os_message_key_t message_key,
                                         solar_os_messaging_message_t *message);
esp_err_t solar_os_messaging_message_delete(
    solar_os_message_key_t message_key);
esp_err_t solar_os_messaging_clear(
    solar_os_messaging_provider_id_t provider,
    size_t *removed);
esp_err_t solar_os_messaging_mark_read(
    solar_os_conversation_id_t conversation_id);
esp_err_t solar_os_messaging_cancel(solar_os_message_key_t message_key);
size_t solar_os_messaging_outbox_snapshot(
    solar_os_messaging_outbound_t *requests,
    size_t max_requests);
esp_err_t solar_os_messaging_outbox_peek(
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_outbound_t *request);
esp_err_t solar_os_messaging_outbox_update(
    uint32_t request_id,
    solar_os_delivery_state_t state,
    const char *error);
esp_err_t solar_os_messaging_read_event_after(uint32_t *cursor,
                                              solar_os_messaging_event_t *event);
uint64_t solar_os_messaging_provider_context(
    solar_os_messaging_provider_id_t provider);
