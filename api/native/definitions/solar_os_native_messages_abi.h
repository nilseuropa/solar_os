#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_messaging_types_v1.h"

#define SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX 64U
#define SOLAR_OS_MESSAGING_SENDER_MAX 40U

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
    int32_t last_error;
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
    bool left; /* Explicitly left: hidden in Chat, history retained until rejoin. */
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
    int32_t error;
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
    int32_t storage_error;
} solar_os_messaging_status_t;

typedef struct {
    char name[16];
    uint32_t capabilities;
    bool creatable;
} solar_os_messaging_class_status_t;

typedef bool (*solar_os_messaging_message_visitor_t)(
    const solar_os_messaging_message_t *message,
    void *user);

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

#define SOLAR_OS_NATIVE_MESSAGES_SERVICE "messages"
#define SOLAR_OS_NATIVE_MESSAGES_ABI 1U
#define SOLAR_OS_MESSAGING_SETTINGS_FIELDS 8U
#define SOLAR_OS_MESSAGING_SETTING_MAX 160U

typedef struct {
    char label[64], hint[96];
    uint32_t capacity;
    bool secret, required;
} solar_os_message_setting_v1_t;

typedef struct {
    uint32_t count;
    char values[SOLAR_OS_MESSAGING_SETTINGS_FIELDS][SOLAR_OS_MESSAGING_SETTING_MAX];
    bool present[SOLAR_OS_MESSAGING_SETTINGS_FIELDS], changed[SOLAR_OS_MESSAGING_SETTINGS_FIELDS];
} solar_os_message_setting_values_v1_t;

typedef struct {
    char label[48], worker[48];
    bool enabled, worker_is_resource;
    solar_os_message_setting_v1_t fields[SOLAR_OS_MESSAGING_SETTINGS_FIELDS];
    solar_os_message_setting_values_v1_t data;
} solar_os_message_settings_v1_t;

/* Records returned in caller buffers own their values. Message pages return
 * the newest matching records in chronological order, with offset counted
 * from the newest match. A zero conversation/provider selects all. The
 * visitor borrows one record at a time and returns false to stop. It is called
 * on the requesting task outside the service lock and is never retained. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*status)(solar_os_messaging_status_t *status);
    bool (*provider_next)(solar_os_messaging_provider_id_t *cursor,
        solar_os_messaging_provider_status_t *status);
    int (*provider_info)(solar_os_messaging_provider_id_t provider,
        solar_os_messaging_provider_status_t *status);
    solar_os_messaging_provider_id_t (*provider_find)(const char *name);
    bool (*provider_is_class)(solar_os_messaging_provider_id_t provider, const char *class_name);
    bool (*class_next)(const char *after, solar_os_messaging_class_status_t *status);
    int (*provider_add)(const char *class_name, const char *name, int argc, const char *const *argv,
        solar_os_messaging_provider_id_t *provider);
    int (*provider_configure)(solar_os_messaging_provider_id_t provider, int argc, const char *const *argv);
    int (*provider_remove)(solar_os_messaging_provider_id_t provider);
    int (*connect)(solar_os_messaging_provider_id_t provider, bool enabled);
    int (*join)(solar_os_messaging_provider_id_t provider, const char *target,
        solar_os_conversation_id_t *conversation);
    int (*leave)(solar_os_conversation_id_t conversation);
    bool (*room_left)(solar_os_messaging_provider_id_t provider, const char *target);
    int (*open_direct)(solar_os_messaging_provider_id_t provider, const char *target,
        solar_os_conversation_id_t *conversation);
    int (*open_contact)(solar_os_contact_id_t contact, solar_os_conversation_id_t *conversation);
    size_t (*conversation_snapshot)(solar_os_messaging_conversation_t *entries, size_t capacity);
    int (*conversation_get)(solar_os_conversation_id_t conversation,
        solar_os_messaging_conversation_t *entry);
    void (*conversation_label)(const solar_os_messaging_conversation_t *entry, char *, size_t);
    int (*mark_read)(solar_os_conversation_id_t conversation);
    int (*message_page)(solar_os_conversation_id_t conversation, solar_os_messaging_provider_id_t provider,
        bool unread_only, size_t offset, solar_os_messaging_message_t *entries,
        size_t capacity, size_t *count, bool *more);
    size_t (*message_visit)(solar_os_conversation_id_t conversation, solar_os_messaging_provider_id_t provider,
        solar_os_messaging_message_visitor_t visitor, void *user, uint32_t *event_cursor, uint32_t *generation);
    int (*message_get)(solar_os_message_key_t key, solar_os_messaging_message_t *entry);
    int (*message_delete)(solar_os_message_key_t key);
    int (*clear)(solar_os_messaging_provider_id_t provider, size_t *removed);
    int (*send)(solar_os_conversation_id_t conversation, const char *body, bool allow_untrusted,
        solar_os_message_key_t *key);
    int (*cancel)(solar_os_message_key_t key);
    int (*event_next)(uint32_t *cursor, solar_os_messaging_event_t *event);
    size_t (*outbox_snapshot)(solar_os_messaging_outbound_t *entries, size_t capacity);
    const char *(*provider_name)(solar_os_messaging_provider_id_t provider);
    const char *(*contact_trust_name)(solar_os_contact_trust_t trust);
    const char *(*conversation_kind_name)(solar_os_conversation_kind_t kind);
    const char *(*delivery_name)(solar_os_delivery_state_t state);
    /* Select a provider instance, or set provider=0 to select a creation
     * form for its class. NOT_FOUND ends form enumeration. Secret readback
     * is empty even when present is true. Unchanged fields keep their
     * existing values and a changed empty secret clears it. */
    int (*settings_read)(const char *class_name, solar_os_messaging_provider_id_t provider,
        uint32_t form, solar_os_message_settings_v1_t *settings);
    int (*settings_write)(const char *class_name, solar_os_messaging_provider_id_t provider,
        uint32_t form, const char *name, const solar_os_message_setting_values_v1_t *settings,
        solar_os_messaging_provider_id_t *result);
    /* Transport workers publish through the same journal and event queue
     * used by the built-in transports. String and byte inputs are borrowed
     * during each call. Use the provider identity assigned by its registry. */
    int (*provider_claim)(solar_os_messaging_provider_id_t provider, const char *detail);
    int (*provider_set_status)(solar_os_messaging_provider_id_t provider,
        bool running, bool connected, int error, const char *detail);
    uint64_t (*provider_context)(solar_os_messaging_provider_id_t provider);
    uint64_t (*provider_cursor)(solar_os_messaging_provider_id_t provider,
        const char *provider_key, uint64_t mask, uint64_t tag);
    int (*publish_inbound)(const solar_os_messaging_inbound_t *entry, bool *inserted,
        solar_os_message_key_t *key);
    int (*conversation_upsert)(const solar_os_messaging_conversation_upsert_t *entry,
        solar_os_conversation_id_t *id);
    int (*conversation_remove)(solar_os_messaging_provider_id_t provider, const char *provider_key);
    int (*groups_begin_sync)(solar_os_messaging_provider_id_t provider, const char *key_prefix);
    int (*join_update)(solar_os_messaging_provider_id_t provider, const char *target,
        solar_os_messaging_join_state_t state, const char *detail);
    int (*outbox_peek)(solar_os_messaging_provider_id_t provider, solar_os_messaging_outbound_t *entry);
    int (*outbox_update)(uint32_t request_id, solar_os_delivery_state_t state, const char *detail);
} solar_os_native_messages_api_v1_t;
