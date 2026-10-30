#include "solar_os_messaging.h"

#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_inbox.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_contacts.h"
#include "solar_os_storage.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_CREDENTIALS
#include "solar_os_credentials.h"
#endif

#define MESSAGING_STORE_MAGIC 0x47534d53UL
#define MESSAGING_STORE_VERSION 1U
#define MESSAGING_STORE_HEADER_COPIES 2U

typedef struct {
    solar_os_messaging_message_t message;
    int32_t disk_index;
    uint32_t revision;
} messaging_message_slot_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint32_t generation;
    uint32_t head;
    uint32_t count;
    uint32_t dropped;
    uint64_t next_message_key;
    uint32_t crc32;
} messaging_store_header_t;

typedef struct {
    solar_os_messaging_message_t message;
    char conversation_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    char conversation_title[SOLAR_OS_MESSAGING_TITLE_MAX];
    solar_os_conversation_kind_t conversation_kind;
    uint32_t group_ref;
    uint32_t conversation_security;
    uint32_t crc32;
} messaging_store_record_t;

#define MESSAGING_STORE_RECORDS_OFFSET \
    (MESSAGING_STORE_HEADER_COPIES * sizeof(messaging_store_header_t))
/* Disk records have two independently checked copies. Cache capacity does not
 * limit history. Updates replace the older copy after the new copy is synced. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint64_t next_message_key;
    uint64_t lookup_key;
    int32_t previous_index;
    messaging_store_record_t record;
    uint32_t crc32;
} messaging_history_copy_t;

#define MESSAGING_HISTORY_RECORD_BYTES (2U * sizeof(messaging_history_copy_t))
#define MESSAGING_HISTORY_BUCKETS 256U
#define MESSAGING_HISTORY_MAGIC 0x48534d53UL
#define MESSAGING_HISTORY_VERSION 1U
#define MESSAGING_HISTORY_PAGE_CAPACITY 80U

/* Bounded navigation metadata, independent of retained disk history size. */
typedef struct {
    solar_os_conversation_id_t conversation;
    size_t count;
    uint64_t provider_cursor;
    int32_t recent[MESSAGING_HISTORY_PAGE_CAPACITY];
    size_t recent_head;
    size_t recent_count;
    int32_t unread[MESSAGING_HISTORY_PAGE_CAPACITY];
    size_t unread_count;
    bool unread_overflow;
} messaging_history_navigation_t;

typedef struct messaging_provider_node {
    struct messaging_provider_node *next;
    solar_os_messaging_provider_status_t status;
    char group_prefix[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    void *context;
    unsigned operations;
    bool removing;
} messaging_provider_node_t;
typedef struct messaging_class_node {
    struct messaging_class_node *next;
    const solar_os_messaging_provider_class_t *type;
} messaging_class_node_t;

typedef struct {
    bool initialized;
    solar_os_messaging_conversation_t *conversations;
    size_t conversation_count;
    messaging_message_slot_t *messages;
    size_t message_head;
    size_t message_count;
    size_t message_unread;
    solar_os_messaging_outbound_t *outbox;
    size_t outbox_head;
    size_t outbox_count;
    solar_os_messaging_event_t *events;
    size_t event_head;
    size_t event_count;
    messaging_provider_node_t *providers;
    messaging_class_node_t *classes;
    messaging_store_record_t *record_scratch;
    messaging_history_copy_t *history_scratch;
    messaging_message_slot_t *pending_slot;
    int32_t *history_buckets;
    messaging_history_navigation_t *navigation;
    uint32_t next_conversation_id;
    uint32_t next_outbox_id;
    uint32_t next_event_id;
    uint64_t next_message_key;
    uint32_t generation;
    uint32_t dropped_messages;
    uint32_t dropped_outbox;
    uint32_t dropped_events;
    bool persistent;
    bool storage_maintenance;
    bool sd_promotion_checked;
    size_t disk_head;
    size_t disk_count;
    esp_err_t storage_error;
    char store_path[SOLAR_OS_STORAGE_PATH_MAX];
    char location_path[SOLAR_OS_STORAGE_PATH_MAX];
    char profile_directory[SOLAR_OS_STORAGE_PATH_MAX];
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
    SemaphoreHandle_t history_lock;
} messaging_state_t;

static messaging_state_t messaging;
static const char *TAG = "messaging";

static void messaging_lock(void)
{
    (void)xSemaphoreTake(messaging.lock, portMAX_DELAY);
}

static void messaging_unlock(void)
{
    xSemaphoreGive(messaging.lock);
}

/* History operations serialize independently of metadata. Disk transfers may
 * release the state mutex while retaining ownership of disk scratch and cache
 * mutations. Provider callbacks and Inbox calls run outside both locks. */
static void messaging_history_lock(void)
{
    (void)xSemaphoreTakeRecursive(messaging.history_lock, portMAX_DELAY);
    messaging_lock();
}

static void messaging_history_unlock(void)
{
    messaging_unlock();
    xSemaphoreGiveRecursive(messaging.history_lock);
}

static uint32_t messaging_next_u32(uint32_t *next)
{
    uint32_t value = (*next)++;
    if (value == 0) {
        value = (*next)++;
    }
    return value;
}

static uint64_t messaging_next_u64(uint64_t *next)
{
    uint64_t value = (*next)++;
    if (value == 0) {
        value = (*next)++;
    }
    return value;
}

static void messaging_note_generation_locked(void)
{
    messaging.generation++;
    if (messaging.generation == 0) {
        messaging.generation = 1U;
    }
}

static uint32_t messaging_crc32(const void *data, size_t len)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ table[crc & 15U];
        crc = (crc >> 4) ^ table[crc & 15U];
    }
    return ~crc;
}

static uint64_t messaging_hash_update(uint64_t hash,
                                      const void *data,
                                      size_t length)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
    return hash;
}

static uint64_t messaging_hash_string(uint64_t hash, const char *text)
{
    const char *value = text != NULL ? text : "";
    return messaging_hash_update(hash, value, strlen(value));
}

static bool messaging_text_valid(const char *text,
                                 size_t capacity,
                                 bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t length = strlen(text);
    if ((!allow_empty && length == 0) || length >= capacity) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0';
         cursor++) {
        if (*cursor < 0x20U && *cursor != '\n' && *cursor != '\t') {
            return false;
        }
        if (*cursor == 0x7fU) {
            return false;
        }
    }
    return true;
}

static bool messaging_provider_valid(solar_os_messaging_provider_id_t provider)
{
    return provider != 0 && provider <= SOLAR_OS_MESSAGING_INSTANCE_ID_MAX;
}

static messaging_provider_node_t *messaging_provider_node_locked(
    solar_os_messaging_provider_id_t provider)
{
    for (messaging_provider_node_t *node = messaging.providers; node; node = node->next)
        if (node->status.id == provider) return node;
    return NULL;
}
static void messaging_profiles_restore(void);
static bool messaging_provider_class_locked(solar_os_messaging_provider_id_t id, const char *name)
{
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    return node && strcmp(node->status.class_name, name) == 0;
}
static esp_err_t messaging_remove_history(solar_os_messaging_provider_id_t id);

static int messaging_conversation_index_locked(
    solar_os_messaging_provider_id_t provider,
    const char *provider_key)
{
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        const solar_os_messaging_conversation_t *conversation =
            &messaging.conversations[i];
        if (conversation->provider == provider &&
            strcmp(conversation->provider_key, provider_key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int messaging_conversation_id_index_locked(
    solar_os_conversation_id_t conversation_id)
{
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        if (messaging.conversations[i].id == conversation_id) {
            return (int)i;
        }
    }
    return -1;
}

static solar_os_conversation_id_t messaging_upsert_conversation_locked(
    const solar_os_messaging_conversation_upsert_t *request,
    bool publish_event)
{
    int index = messaging_conversation_index_locked(request->provider,
                                                    request->provider_key);
    if (index < 0) {
        if (messaging.conversation_count >=
            SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY) {
            return SOLAR_OS_CONVERSATION_ID_NONE;
        }
        index = (int)messaging.conversation_count++;
        memset(&messaging.conversations[index],
               0,
               sizeof(messaging.conversations[index]));
        messaging.conversations[index].id =
            messaging_next_u32(&messaging.next_conversation_id);
    }
    solar_os_messaging_conversation_t *conversation =
        &messaging.conversations[index];
    conversation->provider = request->provider;
    conversation->kind = request->kind;
    conversation->contact_id = request->contact_id;
    conversation->endpoint_id = request->endpoint_id;
    conversation->group_ref = request->group_ref;
    conversation->security_flags = request->security_flags;
    messaging_provider_node_t *provider_node = messaging_provider_node_locked(request->provider);
    const char *group_prefix = provider_node ? provider_node->group_prefix : "";
    conversation->history_only = request->kind == SOLAR_OS_CONVERSATION_GROUP &&
        group_prefix[0] != '\0' &&
        strncmp(request->provider_key, group_prefix, strlen(group_prefix)) != 0;
    if (provider_node && provider_node->status.removed) conversation->history_only = true;
    strlcpy(conversation->provider_key,
            request->provider_key,
            sizeof(conversation->provider_key));
    strlcpy(conversation->title,
            request->title != NULL && request->title[0] != '\0' ?
                request->title : request->provider_key,
            sizeof(conversation->title));
    messaging_note_generation_locked();
    if (publish_event) {
        solar_os_messaging_event_t event = {
            .type = SOLAR_OS_MESSAGING_EVENT_CONVERSATION,
            .provider = request->provider,
            .conversation_id = conversation->id,
        };
        event.id = messaging_next_u32(&messaging.next_event_id);
        messaging.events[messaging.event_head] = event;
        messaging.event_head =
            (messaging.event_head + 1U) % SOLAR_OS_MESSAGING_EVENT_CAPACITY;
        if (messaging.event_count <
            SOLAR_OS_MESSAGING_EVENT_CAPACITY) {
            messaging.event_count++;
        } else {
            messaging.dropped_events++;
        }
    }
    return conversation->id;
}

static size_t messaging_message_oldest_locked(void)
{
    return (messaging.message_head + SOLAR_OS_MESSAGING_MESSAGE_CAPACITY -
            messaging.message_count) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
}

static messaging_message_slot_t *messaging_find_message_locked(
    solar_os_message_key_t key)
{
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        messaging_message_slot_t *slot =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
        if (slot->message.key == key) {
            return slot;
        }
    }
    return NULL;
}

static void messaging_publish_event_locked(
    solar_os_messaging_event_type_t type,
    solar_os_messaging_provider_id_t provider,
    solar_os_conversation_id_t conversation_id,
    solar_os_message_key_t message_key,
    solar_os_delivery_state_t delivery,
    esp_err_t error)
{
    solar_os_messaging_event_t *event = &messaging.events[messaging.event_head];
    memset(event, 0, sizeof(*event));
    event->id = messaging_next_u32(&messaging.next_event_id);
    event->type = type;
    event->provider = provider;
    event->conversation_id = conversation_id;
    event->message_key = message_key;
    event->delivery = delivery;
    event->error = error;
    messaging.event_head =
        (messaging.event_head + 1U) % SOLAR_OS_MESSAGING_EVENT_CAPACITY;
    if (messaging.event_count < SOLAR_OS_MESSAGING_EVENT_CAPACITY) {
        messaging.event_count++;
    } else {
        messaging.dropped_events++;
    }
}

static uint64_t messaging_inbound_key_locked(
    const solar_os_messaging_inbound_t *request)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = messaging_hash_update(hash, &request->provider,
                                 sizeof(request->provider));
    hash = messaging_hash_string(hash, request->conversation_key);
    if (request->provider_message_key != 0) {
        hash = messaging_hash_update(hash, &request->provider_message_key,
                                     sizeof(request->provider_message_key));
    } else {
        hash = messaging_hash_update(hash, &request->timestamp_ms,
                                     sizeof(request->timestamp_ms));
        hash = messaging_hash_string(hash, request->sender);
        hash = messaging_hash_string(hash, request->body);
    }
    return hash != 0 ? hash : 1U;
}

#include "solar_os_messaging_registry.inc"

static esp_err_t messaging_sync_file(FILE *file)
{
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

#include "solar_os_messaging_history.inc"

static void messaging_unlink_all_inbox_projections(void *user)
{
    (void)user;
    if (!messaging.initialized) {
        return;
    }

    solar_os_message_key_t keys[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    size_t count = 0;
    messaging_lock();
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        messaging_message_slot_t *slot =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
        if (slot->message.inbox_id == 0) {
            continue;
        }
        slot->message.inbox_id = 0;
        slot->revision++;
        keys[count++] = slot->message.key;
    }
    if (count > 0) {
        messaging_note_generation_locked();
    }
    messaging_unlock();

    if (messaging.persistent) {
        for (size_t i = 0; i < count; i++) {
            (void)messaging_store_update(keys[i]);
        }
    }
}

static void messaging_reconcile_inbox_projections(void)
{
    solar_os_message_key_t keys[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    uint32_t inbox_ids[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    size_t count = 0;

    messaging_lock();
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        const solar_os_messaging_message_t *message =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY].message;
        if (message->inbox_id != 0) {
            keys[count] = message->key;
            inbox_ids[count] = message->inbox_id;
            count++;
        }
    }
    messaging_unlock();

    for (size_t i = 0; i < count; i++) {
        if (solar_os_inbox_matches_source_id(inbox_ids[i], keys[i])) {
            continue;
        }
        bool unlinked = false;
        messaging_lock();
        messaging_message_slot_t *slot =
            messaging_find_message_locked(keys[i]);
        if (slot != NULL && slot->message.inbox_id == inbox_ids[i]) {
            slot->message.inbox_id = 0;
            slot->revision++;
            messaging_note_generation_locked();
            unlinked = true;
        }
        messaging_unlock();
        if (unlinked && messaging.persistent) {
            (void)messaging_store_update(keys[i]);
        }
    }
}

static void messaging_restore_record_locked(
    const messaging_store_record_t *record,
    int32_t disk_index)
{
    int previous = messaging_conversation_index_locked(record->message.provider, record->conversation_key);
    bool history_only = previous < 0 || messaging.conversations[previous].history_only;
    solar_os_messaging_conversation_upsert_t conversation_request = {
        .provider = record->message.provider,
        .provider_key = record->conversation_key,
        .kind = record->conversation_kind,
        .title = record->conversation_title,
        .contact_id = record->message.contact_id,
        .endpoint_id = record->message.endpoint_id,
        .group_ref = record->group_ref,
        .security_flags = record->conversation_security,
    };
    const solar_os_conversation_id_t conversation_id =
        messaging_upsert_conversation_locked(&conversation_request, false);
    if (conversation_id == SOLAR_OS_CONVERSATION_ID_NONE) {
        return;
    }
    messaging_message_slot_t *slot =
        &messaging.messages[messaging.message_head];
    memset(slot, 0, sizeof(*slot));
    slot->message = record->message;
    if (slot->message.direction == SOLAR_OS_MESSAGE_OUTBOUND &&
        !messaging_message_pending_locked(slot->message.key) &&
        (slot->message.delivery == SOLAR_OS_DELIVERY_QUEUED ||
         slot->message.delivery == SOLAR_OS_DELIVERY_SENDING)) {
        slot->message.delivery = SOLAR_OS_DELIVERY_FAILED;
        strlcpy(slot->message.error, "Send interrupted by restart; resend explicitly",
                sizeof(slot->message.error));
    }
    const int restored_index = messaging_conversation_id_index_locked(conversation_id);
    if (restored_index >= 0 && messaging_provider_class_locked(record->message.provider, "meshcore") &&
        record->conversation_kind == SOLAR_OS_CONVERSATION_GROUP) {
        messaging.conversations[restored_index].history_only = history_only;
    }
    slot->message.conversation_id = conversation_id;
    slot->disk_index = disk_index;
    messaging_history_note_locked(&slot->message, disk_index);
    slot->revision = messaging.generation;
    messaging.message_head =
        (messaging.message_head + 1U) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    if (messaging.message_count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
        messaging.message_count++;
    }
    if (slot->message.unread) {
        messaging.message_unread++;
    }
    const int conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (conversation_index >= 0) {
        solar_os_messaging_conversation_t *conversation =
            &messaging.conversations[conversation_index];
        if (slot->message.unread) {
            conversation->unread_count++;
        }
        if (slot->message.timestamp_ms > conversation->last_message_ms) {
            conversation->last_message_ms = slot->message.timestamp_ms;
        }
        conversation->security_flags |= slot->message.security_flags;
    }
}

static void messaging_publish_inbox_projection(
    const solar_os_messaging_inbound_t *request,
    solar_os_message_key_t message_key,
    uint32_t revision)
{
    char title[SOLAR_OS_INBOX_TITLE_MAX];
    snprintf(title,
             sizeof(title),
             "%s %s",
             solar_os_messaging_provider_name(request->provider),
             request->conversation_title != NULL ?
                 request->conversation_title : request->conversation_key);
    char dedupe_key[24];
    snprintf(dedupe_key,
             sizeof(dedupe_key),
             "%016llx",
             (unsigned long long)message_key);
    const char *source =
        request->provider == SOLAR_OS_MESSAGING_PROVIDER_MESHCORE ?
            "meshcore" :
        request->provider == SOLAR_OS_MESSAGING_PROVIDER_LINK ?
            "link-chat" :
            "messages";
    char instance_source[16];
    if (request->provider > SOLAR_OS_MESSAGING_PROVIDER_LINK) {
        snprintf(instance_source, sizeof(instance_source), "msg-%03" PRIu32, request->provider);
        source = instance_source;
    }
    const solar_os_inbox_publish_t notification = {
        .source = source,
        .topic = request->conversation_key,
        .sender = request->sender,
        .title = title,
        .body = request->body,
        .dedupe_key = dedupe_key,
        .source_id = message_key,
        .source_context =
            solar_os_messaging_provider_context(request->provider),
        .timestamp_ms = request->timestamp_ms,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    uint32_t inbox_id = 0;
    const esp_err_t error = solar_os_inbox_publish(&notification, &inbox_id);
    if (error != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "Inbox projection failed: %s",
                      esp_err_to_name(error));
        return;
    }
    const bool projection_present =
        solar_os_inbox_matches_source_id(inbox_id, message_key);
    messaging_history_lock();
    messaging_message_slot_t *slot =
        messaging_history_cache_locked(message_key);
    const bool linked =
        slot != NULL && slot->revision == revision && slot->message.unread &&
        projection_present;
    if (linked) {
        slot->message.inbox_id = inbox_id;
        slot->revision++;
    }
    messaging_history_unlock();
    if (linked && messaging.persistent) {
        (void)messaging_store_update(message_key);
    } else if (!linked) {
        (void)solar_os_inbox_delete(inbox_id);
    }
}

esp_err_t solar_os_messaging_init(void)
{
    if (messaging.initialized) {
        return ESP_OK;
    }
    messaging.lock = xSemaphoreCreateMutex();
    messaging.io_lock = xSemaphoreCreateMutex();
    messaging.history_lock = xSemaphoreCreateRecursiveMutex();
    if (messaging.lock == NULL || messaging.io_lock == NULL || messaging.history_lock == NULL) {
        if (messaging.lock != NULL) {
            vSemaphoreDelete(messaging.lock);
        }
        if (messaging.io_lock != NULL) {
            vSemaphoreDelete(messaging.io_lock);
        }
        if (messaging.history_lock != NULL) vSemaphoreDelete(messaging.history_lock);
        memset(&messaging, 0, sizeof(messaging));
        return ESP_ERR_NO_MEM;
    }
    messaging.conversations = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
        sizeof(*messaging.conversations),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.conversations");
    messaging.messages = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_MESSAGE_CAPACITY,
        sizeof(*messaging.messages),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.ring");
    messaging.outbox = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_OUTBOX_CAPACITY,
        sizeof(*messaging.outbox),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.outbox");
    messaging.events = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_EVENT_CAPACITY,
        sizeof(*messaging.events),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.events");
    messaging.record_scratch = solar_os_memory_calloc(
        1,
        sizeof(*messaging.record_scratch),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.record");
    messaging.history_scratch = solar_os_memory_calloc(2, sizeof(*messaging.history_scratch),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.disk.copies");
    messaging.pending_slot = solar_os_memory_calloc(1, sizeof(*messaging.pending_slot),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.pending");
    messaging.history_buckets = solar_os_memory_calloc(MESSAGING_HISTORY_BUCKETS,
        sizeof(*messaging.history_buckets), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.index");
    messaging.navigation = solar_os_memory_calloc(SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
        sizeof(*messaging.navigation), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.navigation");
    if (messaging.conversations == NULL || messaging.messages == NULL ||
        messaging.outbox == NULL || messaging.events == NULL ||
        messaging.record_scratch == NULL || messaging.history_scratch == NULL ||
        messaging.pending_slot == NULL || messaging.history_buckets == NULL || messaging.navigation == NULL) {
        solar_os_memory_free(messaging.conversations);
        solar_os_memory_free(messaging.messages);
        solar_os_memory_free(messaging.outbox);
        solar_os_memory_free(messaging.events);
        solar_os_memory_free(messaging.record_scratch);
        solar_os_memory_free(messaging.history_scratch);
        solar_os_memory_free(messaging.pending_slot);
        solar_os_memory_free(messaging.history_buckets);
        solar_os_memory_free(messaging.navigation);
        vSemaphoreDelete(messaging.lock);
        vSemaphoreDelete(messaging.io_lock);
        vSemaphoreDelete(messaging.history_lock);
        memset(&messaging, 0, sizeof(messaging));
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY; i++) {
        messaging.messages[i].disk_index = -1;
    }
    messaging.next_conversation_id = 1U;
    messaging.next_outbox_id = 1U;
    messaging.next_event_id = 1U;
    messaging.next_message_key = 1U;
    memset(messaging.history_buckets, 0xff, MESSAGING_HISTORY_BUCKETS * sizeof(*messaging.history_buckets));
    messaging.generation = 1U;
    messaging.initialized = true;
    messaging_profiles_restore();
    messaging_history_lock();
    (void)messaging_store_load();
    messaging_history_unlock();
    solar_os_inbox_set_clear_observer(messaging_unlink_all_inbox_projections,
                                      NULL);
    messaging_reconcile_inbox_projections();
    /* A committed removal record is also a durable purge request. Complete
     * deletions interrupted by reset before exposing restored conversations. */
    for (messaging_provider_node_t *node = messaging.providers; node; node = node->next) {
        if (!node->status.removed) continue;
        esp_err_t error = messaging_remove_history(node->status.id);
        if (error != ESP_OK) messaging.storage_error = error;
    }
    return ESP_OK;
}

esp_err_t solar_os_messaging_get_status(solar_os_messaging_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    /* Maintenance is opportunistic: ordinary status queries never wait for a
     * history writer. Retry attachment when a saved volume mounts late. */
    if (xSemaphoreTakeRecursive(messaging.history_lock, 0) == pdTRUE) {
        messaging_lock();
        if (!messaging.storage_maintenance) {
            messaging.storage_maintenance = true;
            if (!messaging.persistent) (void)messaging_history_ensure_locked();
            if (messaging.persistent) (void)messaging_history_promote_sd_locked();
            messaging.storage_maintenance = false;
        }
        messaging_unlock();
        xSemaphoreGiveRecursive(messaging.history_lock);
    }
    messaging_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = messaging.initialized;
    status->rings_in_psram = true;
    status->persistent = messaging.persistent;
    status->inbox_backed = false;
    status->conversations = messaging.conversation_count;
    status->messages = messaging.disk_count;
    status->cached_messages = messaging.message_count;
    status->unread = messaging.message_unread;
    status->queued_outbox = messaging.outbox_count;
    status->queued_events = messaging.event_count;
    /* Zero means history is limited by available disk space, not cache size. */
    status->persistent_capacity = 0;
    status->persistent_limit_bytes = 0;
    status->dropped_messages = messaging.dropped_messages;
    status->dropped_outbox = messaging.dropped_outbox;
    status->generation = messaging.generation;
    status->storage_error = messaging.storage_error;
    messaging_unlock();
    return ESP_OK;
}

esp_err_t solar_os_messaging_provider_register(
    solar_os_messaging_provider_id_t provider, const char *name)
{
    return solar_os_messaging_instance_register(provider, name, name, NULL);
}

esp_err_t solar_os_messaging_provider_set_status(
    solar_os_messaging_provider_id_t provider,
    bool running,
    bool connected,
    esp_err_t error,
    const char *detail)
{
    if (!messaging_provider_valid(provider)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t init_error = solar_os_messaging_init();
    if (init_error != ESP_OK) {
        return init_error;
    }
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(provider);
    if (node == NULL) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    solar_os_messaging_provider_status_t *status = &node->status;
    if (!status->registered || status->removed) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const char *next_detail = detail != NULL ? detail : "";
    if (status->running == running &&
        status->connected == connected &&
        status->last_error == error &&
        strcmp(status->detail, next_detail) == 0) {
        messaging_unlock();
        return ESP_OK;
    }
    if ((status->connected && !connected) || (status->running && !running)) {
        for (size_t i = 0; i < messaging.conversation_count; i++) {
            solar_os_messaging_conversation_t *conversation = &messaging.conversations[i];
            if (conversation->provider == provider &&
                (conversation->join_state == SOLAR_OS_MESSAGING_JOIN_JOINED ||
                 conversation->join_state == SOLAR_OS_MESSAGING_JOIN_PENDING)) {
                conversation->join_state = SOLAR_OS_MESSAGING_JOIN_LEFT;
                strlcpy(conversation->join_error, detail ? detail : "disconnected", sizeof(conversation->join_error));
            }
        }
    }
    status->running = running;
    status->connected = connected;
    status->last_error = error;
    strlcpy(status->detail, next_detail, sizeof(status->detail));
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_PROVIDER,
                                    provider,
                                    0,
                                    0,
                                    SOLAR_OS_DELIVERY_RECEIVED,
                                    error);
    messaging_unlock();
    return ESP_OK;
}

esp_err_t solar_os_messaging_provider_claim(
    solar_os_messaging_provider_id_t provider,
    const char *detail)
{
    if (!messaging_provider_valid(provider)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(provider);
    if (node == NULL) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    solar_os_messaging_provider_status_t *status = &node->status;
    if (!status->registered || status->removed || node->removing) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (status->running) {
        messaging_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    status->running = true;
    status->connected = false;
    status->last_error = ESP_OK;
    strlcpy(status->detail, detail != NULL ? detail : "starting",
            sizeof(status->detail));
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_PROVIDER,
                                    provider,
                                    0,
                                    0,
                                    SOLAR_OS_DELIVERY_RECEIVED,
                                    ESP_OK);
    messaging_unlock();
    return ESP_OK;
}

esp_err_t solar_os_messaging_provider_get_status(
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_provider_status_t *status)
{
    if (!messaging_provider_valid(provider) || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(provider);
    if (node == NULL) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    *status = node->status;
    messaging_unlock();
    return status->registered ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_messaging_conversation_upsert(
    const solar_os_messaging_conversation_upsert_t *request,
    solar_os_conversation_id_t *conversation_id)
{
    if (request == NULL || !messaging_provider_valid(request->provider) ||
        !messaging_text_valid(request->provider_key,
                              SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX,
                              false) ||
        (request->title != NULL &&
         !messaging_text_valid(request->title,
                               SOLAR_OS_MESSAGING_TITLE_MAX,
                               true))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_lock();
    messaging_provider_node_t *provider = messaging_provider_node_locked(request->provider);
    if (provider && (provider->status.removed || provider->removing)) {
        messaging_unlock(); return ESP_ERR_INVALID_STATE;
    }
    const solar_os_conversation_id_t id =
        messaging_upsert_conversation_locked(request, true);
    messaging_unlock();
    if (id == SOLAR_OS_CONVERSATION_ID_NONE) {
        return ESP_ERR_NO_MEM;
    }
    if (conversation_id != NULL) {
        *conversation_id = id;
    }
    return ESP_OK;
}

esp_err_t solar_os_messaging_groups_begin_sync(
    solar_os_messaging_provider_id_t provider, const char *provider_key_prefix)
{
    if (!messaging_provider_valid(provider) ||
        !messaging_text_valid(provider_key_prefix, SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(provider);
    if (node == NULL) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    strlcpy(node->group_prefix, provider_key_prefix, SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX);
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        solar_os_messaging_conversation_t *conversation = &messaging.conversations[i];
        if (conversation->provider == provider &&
            conversation->kind == SOLAR_OS_CONVERSATION_GROUP) {
            conversation->history_only = true;
        }
    }
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_CONVERSATION, provider,
        0, 0, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
    messaging_unlock();
    return ESP_OK;
}

void solar_os_messaging_conversation_label(
    const solar_os_messaging_conversation_t *conversation,
    char *label, size_t capacity)
{
    if (conversation == NULL || label == NULL || capacity == 0U) return;
    const char *transport = "";
    if (solar_os_messaging_provider_is_class(conversation->provider, "meshcore") &&
        conversation->kind == SOLAR_OS_CONVERSATION_GROUP) {
        if (strncmp(conversation->provider_key, "ble-group:", 10U) == 0) transport = " [companion]";
        else if (strncmp(conversation->provider_key, "group:", 6U) == 0) transport = " [radio]";
    }
    const char *history = conversation->history_only ? " (history)" : "";
    const size_t suffix = strlen(transport) + strlen(history);
    const size_t title_limit = capacity > suffix + 1U ? capacity - suffix - 1U : 0U;
    snprintf(label, capacity, "%.*s%s%s", (int)title_limit,
             conversation->title, transport, history);
}

esp_err_t solar_os_messaging_conversation_remove(
    solar_os_messaging_provider_id_t provider,
    const char *provider_key)
{
    if (!messaging_provider_valid(provider) ||
        !messaging_text_valid(provider_key,
                              SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX,
                              false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    uint32_t inbox_ids[SOLAR_OS_INBOX_CAPACITY];
    solar_os_message_key_t inbox_keys[SOLAR_OS_INBOX_CAPACITY];
    size_t linked = 0;
    messaging_history_lock();
    int index = messaging_conversation_index_locked(provider, provider_key);
    if (index < 0) { messaging_history_unlock(); return ESP_ERR_NOT_FOUND; }
    solar_os_conversation_id_t id = messaging.conversations[index].id;
    size_t removed = 0;
    error = messaging_history_inbox_links_locked(id, inbox_ids, inbox_keys, &linked);
    if (error == ESP_OK) error = messaging_history_clear_locked(provider, id, &removed);
    if (error != ESP_OK) { messaging_history_unlock(); return error; }
    id = messaging.conversations[index].id;
    memmove(&messaging.conversations[index],
            &messaging.conversations[index + 1],
            (messaging.conversation_count - (size_t)index - 1U) *
                sizeof(*messaging.conversations));
    messaging.conversation_count--;
    messaging_note_generation_locked();
    messaging_publish_event_locked(
        SOLAR_OS_MESSAGING_EVENT_CONVERSATION_REMOVED,
        provider,
        id,
        0,
        SOLAR_OS_DELIVERY_RECEIVED,
        ESP_OK);
    messaging_history_unlock();
    for (size_t i = 0; i < linked; i++) {
        if (!solar_os_inbox_matches_source_id(inbox_ids[i], inbox_keys[i])) continue;
        esp_err_t next = solar_os_inbox_delete(inbox_ids[i]);
        if (next != ESP_OK && next != ESP_ERR_NOT_FOUND) error = next;
    }
    return error;
}

esp_err_t solar_os_messaging_conversation_get(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_conversation_t *conversation)
{
    if (conversation_id == 0 || conversation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_history_lock();
    error = messaging_history_ensure_locked();
    if (error != ESP_OK) { messaging_history_unlock(); return error; }
    const int index =
        messaging_conversation_id_index_locked(conversation_id);
    if (index >= 0) {
        *conversation = messaging.conversations[index];
    }
    messaging_history_unlock();
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_messaging_direct_open(
    solar_os_contact_id_t contact_id,
    solar_os_conversation_id_t *conversation_id)
{
    if (contact_id == 0 || conversation_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_contact_t contact;
    esp_err_t error = solar_os_contacts_get(contact_id, &contact);
    if (error != ESP_OK) {
        return error;
    }
    solar_os_endpoint_t *endpoints =
        solar_os_memory_calloc(SOLAR_OS_ENDPOINT_CAPACITY,
                               sizeof(*endpoints),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "messages.direct.endpoints");
    if (endpoints == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const size_t count =
        solar_os_contacts_endpoint_snapshot(contact_id,
                                            endpoints,
                                            SOLAR_OS_ENDPOINT_CAPACITY);
    const solar_os_endpoint_t *selected = NULL;
    for (size_t pass = 0; pass < 2 && selected == NULL; pass++) {
        const solar_os_contact_trust_t desired =
            pass == 0 ? SOLAR_OS_CONTACT_TRUST_TRUSTED :
                        SOLAR_OS_CONTACT_TRUST_DISCOVERED;
        for (size_t i = 0; i < count; i++) {
            solar_os_messaging_provider_status_t provider;
            if (solar_os_messaging_provider_get_status(endpoints[i].provider, &provider) != ESP_OK || provider.removed) continue;
            if (endpoints[i].trust == desired &&
                (endpoints[i].capabilities & SOLAR_OS_ENDPOINT_CAP_DIRECT) != 0) {
                selected = &endpoints[i];
                break;
            }
        }
    }
    if (selected == NULL) {
        solar_os_memory_free(endpoints);
        return ESP_ERR_INVALID_STATE;
    }
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    snprintf(provider_key,
             sizeof(provider_key),
             "direct:%" PRIu32,
             selected->id);
    uint32_t security_flags = 0;
    if (solar_os_messaging_provider_is_class(selected->provider, "meshcore")) {
        security_flags |= SOLAR_OS_SECURITY_ENCRYPTED |
            SOLAR_OS_SECURITY_PEER_KEY_KNOWN;
    }
    if (selected->trust == SOLAR_OS_CONTACT_TRUST_TRUSTED) {
        security_flags |= SOLAR_OS_SECURITY_PEER_TRUSTED;
    }
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = selected->provider,
        .provider_key = provider_key,
        .kind = SOLAR_OS_CONVERSATION_DIRECT,
        .title = contact.display_name,
        .contact_id = contact.id,
        .endpoint_id = selected->id,
        .security_flags = security_flags,
    };
    error = solar_os_messaging_conversation_upsert(&request,
                                                   conversation_id);
    solar_os_memory_free(endpoints);
    return error;
}

size_t solar_os_messaging_conversation_snapshot(
    solar_os_messaging_conversation_t *conversations,
    size_t max_conversations)
{
    if (conversations == NULL || max_conversations == 0 ||
        solar_os_messaging_init() != ESP_OK) {
        return 0;
    }
    messaging_history_lock();
    if (messaging_history_ensure_locked() != ESP_OK) { messaging_history_unlock(); return 0; }
    const size_t count = messaging.conversation_count < max_conversations ?
        messaging.conversation_count : max_conversations;
    memcpy(conversations,
           messaging.conversations,
           count * sizeof(*conversations));
    messaging_history_unlock();
    return count;
}

esp_err_t solar_os_messaging_publish_inbound(
    const solar_os_messaging_inbound_t *request,
    bool *inserted,
    solar_os_message_key_t *message_key)
{
    if (inserted != NULL) {
        *inserted = false;
    }
    if (message_key != NULL) {
        *message_key = 0;
    }
    if (request == NULL || !messaging_provider_valid(request->provider) ||
        !messaging_text_valid(request->conversation_key,
                              SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX,
                              false) ||
        !messaging_text_valid(request->body,
                              SOLAR_OS_MESSAGING_BODY_MAX,
                              true) ||
        (request->sender != NULL &&
         !messaging_text_valid(request->sender,
                               SOLAR_OS_MESSAGING_SENDER_MAX,
                               true))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_history_lock();
    if (!messaging.persistent && messaging_history_ensure_locked() != ESP_OK) {
        error = messaging.storage_error;
        messaging_history_unlock();
        return error;
    }
    const solar_os_message_key_t key =
        messaging_inbound_key_locked(request);
    if (messaging_history_cache_locked(key) != NULL) {
        messaging_history_unlock();
        if (message_key != NULL) {
            *message_key = key;
        }
        return ESP_OK;
    }
    if (messaging.storage_error != ESP_OK) { error = messaging.storage_error; messaging_history_unlock(); return error; }
    if (request->endpoint_id != 0) {
        solar_os_endpoint_t endpoint;
        messaging_history_unlock();
        error = solar_os_contacts_get_endpoint(request->endpoint_id, &endpoint);
        if (error != ESP_OK) {
            return error;
        }
        if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) {
            return ESP_ERR_INVALID_STATE;
        }
        messaging_history_lock();
        if (messaging_history_cache_locked(key) != NULL) {
            messaging_history_unlock();
            if (message_key != NULL) {
                *message_key = key;
            }
            return ESP_OK;
        }
        if (messaging.storage_error != ESP_OK) { error = messaging.storage_error; messaging_history_unlock(); return error; }
    }
    solar_os_messaging_conversation_upsert_t conversation_request = {
        .provider = request->provider,
        .provider_key = request->conversation_key,
        .kind = request->conversation_kind,
        .title = request->conversation_title,
        .contact_id = request->contact_id,
        .endpoint_id = request->endpoint_id,
        .group_ref = request->group_ref,
        .security_flags = request->security_flags,
    };
    messaging_provider_node_t *provider = messaging_provider_node_locked(request->provider);
    if (provider && provider->status.removed) { messaging_history_unlock(); return ESP_ERR_INVALID_STATE; }
    const solar_os_conversation_id_t conversation_id =
        messaging_upsert_conversation_locked(&conversation_request, false);
    if (conversation_id == 0) {
        messaging_history_unlock();
        return ESP_ERR_NO_MEM;
    }
    messaging_message_slot_t *slot = messaging.pending_slot;
    memset(slot, 0, sizeof(*slot));
    slot->disk_index = -1;
    slot->message.key = key;
    slot->message.provider_message_key = request->provider_message_key;
    slot->message.conversation_id = conversation_id;
    slot->message.contact_id = request->contact_id;
    slot->message.endpoint_id = request->endpoint_id;
    slot->message.timestamp_ms = request->timestamp_ms;
    slot->message.received_ms =
        pdTICKS_TO_MS(xTaskGetTickCount());
    slot->message.provider = request->provider;
    slot->message.direction = SOLAR_OS_MESSAGE_INBOUND;
    slot->message.delivery = SOLAR_OS_DELIVERY_RECEIVED;
    slot->message.security_flags = request->security_flags;
    slot->message.unread = true;
    slot->message.truncated = request->truncated;
    strlcpy(slot->message.sender,
            request->sender != NULL ? request->sender : "",
            sizeof(slot->message.sender));
    strlcpy(slot->message.body, request->body, sizeof(slot->message.body));
    error = messaging_history_commit_locked(slot);
    if (error != ESP_OK) { messaging_history_unlock(); return error; }
    messaging.messages[messaging.message_head] = *slot;
    slot = &messaging.messages[messaging.message_head];
    messaging.message_head =
        (messaging.message_head + 1U) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    if (messaging.message_count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
        messaging.message_count++;
    }
    messaging.message_unread++;
    const int conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (conversation_index >= 0) {
        solar_os_messaging_conversation_t *conversation =
            &messaging.conversations[conversation_index];
        conversation->unread_count++;
        conversation->last_message_ms = request->timestamp_ms;
        conversation->security_flags |= request->security_flags;
    }
    messaging_note_generation_locked();
    slot->revision = messaging.generation;
    const uint32_t revision = slot->revision;
    messaging_history_unlock();

    messaging_publish_inbox_projection(request, key, revision);
    messaging_history_lock();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_MESSAGE,
                                    request->provider,
                                    conversation_id,
                                    key,
                                    SOLAR_OS_DELIVERY_RECEIVED,
                                    ESP_OK);
    messaging_history_unlock();
    if (inserted != NULL) {
        *inserted = true;
    }
    if (message_key != NULL) {
        *message_key = key;
    }
    return ESP_OK;
}

esp_err_t solar_os_messaging_send(solar_os_conversation_id_t conversation_id,
                                  const char *body,
                                  bool allow_untrusted,
                                  solar_os_message_key_t *message_key)
{
    if (conversation_id == 0 ||
        !messaging_text_valid(body, SOLAR_OS_MESSAGING_BODY_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    solar_os_messaging_conversation_t conversation_snapshot;
    messaging_history_lock();
    if (!messaging.persistent && messaging_history_ensure_locked() != ESP_OK) {
        error = messaging.storage_error;
        messaging_history_unlock();
        return error;
    }
    const int conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (conversation_index < 0) {
        messaging_history_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (messaging.outbox_count >= SOLAR_OS_MESSAGING_OUTBOX_CAPACITY) {
        messaging.dropped_outbox++;
        messaging_history_unlock();
        return ESP_ERR_NO_MEM;
    }
    solar_os_messaging_conversation_t *conversation =
        &messaging.conversations[conversation_index];
    conversation_snapshot = *conversation;
    messaging_history_unlock();
    if (conversation_snapshot.history_only) return ESP_ERR_NOT_SUPPORTED;
    if (conversation_snapshot.kind == SOLAR_OS_CONVERSATION_DIRECT &&
        conversation_snapshot.endpoint_id != 0) {
        solar_os_endpoint_t endpoint;
        error = solar_os_contacts_get_endpoint(
            conversation_snapshot.endpoint_id,
            &endpoint);
        if (error != ESP_OK) {
            return error;
        }
        if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED ||
            (endpoint.trust == SOLAR_OS_CONTACT_TRUST_DISCOVERED &&
             !allow_untrusted)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    messaging_history_lock();
    const int current_conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (current_conversation_index < 0) {
        messaging_history_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    conversation = &messaging.conversations[current_conversation_index];
    messaging_provider_node_t *provider = messaging_provider_node_locked(conversation->provider);
    if (provider && (provider->status.removed || provider->removing)) {
        messaging_history_unlock(); return ESP_ERR_INVALID_STATE;
    }
    if (conversation->history_only) {
        messaging_history_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (messaging.outbox_count >= SOLAR_OS_MESSAGING_OUTBOX_CAPACITY) {
        messaging.dropped_outbox++;
        messaging_history_unlock();
        return ESP_ERR_NO_MEM;
    }
    const solar_os_message_key_t key =
        messaging_next_u64(&messaging.next_message_key);
    messaging_message_slot_t *slot = messaging.pending_slot;
    memset(slot, 0, sizeof(*slot));
    slot->disk_index = -1;
    slot->message.key = key;
    slot->message.conversation_id = conversation_id;
    slot->message.contact_id = conversation->contact_id;
    slot->message.endpoint_id = conversation->endpoint_id;
    slot->message.received_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    slot->message.timestamp_ms = slot->message.received_ms;
    slot->message.provider = conversation->provider;
    slot->message.direction = SOLAR_OS_MESSAGE_OUTBOUND;
    slot->message.delivery = SOLAR_OS_DELIVERY_QUEUED;
    slot->message.security_flags = conversation->security_flags;
    strlcpy(slot->message.body, body, sizeof(slot->message.body));
    error = messaging_history_commit_locked(slot);
    if (error != ESP_OK) { messaging_history_unlock(); return error; }
    messaging.messages[messaging.message_head] = *slot;
    slot = &messaging.messages[messaging.message_head];
    messaging.message_head =
        (messaging.message_head + 1U) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    if (messaging.message_count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
        messaging.message_count++;
    }
    solar_os_messaging_outbound_t *outbound =
        &messaging.outbox[messaging.outbox_head];
    memset(outbound, 0, sizeof(*outbound));
    outbound->id = messaging_next_u32(&messaging.next_outbox_id);
    outbound->message_key = key;
    outbound->conversation_id = conversation_id;
    outbound->provider = conversation->provider;
    outbound->allow_untrusted = allow_untrusted;
    strlcpy(outbound->provider_key,
            conversation->provider_key,
            sizeof(outbound->provider_key));
    strlcpy(outbound->body, body, sizeof(outbound->body));
    messaging.outbox_head =
        (messaging.outbox_head + 1U) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    messaging.outbox_count++;
    messaging_note_generation_locked();
    slot->revision = messaging.generation;
    conversation->last_message_ms = slot->message.received_ms;
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_MESSAGE,
                                    slot->message.provider,
                                    conversation_id,
                                    key,
                                    SOLAR_OS_DELIVERY_QUEUED,
                                    ESP_OK);
    messaging_history_unlock();
    if (message_key != NULL) {
        *message_key = key;
    }
    return ESP_OK;
}

esp_err_t solar_os_messaging_message_page(solar_os_conversation_id_t conversation,
    solar_os_messaging_provider_id_t provider, size_t offset,
    solar_os_messaging_message_t *messages, size_t capacity, size_t *count, bool *more)
{
    return solar_os_messaging_message_page_filtered(conversation, provider, false,
        offset, messages, capacity, count, more);
}

esp_err_t solar_os_messaging_message_page_filtered(solar_os_conversation_id_t conversation,
    solar_os_messaging_provider_id_t provider, bool unread_only, size_t offset,
    solar_os_messaging_message_t *messages, size_t capacity, size_t *count, bool *more)
{
    if (!messages || !capacity || !count || !more) return ESP_ERR_INVALID_ARG;
    *count = 0; *more = false;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_history_lock();
    error = messaging_history_ensure_locked();
    if (error != ESP_OK) { messaging_history_unlock(); return error; }
    messaging_history_navigation_t *n = conversation ? messaging_history_navigation_locked(conversation, false) : NULL;
    int c = conversation ? messaging_conversation_id_index_locked(conversation) : -1;
    if (conversation && (!n || offset >= n->count || c < 0 ||
        (provider && messaging.conversations[c].provider != provider))) {
        messaging_history_unlock(); return ESP_OK;
    }
    bool indexed = !unread_only && n && offset <= n->recent_count &&
        (capacity <= n->recent_count - offset || n->count <= n->recent_count);
    size_t skip = offset;
    size_t limit = indexed ? n->recent_count - offset : messaging.disk_head;
    FILE *file = NULL;
    for (size_t i = 0; i < limit; i++) {
        int32_t index = indexed ? n->recent[(n->recent_head + MESSAGING_HISTORY_PAGE_CAPACITY - offset - i - 1) %
            MESSAGING_HISTORY_PAGE_CAPACITY] : (int32_t)(messaging.disk_head - i - 1);
        messaging_message_slot_t *cached = messaging_history_cached_index_locked(index);
        const solar_os_messaging_message_t *m;
        if (cached) m = &cached->message;
        else {
            if (!file) file = messaging_history_fopen(messaging.store_path, "rb");
            if (!file || messaging_history_read_locked(file, (size_t)index, messaging.record_scratch) < 0) { error = ESP_FAIL; break; }
            if (!messaging.record_scratch->message.key) continue;
            messaging_history_normalize_locked(messaging.record_scratch);
            m = &messaging.record_scratch->message;
        }
        if (!m->key || (conversation && m->conversation_id != conversation) ||
            (provider && m->provider != provider) || (unread_only && !m->unread)) continue;
        if (!indexed && skip) { skip--; continue; }
        if (*count == capacity) { *more = true; break; }
        messages[(*count)++] = *m;
        if (!cached) messaging_history_cache_record_locked(messaging.record_scratch, index);
    }
    if (file) messaging_history_fclose(file);
    if (n && !unread_only) *more = offset < n->count && *count < n->count - offset;
    for (size_t i = 0; i < *count / 2; i++) {
        messaging.pending_slot->message = messages[i];
        messages[i] = messages[*count - i - 1];
        messages[*count - i - 1] = messaging.pending_slot->message;
    }
    messaging.storage_error = error;
    messaging_history_unlock();
    return error;
}

size_t solar_os_messaging_message_visit_consistent(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_message_visitor_t visitor, void *user,
    uint32_t *event_cursor, uint32_t *generation)
{
    if (!visitor || solar_os_messaging_init() != ESP_OK) return 0;
    solar_os_messaging_message_t *batch = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_MESSAGE_CAPACITY, sizeof(*batch),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.snapshot");
    if (!batch) return 0;
    messaging_history_lock();
    if (!messaging.persistent) (void)messaging_history_ensure_locked();
    size_t limit = messaging.disk_head;
    if (generation) *generation = messaging.generation;
    if (event_cursor && messaging.event_count) {
        size_t newest = (messaging.event_head + SOLAR_OS_MESSAGING_EVENT_CAPACITY - 1) %
            SOLAR_OS_MESSAGING_EVENT_CAPACITY;
        *event_cursor = messaging.events[newest].id;
    }
    messaging_history_unlock();
    size_t visited = 0, index = 0;
    bool stopped = false;
    while (index < limit && !stopped) {
        size_t count = 0;
        messaging_history_lock();
        FILE *file = messaging_history_fopen(messaging.store_path, "rb");
        if (!file) { messaging.storage_error = ESP_FAIL; messaging_history_unlock(); break; }
        while (index < limit && count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
            if (messaging_history_read_locked(file, index++, messaging.record_scratch) < 0) {
                messaging.storage_error = ESP_FAIL; stopped = true; break;
            }
            if (!messaging.record_scratch->message.key) continue;
            messaging_history_normalize_locked(messaging.record_scratch);
            const solar_os_messaging_message_t *m = &messaging.record_scratch->message;
            if ((!conversation_id || m->conversation_id == conversation_id) &&
                (!provider || m->provider == provider)) batch[count++] = *m;
        }
        messaging_history_fclose(file);
        messaging_history_unlock();
        for (size_t i = 0; i < count; i++) {
            if (!visitor(&batch[i], user)) { stopped = true; break; }
            visited++;
        }
    }
    solar_os_memory_free(batch);
    return visited;
}

size_t solar_os_messaging_message_visit(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_message_visitor_t visitor,
    void *user,
    uint32_t *event_cursor)
{
    return solar_os_messaging_message_visit_consistent(conversation_id,
                                                       provider,
                                                       visitor,
                                                       user,
                                                       event_cursor,
                                                       NULL);
}

uint64_t solar_os_messaging_provider_cursor(
    solar_os_messaging_provider_id_t provider, const char *provider_key)
{
    if (!messaging_provider_valid(provider) || !provider_key ||
        solar_os_messaging_init() != ESP_OK) return 0;
    messaging_history_lock();
    if (!messaging.persistent) (void)messaging_history_ensure_locked();
    int c = messaging_conversation_index_locked(provider, provider_key);
    messaging_history_navigation_t *n = c >= 0 ?
        messaging_history_navigation_locked(messaging.conversations[c].id, false) : NULL;
    uint64_t cursor = n ? n->provider_cursor : 0;
    messaging_history_unlock();
    return cursor;
}

uint64_t solar_os_messaging_provider_cursor_masked(
    solar_os_messaging_provider_id_t provider,
    const char *provider_key,
    uint64_t mask,
    uint64_t tag)
{
    if (!mask && !tag) return solar_os_messaging_provider_cursor(provider, provider_key);
    if ((tag & ~mask) || !messaging_provider_valid(provider) ||
        !messaging_text_valid(provider_key,
                              SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX,
                              false) ||
        solar_os_messaging_init() != ESP_OK) {
        return 0;
    }
    messaging_history_lock();
    if (!messaging.persistent) (void)messaging_history_ensure_locked();
    const int conversation_index =
        messaging_conversation_index_locked(provider, provider_key);
    if (conversation_index < 0) {
        messaging_history_unlock();
        return 0;
    }
    const solar_os_conversation_id_t conversation_id =
        messaging.conversations[conversation_index].id;
    uint64_t cursor = 0;
    FILE *file = messaging_history_fopen(messaging.store_path, "rb");
    for (size_t i = 0; file && i < messaging.disk_head; i++) {
        if (messaging_history_read_locked(file, i, messaging.record_scratch) < 0) {
            messaging.storage_error = ESP_FAIL;
            break;
        }
        if (messaging.record_scratch->message.key)
            messaging_history_normalize_locked(messaging.record_scratch);
        const solar_os_messaging_message_t *message = &messaging.record_scratch->message;
        if (message->key && message->provider == provider &&
            message->conversation_id == conversation_id &&
            (message->provider_message_key & mask) == tag &&
            (message->provider_message_key & ~mask) > cursor) {
            cursor = message->provider_message_key & ~mask;
        }
    }
    if (file) messaging_history_fclose(file);
    messaging_history_unlock();
    return cursor;
}

esp_err_t solar_os_messaging_message_get(solar_os_message_key_t message_key,
                                         solar_os_messaging_message_t *message)
{
    if (message_key == 0 || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_history_lock();
    error = messaging_history_ensure_locked();
    if (error != ESP_OK) { messaging_history_unlock(); return error; }
    const messaging_message_slot_t *slot =
        messaging_history_cache_locked(message_key);
    if (slot != NULL) {
        *message = slot->message;
    }
    messaging_history_unlock();
    return slot != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool messaging_message_pending_locked(solar_os_message_key_t key)
{
    const size_t oldest =
        (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
         messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        if (messaging.outbox[(oldest + i) %
                             SOLAR_OS_MESSAGING_OUTBOX_CAPACITY].message_key == key) {
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_messaging_message_delete(solar_os_message_key_t key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_history_lock();
    if (messaging_message_pending_locked(key)) { messaging_history_unlock(); return ESP_ERR_INVALID_STATE; }
    messaging_message_slot_t *slot = messaging_history_cache_locked(key);
    if (!slot) { messaging_history_unlock(); return ESP_ERR_NOT_FOUND; }
    uint32_t inbox_id = slot->message.inbox_id;
    uint32_t provider = slot->message.provider, conversation = slot->message.conversation_id;
    error = messaging_history_tombstone_locked(slot->disk_index);
    if (error == ESP_OK) error = messaging_history_rebuild_locked();
    if (error == ESP_OK) {
        messaging_note_generation_locked();
        messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_MESSAGE_REMOVED,
            provider, conversation, key, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
    }
    messaging_history_unlock();
    if (error == ESP_OK && inbox_id && solar_os_inbox_matches_source_id(inbox_id, key)) {
        error = solar_os_inbox_delete(inbox_id);
        if (error == ESP_ERR_NOT_FOUND) error = ESP_OK;
    }
    return error;
}

static esp_err_t messaging_clear_inbox_projections(
    solar_os_messaging_provider_id_t provider)
{
    static const char *const gateway_sources[] = {"messages", "chat"};
    static const char *const meshcore_sources[] = {"meshcore"};
    static const char *const link_sources[] = {"link-chat", "link"};
    static const char *const all_sources[] = {
        "messages", "chat", "meshcore", "link-chat", "link",
    };
    const char *const *sources = all_sources;
    size_t source_count = sizeof(all_sources) / sizeof(all_sources[0]);
    if (provider == SOLAR_OS_MESSAGING_PROVIDER_GATEWAY) {
        sources = gateway_sources;
        source_count = sizeof(gateway_sources) / sizeof(gateway_sources[0]);
    } else if (provider == SOLAR_OS_MESSAGING_PROVIDER_MESHCORE) {
        sources = meshcore_sources;
        source_count = sizeof(meshcore_sources) / sizeof(meshcore_sources[0]);
    } else if (provider == SOLAR_OS_MESSAGING_PROVIDER_LINK) {
        sources = link_sources;
        source_count = sizeof(link_sources) / sizeof(link_sources[0]);
    }
    size_t deleted = 0;
    if (provider > SOLAR_OS_MESSAGING_PROVIDER_LINK) {
        char source[16]; snprintf(source, sizeof(source), "msg-%03" PRIu32, provider);
        const char *one[] = {source};
        return solar_os_inbox_delete_sources(one, 1, &deleted);
    }
    esp_err_t error = solar_os_inbox_delete_sources(sources, source_count, &deleted);
    if (provider == 0) {
        /* Sweep the complete persisted identity space, including profiles whose
         * removable media is currently absent. */
        for (uint32_t id = 4; id <= SOLAR_OS_MESSAGING_INSTANCE_ID_MAX; id++) {
            char source[16]; snprintf(source, sizeof(source), "msg-%03" PRIu32, id);
            const char *one[] = {source};
            esp_err_t next = solar_os_inbox_delete_sources(one, 1, &deleted);
            if (error == ESP_OK) error = next;
        }
    }
    return error;
}

esp_err_t solar_os_messaging_clear(solar_os_messaging_provider_id_t provider, size_t *removed)
{
    if (removed) *removed = 0;
    if (provider && !messaging_provider_valid(provider)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_history_lock();
    size_t count = 0;
    error = messaging_history_clear_locked(provider, 0, &count);
    if (count) {
        messaging_note_generation_locked();
        messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_MESSAGES_CLEARED,
            provider, 0, 0, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
    }
    messaging_history_unlock();
    if (removed) *removed = count;
    if (error == ESP_OK) error = messaging_clear_inbox_projections(provider);
    return error;
}

esp_err_t solar_os_messaging_mark_read(solar_os_conversation_id_t conversation_id)
{
    if (!conversation_id) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    uint32_t inbox_ids[SOLAR_OS_INBOX_CAPACITY];
    solar_os_message_key_t keys[SOLAR_OS_INBOX_CAPACITY];
    size_t linked = 0, changed = 0;
    messaging_history_lock();
    int c = messaging_conversation_id_index_locked(conversation_id);
    if (c < 0) { messaging_history_unlock(); return ESP_ERR_NOT_FOUND; }
    if (!messaging.conversations[c].unread_count) { messaging_history_unlock(); return ESP_OK; }
    messaging_history_navigation_t *navigation = messaging_history_navigation_locked(conversation_id, false);
    bool indexed = navigation && !navigation->unread_overflow;
    size_t limit = indexed ? navigation->unread_count : messaging.disk_head;
    FILE *file = messaging.persistent ? messaging_history_fopen(messaging.store_path, "r+b") : NULL;
    if (!file) { messaging_history_unlock(); return ESP_FAIL; }
    for (size_t i = 0; i < limit; i++) {
        size_t index = indexed ? (size_t)navigation->unread[i] : i;
        if (messaging_history_read_locked(file, index, messaging.record_scratch) < 0) { error = ESP_FAIL; break; }
        messaging_store_record_t *r = messaging.record_scratch;
        if (!r->message.key || !r->message.unread ||
            r->message.provider != messaging.conversations[c].provider ||
            strcmp(r->conversation_key, messaging.conversations[c].provider_key)) continue;
        r->message.unread = false;
        error = messaging_history_write_locked(file, index, r, false);
        if (error != ESP_OK) break;
        messaging_message_slot_t *slot = messaging_find_message_locked(r->message.key);
        if (slot) { slot->message.unread = false; slot->revision++; }
        if (r->message.inbox_id && linked < SOLAR_OS_INBOX_CAPACITY) {
            keys[linked] = r->message.key; inbox_ids[linked++] = r->message.inbox_id;
        }
        if (messaging.message_unread) messaging.message_unread--;
        if (messaging.conversations[c].unread_count) messaging.conversations[c].unread_count--;
        changed++;
    }
    if (messaging_history_fclose(file) && error == ESP_OK) error = ESP_FAIL;
    messaging.storage_error = error;
    if (error == ESP_OK && navigation) { navigation->unread_count = 0; navigation->unread_overflow = false; }
    if (changed) messaging_note_generation_locked();
    messaging_history_unlock();
    for (size_t i = 0; i < linked; i++) {
        if (solar_os_inbox_matches_source_id(inbox_ids[i], keys[i]))
            (void)solar_os_inbox_mark_read(inbox_ids[i], true);
    }
    return error;
}

esp_err_t solar_os_messaging_cancel(solar_os_message_key_t message_key)
{
    if (message_key == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_history_lock();
    messaging_message_slot_t *slot =
        messaging_history_cache_locked(message_key);
    if (slot == NULL || slot->message.direction != SOLAR_OS_MESSAGE_OUTBOUND ||
        (slot->message.delivery != SOLAR_OS_DELIVERY_QUEUED &&
         slot->message.delivery != SOLAR_OS_DELIVERY_FAILED)) {
        messaging_history_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    bool removed = false;
    const size_t oldest =
        (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
         messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        const size_t index =
            (oldest + i) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
        if (messaging.outbox[index].message_key == message_key) {
            *messaging.pending_slot = *slot;
            messaging.pending_slot->message.delivery = SOLAR_OS_DELIVERY_CANCELLED;
            error = messaging_history_commit_locked(messaging.pending_slot);
            if (error != ESP_OK) { messaging_history_unlock(); return error; }
            *slot = *messaging.pending_slot;
            for (size_t move = i; move + 1U < messaging.outbox_count; move++) {
                const size_t to =
                    (oldest + move) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
                const size_t from =
                    (oldest + move + 1U) %
                    SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
                messaging.outbox[to] = messaging.outbox[from];
            }
            messaging.outbox_head =
                (messaging.outbox_head +
                 SOLAR_OS_MESSAGING_OUTBOX_CAPACITY - 1U) %
                SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
            messaging.outbox_count--;
            removed = true;
            break;
        }
    }
    if (!removed) {
        messaging_history_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    slot->message.delivery = SOLAR_OS_DELIVERY_CANCELLED;
    slot->revision++;
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_DELIVERY,
                                    slot->message.provider,
                                    slot->message.conversation_id,
                                    message_key,
                                    SOLAR_OS_DELIVERY_CANCELLED,
                                    ESP_OK);
    messaging_history_unlock();
    return ESP_OK;
}

size_t solar_os_messaging_outbox_snapshot(
    solar_os_messaging_outbound_t *requests,
    size_t max_requests)
{
    if ((requests == NULL && max_requests != 0U) ||
        solar_os_messaging_init() != ESP_OK) {
        return 0U;
    }
    messaging_lock();
    const size_t count = messaging.outbox_count < max_requests ?
        messaging.outbox_count : max_requests;
    const size_t oldest =
        (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
         messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    for (size_t i = 0; i < count; i++) {
        requests[i] = messaging.outbox[
            (oldest + i) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY];
    }
    messaging_unlock();
    return count;
}

esp_err_t solar_os_messaging_outbox_peek(
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_outbound_t *request)
{
    if (!messaging_provider_valid(provider) || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_lock();
    messaging_provider_node_t *provider_node = messaging_provider_node_locked(provider);
    if (provider_node && (provider_node->status.removed || provider_node->removing)) {
        messaging_unlock(); return ESP_ERR_NOT_FOUND;
    }
    const size_t oldest =
        (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
         messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    bool found = false;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        const solar_os_messaging_outbound_t *candidate =
            &messaging.outbox[(oldest + i) %
                              SOLAR_OS_MESSAGING_OUTBOX_CAPACITY];
        if (candidate->provider == provider) {
            *request = *candidate;
            found = true;
            break;
        }
    }
    messaging_unlock();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_messaging_outbox_update(
    uint32_t request_id,
    solar_os_delivery_state_t state,
    const char *error)
{
    if (request_id == 0 ||
        (state != SOLAR_OS_DELIVERY_QUEUED &&
         state != SOLAR_OS_DELIVERY_SENDING &&
         state != SOLAR_OS_DELIVERY_SENT &&
         state != SOLAR_OS_DELIVERY_DELIVERED &&
         state != SOLAR_OS_DELIVERY_FAILED)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t init_error = solar_os_messaging_init();
    if (init_error != ESP_OK) {
        return init_error;
    }
    messaging_history_lock();
    const size_t oldest =
        (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
         messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    size_t found_offset = SIZE_MAX;
    solar_os_message_key_t key = 0;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        solar_os_messaging_outbound_t *candidate =
            &messaging.outbox[(oldest + i) %
                              SOLAR_OS_MESSAGING_OUTBOX_CAPACITY];
        if (candidate->id == request_id) {
            messaging_provider_node_t *node = messaging_provider_node_locked(candidate->provider);
            if (state == SOLAR_OS_DELIVERY_SENDING && node && (node->status.removed || node->removing)) {
                messaging_history_unlock(); return ESP_ERR_INVALID_STATE;
            }
            found_offset = i;
            key = candidate->message_key;
            if (state == SOLAR_OS_DELIVERY_QUEUED) {
                candidate->attempts++;
            }
            break;
        }
    }
    if (found_offset == SIZE_MAX) {
        messaging_history_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    messaging_message_slot_t *slot = messaging_history_cache_locked(key);
    if (slot == NULL) {
        messaging_history_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *messaging.pending_slot = *slot;
    messaging.pending_slot->message.delivery = state;
    strlcpy(messaging.pending_slot->message.error,
            error != NULL ? error : "",
            sizeof(messaging.pending_slot->message.error));
    esp_err_t saved = messaging_history_commit_locked(messaging.pending_slot);
    if (saved != ESP_OK) { messaging_history_unlock(); return saved; }
    *slot = *messaging.pending_slot;
    slot->revision++;
    if (state == SOLAR_OS_DELIVERY_SENT ||
        state == SOLAR_OS_DELIVERY_DELIVERED ||
        state == SOLAR_OS_DELIVERY_FAILED) {
        for (size_t move = found_offset;
             move + 1U < messaging.outbox_count;
             move++) {
            const size_t to =
                (oldest + move) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
            const size_t from =
                (oldest + move + 1U) %
                SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
            messaging.outbox[to] = messaging.outbox[from];
        }
        messaging.outbox_head =
            (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY - 1U) %
            SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
        messaging.outbox_count--;
    }
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_DELIVERY,
                                    slot->message.provider,
                                    slot->message.conversation_id,
                                    key,
                                    state,
                                    state == SOLAR_OS_DELIVERY_FAILED ?
                                        ESP_FAIL : ESP_OK);
    messaging_history_unlock();
    return ESP_OK;
}

esp_err_t solar_os_messaging_read_event_after(uint32_t *cursor,
                                              solar_os_messaging_event_t *event)
{
    if (cursor == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_lock();
    const size_t oldest =
        (messaging.event_head + SOLAR_OS_MESSAGING_EVENT_CAPACITY -
         messaging.event_count) % SOLAR_OS_MESSAGING_EVENT_CAPACITY;
    bool found = false;
    for (size_t i = 0; i < messaging.event_count; i++) {
        const solar_os_messaging_event_t *candidate =
            &messaging.events[(oldest + i) %
                              SOLAR_OS_MESSAGING_EVENT_CAPACITY];
        if ((int32_t)(candidate->id - *cursor) > 0) {
            *event = *candidate;
            *cursor = candidate->id;
            found = true;
            break;
        }
    }
    messaging_unlock();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

uint64_t solar_os_messaging_provider_context(
    solar_os_messaging_provider_id_t provider)
{
    if (!messaging_provider_valid(provider)) {
        return 0;
    }
    uint64_t hash = 1469598103934665603ULL;
    hash = messaging_hash_update(hash, &provider, sizeof(provider));
    const char *name = solar_os_messaging_provider_name(provider);
    hash = messaging_hash_string(hash, name);
    return hash != 0 ? hash : 1U;
}

/* Caller owns io_lock. Retry this even for an already retired instance so a
 * previous cleanup error cannot leave an old credential-bearing backup. */
static esp_err_t messaging_archive_cleanup(const solar_os_messaging_provider_status_t *status)
{
    char active[SOLAR_OS_STORAGE_PATH_MAX], path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = messaging_profile_path(status->id, active, sizeof(active));
    const char *suffixes[] = {".bak", ".tmp"};
    for (size_t i = 0; error == ESP_OK && i < 2; i++) {
        if (snprintf(path, sizeof(path), "%s%s", active, suffixes[i]) >= (int)sizeof(path)) error = ESP_ERR_INVALID_SIZE;
        else if (unlink(path) != 0 && errno != ENOENT) error = ESP_FAIL;
    }
    return error;
}

static esp_err_t messaging_archive_profile(const solar_os_messaging_provider_status_t *status,
    bool *committed)
{
    const unsigned char empty = 0;
    xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    esp_err_t error = messaging_profile_write(status, &empty, 0);
    *committed = error == ESP_OK;
    if (*committed) error = messaging_archive_cleanup(status);
    xSemaphoreGive(messaging.io_lock); return error;
}

static void messaging_archive_runtime(messaging_provider_node_t *node)
{
    /* Transport work has stopped. Drop even requests whose history slot has
     * been evicted, and keep every other instance's queue in the same order. */
    messaging_history_lock();
    const solar_os_messaging_provider_id_t id = node->status.id;
    const size_t oldest = (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
        messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    size_t kept = 0;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        solar_os_messaging_outbound_t *entry = &messaging.outbox[(oldest + i) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY];
        if (entry->provider != id) messaging.outbox[(oldest + kept++) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY] = *entry;
    }
    for (size_t i = kept; i < messaging.outbox_count; i++)
        memset(&messaging.outbox[(oldest + i) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY], 0, sizeof(*messaging.outbox));
    messaging.outbox_count = kept; messaging.outbox_head = (oldest + kept) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    for (size_t i = 0; i < messaging.message_count; i++) {
        messaging_message_slot_t *slot = &messaging.messages[(messaging_message_oldest_locked() + i) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
        if (slot->message.provider != id || slot->message.direction != SOLAR_OS_MESSAGE_OUTBOUND ||
            (slot->message.delivery != SOLAR_OS_DELIVERY_QUEUED && slot->message.delivery != SOLAR_OS_DELIVERY_SENDING)) continue;
        slot->message.delivery = slot->message.delivery == SOLAR_OS_DELIVERY_SENDING ?
            SOLAR_OS_DELIVERY_FAILED : SOLAR_OS_DELIVERY_CANCELLED;
        strlcpy(slot->message.error, "instance removed", sizeof(slot->message.error)); slot->revision++;
        messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_DELIVERY, id, slot->message.conversation_id,
            slot->message.key, slot->message.delivery, ESP_OK);
    }
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        solar_os_messaging_conversation_t *conversation = &messaging.conversations[i];
        if (conversation->provider != id) continue;
        conversation->history_only = true; conversation->join_state = SOLAR_OS_MESSAGING_JOIN_LEFT;
        strlcpy(conversation->join_error, "instance removed", sizeof(conversation->join_error));
    }
    node->status.removed = true; node->status.running = node->status.connected = false;
    node->status.last_error = ESP_OK; node->context = NULL; node->removing = false;
    strlcpy(node->status.detail, "removed", sizeof(node->status.detail));
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_PROVIDER, id, 0, 0, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
    messaging_history_unlock();
}

static esp_err_t messaging_remove_history(solar_os_messaging_provider_id_t id)
{
    esp_err_t error = solar_os_messaging_clear(id, NULL);
#if SOLAR_OS_PACKAGE_SERVICE_CREDENTIALS
    esp_err_t credentials_error = solar_os_credentials_remove_provider(id);
    if (error == ESP_OK) error = credentials_error;
#endif
    messaging_lock();
    for (size_t i = 0; i < messaging.conversation_count;) {
        solar_os_messaging_conversation_t *conversation = &messaging.conversations[i];
        if (conversation->provider != id) { i++; continue; }
        messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_CONVERSATION_REMOVED, id,
            conversation->id, 0, SOLAR_OS_DELIVERY_RECEIVED, ESP_OK);
        memmove(conversation, conversation + 1,
            (messaging.conversation_count - i - 1) * sizeof(*conversation));
        messaging.conversation_count--;
    }
    messaging_note_generation_locked();
    messaging_unlock();
    return error;
}

esp_err_t solar_os_messaging_instance_remove(solar_os_messaging_provider_id_t id)
{
    if (!messaging_provider_valid(id)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    messaging_lock();
    messaging_provider_node_t *node = messaging_provider_node_locked(id);
    if (!node) { messaging_unlock(); return ESP_ERR_NOT_FOUND; }
    if (node->status.removed) {
        messaging_unlock();
        xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
        error = messaging_archive_cleanup(&node->status);
        xSemaphoreGive(messaging.io_lock);
        esp_err_t purge = messaging_remove_history(id);
        return error != ESP_OK ? error : purge;
    }
    if (node->removing || node->operations) { messaging_unlock(); return ESP_ERR_INVALID_STATE; }
    const solar_os_messaging_provider_class_t *type = messaging_class_locked(node->status.class_name);
    void *context = node->context;
    if (id <= SOLAR_OS_MESSAGING_PROVIDER_LINK &&
        (!context || !type || !type->remove_prepare || !type->remove_finish)) {
        messaging_unlock(); return ESP_ERR_NOT_SUPPORTED;
    }
    if (context && (!type || !type->remove_prepare || !type->remove_finish)) {
        messaging_unlock(); return ESP_ERR_NOT_SUPPORTED;
    }
    node->removing = true;
    messaging_unlock();
    if (context) error = type->remove_prepare(context);
    bool committed = false;
    if (error == ESP_OK) {
        error = messaging_archive_profile(&node->status, &committed);
        if (context) type->remove_finish(context, committed);
    }
    if (committed) {
        messaging_archive_runtime(node);
        esp_err_t purge = messaging_remove_history(id);
        if (error == ESP_OK) error = purge;
    }
    else { messaging_lock(); node->removing = false; messaging_unlock(); }
    return error;
}
