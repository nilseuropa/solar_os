#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_messaging_types.h"
#include "solar_os_native_messages_abi.h"

#define SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY 32U
#define SOLAR_OS_MESSAGING_MESSAGE_CAPACITY 64U
#define SOLAR_OS_MESSAGING_OUTBOX_CAPACITY 16U
#define SOLAR_OS_MESSAGING_EVENT_CAPACITY 80U
#define SOLAR_OS_MESSAGING_STORE_DIR ".messages"
#define SOLAR_OS_MESSAGING_STORE_FILE "history.bin"

typedef struct {
    const char *label;
    const char *hint;
    size_t capacity; /* Includes the terminating NUL. */
    bool secret;
    bool required;
} solar_os_messaging_setting_t;

/* Forms own their copied descriptors and values. The registry clears secret
 * readback. Allocate forms outside the task stack. */
typedef solar_os_message_settings_v1_t solar_os_messaging_settings_t;

void solar_os_messaging_settings_form(solar_os_messaging_settings_t *settings, const char *label,
    const solar_os_messaging_setting_t *fields, size_t count);
void solar_os_messaging_settings_value(solar_os_messaging_settings_t *settings,
    size_t field, const char *value);
void solar_os_messaging_settings_clear(solar_os_messaging_settings_t *settings);

/* Class descriptors stay valid until unregistered. Operations lease their
 * class and instance contexts. Successful removal excludes workers before
 * freeing context. Callbacks run outside the Messaging lock. */
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
                                const solar_os_message_setting_values_v1_t *settings,
                                solar_os_messaging_provider_id_t *id);
    esp_err_t (*normalize_target)(void *context, const char *target,
                                  char *normalized, size_t size);
    /* A successful prepare excludes transport work until finish. A failed
     * prepare leaves the instance active. Finish cannot fail;
     * when removed is true it destroys the context, otherwise it resumes it. */
    esp_err_t (*remove_prepare)(void *context);
    void (*remove_finish)(void *context, bool removed);
    /* Portable classes supply an owner for class-level dispatch. An instance
     * may be absent when a settings callback describes a creation form. */
    void *owner;
    esp_err_t (*create_owned)(void *owner, const char *name, int argc,
        const char *const *argv, solar_os_messaging_provider_id_t *id);
    esp_err_t (*settings_read_owned)(void *owner, void *context, unsigned form,
        solar_os_messaging_settings_t *settings);
    esp_err_t (*settings_write_owned)(void *owner, void *context, unsigned form, const char *name,
        const solar_os_message_setting_values_v1_t *settings, solar_os_messaging_provider_id_t *id);
} solar_os_messaging_provider_class_t;

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
esp_err_t solar_os_messaging_class_unregister(const solar_os_messaging_provider_class_t *provider_class);
esp_err_t solar_os_messaging_instance_attach(solar_os_messaging_provider_id_t id,
    const solar_os_messaging_provider_class_t *owner, void *context);
esp_err_t solar_os_messaging_instance_detach(solar_os_messaging_provider_id_t id,
    const solar_os_messaging_provider_class_t *owner);
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
esp_err_t solar_os_messaging_settings_write_values(const char *class_name,
    solar_os_messaging_provider_id_t id, unsigned form, const char *name,
    const solar_os_message_setting_values_v1_t *settings, solar_os_messaging_provider_id_t *result);
esp_err_t solar_os_messaging_join(solar_os_messaging_provider_id_t instance,
    const char *target, solar_os_conversation_id_t *conversation);
esp_err_t solar_os_messaging_leave(solar_os_conversation_id_t conversation);
/* Canonical room key; explicit departures persist independently of connectivity. */
bool solar_os_messaging_room_left(solar_os_messaging_provider_id_t instance, const char *target);
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
