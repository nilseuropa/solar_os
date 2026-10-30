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
#define SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX 64U
#define SOLAR_OS_MESSAGING_SENDER_MAX 40U
#define SOLAR_OS_MESSAGING_STORE_DIR ".messages"
#define SOLAR_OS_MESSAGING_STORE_FILE "history.bin"

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
    char class_name[16];
    uint32_t capabilities;
    bool connection_oriented;
    bool removed;
} solar_os_messaging_provider_status_t;

typedef enum {
    SOLAR_OS_MESSAGING_JOIN_NONE,
    SOLAR_OS_MESSAGING_JOIN_LEFT,
    SOLAR_OS_MESSAGING_JOIN_PENDING,
    SOLAR_OS_MESSAGING_JOIN_JOINED,
    SOLAR_OS_MESSAGING_JOIN_FAILED,
} solar_os_messaging_join_state_t;

#define SOLAR_OS_MESSAGING_SETTINGS_FIELDS 8U
#define SOLAR_OS_MESSAGING_SETTING_MAX 160U
typedef struct {
    const char *label;
    const char *hint;
    size_t capacity; /* Includes the terminating NUL. */
    bool secret;
    bool required;
} solar_os_messaging_setting_t;

/* Provider-owned form descriptions have static lifetime. Secret readback is
 * always empty; present indicates a saved value. An unchanged field preserves
 * its current value, including credentials. Changing a secret to empty clears it.
 * Allocate forms outside the task stack. */
typedef struct {
    const char *label;
    const solar_os_messaging_setting_t *fields;
    size_t count;
    char worker[48];
    bool worker_is_resource; /* Resolve a job by its named resource. */
    bool enabled;
    char values[SOLAR_OS_MESSAGING_SETTINGS_FIELDS][SOLAR_OS_MESSAGING_SETTING_MAX];
    bool present[SOLAR_OS_MESSAGING_SETTINGS_FIELDS];
    bool changed[SOLAR_OS_MESSAGING_SETTINGS_FIELDS];
} solar_os_messaging_settings_t;

void solar_os_messaging_settings_value(solar_os_messaging_settings_t *settings,
    size_t field, const char *value);
void solar_os_messaging_settings_clear(solar_os_messaging_settings_t *settings);

/* Class descriptors have static lifetime. Operations lease instance contexts
 * until completion; successful removal destroys them after excluding workers.
 * Operations run outside the Messaging lock. */
typedef struct {
    const char *name;
    uint32_t capabilities;
    bool connection_oriented;
    esp_err_t (*join)(void *context, const char *target);
    /* Group backends can resolve/configure a subscription and return its
     * canonical conversation instead of allocating a room before the call. */
    esp_err_t (*join_open)(void *context, const char *target,
                           solar_os_conversation_id_t *conversation);
    esp_err_t (*leave)(void *context, const char *target);
    esp_err_t (*direct_open)(void *context, const char *target,
                             solar_os_conversation_id_t *conversation);
    esp_err_t (*connect)(void *context, bool enabled);
    esp_err_t (*create)(const char *name, int argc, const char *const *argv,
                        solar_os_messaging_provider_id_t *id);
    esp_err_t (*configure)(void *context, int argc, const char *const *argv);
    /* NULL context describes/creates a new instance. Form indices enumerate
     * available variants; ESP_ERR_NOT_FOUND ends enumeration. */
    esp_err_t (*settings_read)(void *context, unsigned form,
                               solar_os_messaging_settings_t *settings);
    esp_err_t (*settings_write)(void *context, unsigned form, const char *name,
                                const solar_os_messaging_settings_t *settings,
                                solar_os_messaging_provider_id_t *id);
    esp_err_t (*normalize_target)(void *context, const char *target,
                                  char *normalized, size_t size);
    /* Prepare excludes transport work until finish, including on failure.
     * No Messaging locks are held by either callback. Finish cannot fail;
     * when removed is true it destroys the context, otherwise it resumes it. */
    esp_err_t (*remove_prepare)(void *context);
    void (*remove_finish)(void *context, bool removed);
} solar_os_messaging_provider_class_t;

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
    bool history_only;
    solar_os_messaging_join_state_t join_state;
    char join_error[SOLAR_OS_MESSAGING_ERROR_MAX];
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
    size_t cached_messages;
    size_t unread;
    size_t queued_outbox;
    size_t queued_events;
    /* Zero denotes an archive limited by available disk space. */
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
/* Newest matching messages, returned in chronological order. Offset counts
 * messages from the newest end of the conversation; memory use is caller-bounded. */
esp_err_t solar_os_messaging_message_page(solar_os_conversation_id_t conversation,
    solar_os_messaging_provider_id_t provider, size_t offset,
    solar_os_messaging_message_t *messages, size_t capacity, size_t *count, bool *more);
/* Offset counts matching records, including when unread_only is selected. */
esp_err_t solar_os_messaging_message_page_filtered(solar_os_conversation_id_t conversation,
    solar_os_messaging_provider_id_t provider, bool unread_only, size_t offset,
    solar_os_messaging_message_t *messages, size_t capacity, size_t *count, bool *more);
esp_err_t solar_os_messaging_get_status(solar_os_messaging_status_t *status);
esp_err_t solar_os_messaging_provider_register(
    solar_os_messaging_provider_id_t provider,
    const char *name);
esp_err_t solar_os_messaging_class_register(
    const solar_os_messaging_provider_class_t *provider_class);
esp_err_t solar_os_messaging_instance_register(
    solar_os_messaging_provider_id_t id, const char *class_name,
    const char *name, void *context);
/* Retain an instance during setup of another resource that refers to it.
 * Removal refuses active leases. Release exactly once after successful retain. */
esp_err_t solar_os_messaging_instance_retain(solar_os_messaging_provider_id_t id, void **lease);
void solar_os_messaging_instance_release(void *lease);
/* Cursor is an instance ID, initially zero. No caller-sized provider array. */
bool solar_os_messaging_provider_next(solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status);
solar_os_messaging_provider_id_t solar_os_messaging_provider_find(const char *name);
bool solar_os_messaging_provider_is_class(solar_os_messaging_provider_id_t id,
                                         const char *class_name);
typedef struct {
    char name[16];
    uint32_t capabilities;
    bool creatable;
} solar_os_messaging_class_status_t;
/* Pass an empty string initially, then the previous status.name. */
bool solar_os_messaging_class_next(const char *after, solar_os_messaging_class_status_t *status);
esp_err_t solar_os_messaging_profile_save(solar_os_messaging_provider_id_t id,
                                         const void *data, size_t size);
esp_err_t solar_os_messaging_profile_load(solar_os_messaging_provider_id_t id,
                                         void *data, size_t size);
esp_err_t solar_os_messaging_instance_create(const char *class_name,
    const char *name, const void *config, size_t size,
    solar_os_messaging_provider_id_t *id);
/* Remove an instance, pending sends, messages, Inbox projections and credentials.
 * A small credential-free record reserves its identity and retries interrupted
 * history deletion after reboot. */
esp_err_t solar_os_messaging_instance_remove(solar_os_messaging_provider_id_t id);
esp_err_t solar_os_messaging_instance_add(const char *class_name, const char *name,
    int argc, const char *const *argv, solar_os_messaging_provider_id_t *id);
esp_err_t solar_os_messaging_instance_configure(solar_os_messaging_provider_id_t id,
    int argc, const char *const *argv);
/* id=0 selects a creation form for class_name; otherwise class_name must match
 * the leased instance. No Messaging lock is held during provider callbacks. */
esp_err_t solar_os_messaging_settings_read(const char *class_name,
    solar_os_messaging_provider_id_t id, unsigned form,
    solar_os_messaging_settings_t *settings);
esp_err_t solar_os_messaging_settings_write(const char *class_name,
    solar_os_messaging_provider_id_t id, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings,
    solar_os_messaging_provider_id_t *result);
esp_err_t solar_os_messaging_join(solar_os_messaging_provider_id_t instance,
    const char *target, solar_os_conversation_id_t *conversation);
esp_err_t solar_os_messaging_leave(solar_os_conversation_id_t conversation);
esp_err_t solar_os_messaging_open_direct(solar_os_messaging_provider_id_t instance,
    const char *target, solar_os_conversation_id_t *conversation);
esp_err_t solar_os_messaging_connect(solar_os_messaging_provider_id_t instance, bool enabled);
esp_err_t solar_os_messaging_join_update(solar_os_messaging_provider_id_t instance,
    const char *target, solar_os_messaging_join_state_t state, const char *error);
esp_err_t solar_os_messaging_provider_claim(
    solar_os_messaging_provider_id_t provider,
    const char *detail);
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
/* A provider refresh marks old groups as history; upsert reactivates its
 * configured groups. History and message IDs are preserved. */
esp_err_t solar_os_messaging_groups_begin_sync(
    solar_os_messaging_provider_id_t provider, const char *provider_key_prefix);
void solar_os_messaging_conversation_label(
    const solar_os_messaging_conversation_t *conversation,
    char *label, size_t capacity);
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
/* The same over messages whose provider key has (key & mask) == tag, with
 * the mask bits cleared (a provider that tags keys by source, e.g. relay). */
uint64_t solar_os_messaging_provider_cursor_masked(
    solar_os_messaging_provider_id_t provider,
    const char *provider_key,
    uint64_t mask,
    uint64_t tag);
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
