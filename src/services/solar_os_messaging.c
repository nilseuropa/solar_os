#include "solar_os_messaging.h"

#include <errno.h>
#include <inttypes.h>
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

#define MESSAGING_STORE_MAGIC 0x47534d53UL
#define MESSAGING_STORE_VERSION 1U
#define MESSAGING_STORE_HEADER_COPIES 2U
#define MESSAGING_STORE_RESERVED_BYTES (36U * 1024U)
#define MESSAGING_LEGACY_STORE ".chat/messages.bin"

typedef struct {
    solar_os_messaging_message_t message;
    int16_t disk_index;
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
#define MESSAGING_STORE_MAX_BYTES \
    (MESSAGING_STORE_RECORDS_OFFSET + \
     SOLAR_OS_MESSAGING_MESSAGE_CAPACITY * sizeof(messaging_store_record_t))

_Static_assert(MESSAGING_STORE_MAX_BYTES <= 384U * 1024U,
               "persistent messaging history must remain bounded");

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
    solar_os_messaging_provider_status_t providers[
        SOLAR_OS_MESSAGING_PROVIDER_CAPACITY];
    messaging_store_record_t *record_scratch;
    uint32_t next_conversation_id;
    uint32_t next_outbox_id;
    uint32_t next_event_id;
    uint64_t next_message_key;
    uint32_t generation;
    uint32_t dropped_messages;
    uint32_t dropped_outbox;
    uint32_t dropped_events;
    bool persistent;
    bool inbox_backed;
    size_t disk_capacity;
    size_t disk_head;
    size_t disk_count;
    uint32_t disk_generation;
    size_t inbox_capacity;
    size_t inbox_limit_bytes;
    esp_err_t storage_error;
    char store_path[SOLAR_OS_STORAGE_PATH_MAX];
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
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
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; bit++) {
            crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
        }
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
    return provider >= SOLAR_OS_MESSAGING_PROVIDER_GATEWAY &&
        provider <= SOLAR_OS_MESSAGING_PROVIDER_LINK;
}

static size_t messaging_provider_index(
    solar_os_messaging_provider_id_t provider)
{
    return (size_t)provider - 1U;
}

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

static esp_err_t messaging_sync_file(FILE *file)
{
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

static void messaging_make_header_locked(messaging_store_header_t *header,
                                         uint32_t generation)
{
    memset(header, 0, sizeof(*header));
    header->magic = MESSAGING_STORE_MAGIC;
    header->version = MESSAGING_STORE_VERSION;
    header->capacity = (uint16_t)messaging.disk_capacity;
    header->generation = generation;
    header->head = (uint32_t)messaging.disk_head;
    header->count = (uint32_t)messaging.disk_count;
    header->dropped = messaging.dropped_messages;
    header->next_message_key = messaging.next_message_key;
    header->crc32 =
        messaging_crc32(header, offsetof(messaging_store_header_t, crc32));
}

static bool messaging_header_valid(const messaging_store_header_t *header)
{
    return header->magic == MESSAGING_STORE_MAGIC &&
        header->version == MESSAGING_STORE_VERSION &&
        header->capacity > 0 &&
        header->capacity <= SOLAR_OS_MESSAGING_MESSAGE_CAPACITY &&
        header->head < header->capacity &&
        header->count <= header->capacity &&
        header->next_message_key != 0 &&
        header->crc32 ==
            messaging_crc32(header,
                            offsetof(messaging_store_header_t, crc32));
}

static bool messaging_generation_newer(uint32_t first, uint32_t second)
{
    return (int32_t)(first - second) > 0;
}

static void messaging_make_record_locked(
    messaging_store_record_t *record,
    const messaging_message_slot_t *slot)
{
    memset(record, 0, sizeof(*record));
    record->message = slot->message;
    const int conversation_index =
        messaging_conversation_id_index_locked(slot->message.conversation_id);
    if (conversation_index >= 0) {
        const solar_os_messaging_conversation_t *conversation =
            &messaging.conversations[conversation_index];
        strlcpy(record->conversation_key,
                conversation->provider_key,
                sizeof(record->conversation_key));
        strlcpy(record->conversation_title,
                conversation->title,
                sizeof(record->conversation_title));
        record->conversation_kind = conversation->kind;
        record->group_ref = conversation->group_ref;
        record->conversation_security = conversation->security_flags;
    }
    record->crc32 =
        messaging_crc32(record, offsetof(messaging_store_record_t, crc32));
}

static bool messaging_record_valid(const messaging_store_record_t *record)
{
    if (record->crc32 !=
        messaging_crc32(record,
                        offsetof(messaging_store_record_t, crc32))) {
        return false;
    }
    if (record->message.key == 0) {
        return true;
    }
    return record->message.provider >= SOLAR_OS_MESSAGING_PROVIDER_GATEWAY &&
        record->message.provider <= SOLAR_OS_MESSAGING_PROVIDER_LINK &&
        record->conversation_key[0] != '\0';
}

static esp_err_t messaging_prepare_store_path(void)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!solar_os_storage_sd_is_mounted()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error =
        solar_os_storage_default_path(SOLAR_OS_MESSAGING_STORE_DIR,
                                      directory,
                                      sizeof(directory));
    if (error != ESP_OK) {
        return error;
    }
    if (solar_os_storage_mkdir(directory) != ESP_OK && errno != EEXIST) {
        return ESP_FAIL;
    }
    return solar_os_storage_default_path(
        SOLAR_OS_MESSAGING_STORE_DIR "/" SOLAR_OS_MESSAGING_STORE_FILE,
        messaging.store_path,
        sizeof(messaging.store_path));
}

static size_t messaging_select_disk_capacity(uint64_t reclaimed_bytes)
{
    solar_os_storage_usage_t usage;
    if (solar_os_storage_get_usage(&usage) != ESP_OK) {
        return 0;
    }
    const uint64_t available = usage.free_bytes + reclaimed_bytes;
    if (available <=
        MESSAGING_STORE_RESERVED_BYTES + MESSAGING_STORE_RECORDS_OFFSET) {
        return 0;
    }
    uint64_t bytes = available - MESSAGING_STORE_RESERVED_BYTES -
        MESSAGING_STORE_RECORDS_OFFSET;
    size_t capacity = (size_t)(bytes / sizeof(messaging_store_record_t));
    if (capacity > SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
        capacity = SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    }
    return capacity;
}

static esp_err_t messaging_store_reset(void)
{
    if (messaging.disk_capacity == 0) {
        messaging.storage_error = ESP_ERR_NO_MEM;
        messaging.persistent = false;
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(messaging.store_path, "w+b");
    if (file == NULL) {
        messaging.storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    messaging_lock();
    uint32_t generation = messaging.disk_generation + 1U;
    if (generation == 0) {
        generation = 1U;
    }
    messaging.disk_head = 0;
    messaging.disk_count = 0;
    messaging_store_header_t header;
    messaging_make_header_locked(&header, generation);
    messaging_unlock();
    bool ok = true;
    for (size_t i = 0; i < MESSAGING_STORE_HEADER_COPIES; i++) {
        if (fwrite(&header, sizeof(header), 1, file) != 1) {
            ok = false;
            break;
        }
    }
    esp_err_t error = ok ? messaging_sync_file(file) : ESP_FAIL;
    if (fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    messaging_lock();
    if (error == ESP_OK) {
        messaging.disk_generation = generation;
        messaging.persistent = true;
        messaging.storage_error = ESP_OK;
    } else {
        messaging.persistent = false;
        messaging.storage_error = error;
    }
    messaging_unlock();
    return error;
}

static void messaging_clear_disk_mapping_locked(size_t disk_index)
{
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY; i++) {
        if (messaging.messages[i].disk_index == (int16_t)disk_index) {
            messaging.messages[i].disk_index = -1;
        }
    }
}

static esp_err_t messaging_store_write(solar_os_message_key_t key)
{
    (void)xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    messaging_lock();
    messaging_message_slot_t *slot = messaging_find_message_locked(key);
    if (slot == NULL || !messaging.persistent ||
        messaging.disk_capacity == 0) {
        messaging_unlock();
        xSemaphoreGive(messaging.io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t disk_index = messaging.disk_head;
    messaging_make_record_locked(messaging.record_scratch, slot);
    messaging.disk_head =
        (messaging.disk_head + 1U) % messaging.disk_capacity;
    if (messaging.disk_count < messaging.disk_capacity) {
        messaging.disk_count++;
    }
    uint32_t generation = messaging.disk_generation + 1U;
    if (generation == 0) {
        generation = 1U;
    }
    messaging_store_header_t header;
    messaging_make_header_locked(&header, generation);
    messaging_unlock();

    FILE *file = fopen(messaging.store_path, "r+b");
    esp_err_t error = file != NULL ? ESP_OK : ESP_FAIL;
    const long record_offset =
        (long)(MESSAGING_STORE_RECORDS_OFFSET +
               disk_index * sizeof(*messaging.record_scratch));
    if (error == ESP_OK &&
        (fseek(file, record_offset, SEEK_SET) != 0 ||
         fwrite(messaging.record_scratch,
                sizeof(*messaging.record_scratch),
                1,
                file) != 1 ||
         messaging_sync_file(file) != ESP_OK)) {
        error = ESP_FAIL;
    }
    const long header_offset =
        (long)((generation % MESSAGING_STORE_HEADER_COPIES) * sizeof(header));
    if (error == ESP_OK &&
        (fseek(file, header_offset, SEEK_SET) != 0 ||
         fwrite(&header, sizeof(header), 1, file) != 1 ||
         messaging_sync_file(file) != ESP_OK)) {
        error = ESP_FAIL;
    }
    if (file != NULL && fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }

    messaging_lock();
    slot = messaging_find_message_locked(key);
    if (error == ESP_OK) {
        messaging_clear_disk_mapping_locked(disk_index);
        if (slot != NULL) {
            slot->disk_index = (int16_t)disk_index;
        }
        messaging.disk_generation = generation;
        messaging.storage_error = ESP_OK;
    } else {
        messaging.storage_error = error;
    }
    messaging_unlock();
    xSemaphoreGive(messaging.io_lock);
    return error;
}

static esp_err_t messaging_store_update(solar_os_message_key_t key)
{
    (void)xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    messaging_lock();
    messaging_message_slot_t *slot = messaging_find_message_locked(key);
    if (slot == NULL || !messaging.persistent || slot->disk_index < 0 ||
        (size_t)slot->disk_index >= messaging.disk_capacity) {
        messaging_unlock();
        xSemaphoreGive(messaging.io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t disk_index = (size_t)slot->disk_index;
    messaging_make_record_locked(messaging.record_scratch, slot);
    messaging_unlock();

    FILE *file = fopen(messaging.store_path, "r+b");
    esp_err_t error = file != NULL ? ESP_OK : ESP_FAIL;
    const long offset = (long)(MESSAGING_STORE_RECORDS_OFFSET +
                               disk_index *
                                   sizeof(*messaging.record_scratch));
    if (error == ESP_OK &&
        (fseek(file, offset, SEEK_SET) != 0 ||
         fwrite(messaging.record_scratch,
                sizeof(*messaging.record_scratch),
                1,
                file) != 1 ||
         messaging_sync_file(file) != ESP_OK)) {
        error = ESP_FAIL;
    }
    if (file != NULL && fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    messaging_lock();
    messaging.storage_error = error;
    messaging_unlock();
    xSemaphoreGive(messaging.io_lock);
    return error;
}

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

static esp_err_t messaging_store_tombstone_many(const int16_t *disk_indexes,
                                                size_t count)
{
    if (count == 0 || !messaging.persistent) {
        return ESP_OK;
    }
    (void)xSemaphoreTake(messaging.io_lock, portMAX_DELAY);
    FILE *file = fopen(messaging.store_path, "r+b");
    esp_err_t error = file != NULL ? ESP_OK : ESP_FAIL;
    for (size_t i = 0; error == ESP_OK && i < count; i++) {
        if (disk_indexes[i] < 0 ||
            (size_t)disk_indexes[i] >= messaging.disk_capacity) {
            continue;
        }
        memset(messaging.record_scratch, 0, sizeof(*messaging.record_scratch));
        messaging.record_scratch->crc32 = messaging_crc32(
            messaging.record_scratch,
            offsetof(messaging_store_record_t, crc32));
        const long offset = (long)(MESSAGING_STORE_RECORDS_OFFSET +
                                   (size_t)disk_indexes[i] *
                                       sizeof(*messaging.record_scratch));
        if (fseek(file, offset, SEEK_SET) != 0 ||
            fwrite(messaging.record_scratch,
                   sizeof(*messaging.record_scratch),
                   1,
                   file) != 1) {
            error = ESP_FAIL;
        }
    }
    if (error == ESP_OK) {
        error = messaging_sync_file(file);
    }
    if (file != NULL && fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    messaging_lock();
    if (error == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            if (disk_indexes[i] >= 0) {
                messaging_clear_disk_mapping_locked(
                    (size_t)disk_indexes[i]);
            }
        }
    }
    messaging.storage_error = error;
    messaging_unlock();
    xSemaphoreGive(messaging.io_lock);
    return error;
}

static void messaging_restore_record_locked(
    const messaging_store_record_t *record,
    int16_t disk_index)
{
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
    if (messaging.message_count == SOLAR_OS_MESSAGING_MESSAGE_CAPACITY &&
        slot->message.unread && messaging.message_unread > 0) {
        messaging.message_unread--;
    }
    memset(slot, 0, sizeof(*slot));
    slot->message = record->message;
    slot->message.conversation_id = conversation_id;
    slot->disk_index = disk_index;
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

static esp_err_t messaging_store_load(void)
{
    esp_err_t error = messaging_prepare_store_path();
    if (error != ESP_OK) {
        messaging.storage_error = error;
        return error;
    }
    struct stat info;
    if (stat(messaging.store_path, &info) != 0) {
        if (errno != ENOENT) {
            messaging.storage_error = ESP_FAIL;
            return ESP_FAIL;
        }
        messaging.disk_capacity = messaging_select_disk_capacity(0);
        return messaging_store_reset();
    }
    if (info.st_size < (off_t)MESSAGING_STORE_RECORDS_OFFSET ||
        info.st_size > (off_t)MESSAGING_STORE_MAX_BYTES) {
        messaging.disk_capacity =
            messaging_select_disk_capacity((uint64_t)info.st_size);
        return messaging_store_reset();
    }
    FILE *file = fopen(messaging.store_path, "rb");
    if (file == NULL) {
        messaging.storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    messaging_store_header_t headers[MESSAGING_STORE_HEADER_COPIES];
    const bool read_ok = fread(headers, sizeof(headers), 1, file) == 1;
    const bool first_valid = read_ok && messaging_header_valid(&headers[0]);
    const bool second_valid = read_ok && messaging_header_valid(&headers[1]);
    if (!first_valid && !second_valid) {
        fclose(file);
        messaging.disk_capacity =
            messaging_select_disk_capacity((uint64_t)info.st_size);
        return messaging_store_reset();
    }
    const messaging_store_header_t *header = &headers[0];
    if (!first_valid ||
        (second_valid &&
         messaging_generation_newer(headers[1].generation,
                                    headers[0].generation))) {
        header = &headers[1];
    }
    messaging.disk_capacity = header->capacity;
    messaging.disk_head = header->head;
    messaging.disk_count = header->count;
    messaging.disk_generation = header->generation;
    messaging.dropped_messages = header->dropped;
    messaging.next_message_key = header->next_message_key;
    const size_t oldest =
        (header->head + header->capacity - header->count) % header->capacity;
    bool records_valid = true;
    for (size_t i = 0; i < header->count; i++) {
        const size_t disk_index = (oldest + i) % header->capacity;
        const long offset =
            (long)(MESSAGING_STORE_RECORDS_OFFSET +
                   disk_index * sizeof(*messaging.record_scratch));
        if (fseek(file, offset, SEEK_SET) != 0 ||
            fread(messaging.record_scratch,
                  sizeof(*messaging.record_scratch),
                  1,
                  file) != 1 ||
            !messaging_record_valid(messaging.record_scratch)) {
            records_valid = false;
            break;
        }
        if (messaging.record_scratch->message.key != 0) {
            messaging_lock();
            messaging_restore_record_locked(messaging.record_scratch,
                                            (int16_t)disk_index);
            messaging_unlock();
        }
    }
    fclose(file);
    if (!records_valid) {
        messaging_lock();
        memset(messaging.messages,
               0,
               SOLAR_OS_MESSAGING_MESSAGE_CAPACITY *
                   sizeof(*messaging.messages));
        messaging.message_head = 0;
        messaging.message_count = 0;
        messaging.message_unread = 0;
        messaging.conversation_count = 0;
        messaging.next_message_key = 1U;
        messaging_unlock();
        return messaging_store_reset();
    }
    messaging.persistent = true;
    messaging.storage_error = ESP_OK;
    return ESP_OK;
}

static void messaging_remove_legacy_store(void)
{
    char legacy_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(MESSAGING_LEGACY_STORE,
                                      legacy_path,
                                      sizeof(legacy_path)) != ESP_OK) {
        return;
    }
    struct stat info;
    if (stat(legacy_path, &info) == 0) {
        const esp_err_t error = solar_os_storage_remove(legacy_path);
        if (error != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "obsolete Chat history removal failed: %s",
                          esp_err_to_name(error));
        }
    }
}

static void messaging_restore_inbox(void)
{
    solar_os_inbox_status_t status;
    if (solar_os_inbox_get_status(&status) != ESP_OK || !status.persistent) {
        return;
    }
    solar_os_inbox_entry_t *entries = solar_os_memory_calloc(
        SOLAR_OS_INBOX_CAPACITY,
        sizeof(*entries),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.inbox.restore");
    if (entries == NULL) {
        return;
    }
    const size_t count = solar_os_inbox_snapshot(entries,
                                                 SOLAR_OS_INBOX_CAPACITY,
                                                 false,
                                                 NULL);
    messaging_lock();
    for (size_t reverse = count; reverse > 0; reverse--) {
        const solar_os_inbox_entry_t *entry = &entries[reverse - 1U];
        solar_os_messaging_provider_id_t provider;
        if (strcmp(entry->source, "chat") == 0 ||
            strcmp(entry->source, "messages") == 0) {
            provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY;
        } else if (strcmp(entry->source, "meshcore") == 0) {
            provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE;
        } else if (strcmp(entry->source, "link-chat") == 0) {
            provider = SOLAR_OS_MESSAGING_PROVIDER_LINK;
        } else {
            continue;
        }
        if (entry->source_id == 0 ||
            messaging_find_message_locked(entry->source_id) != NULL) {
            continue;
        }
        const char *provider_key =
            entry->topic[0] != '\0' ? entry->topic : "general";
        solar_os_messaging_conversation_upsert_t conversation_request = {
            .provider = provider,
            .provider_key = provider_key,
            .kind =
                provider == SOLAR_OS_MESSAGING_PROVIDER_GATEWAY ?
                    SOLAR_OS_CONVERSATION_ROOM :
                provider == SOLAR_OS_MESSAGING_PROVIDER_LINK &&
                    strncmp(provider_key, "b:", 2U) == 0 ?
                    SOLAR_OS_CONVERSATION_BROADCAST :
                    SOLAR_OS_CONVERSATION_DIRECT,
            .title = provider_key,
        };
        const solar_os_conversation_id_t conversation_id =
            messaging_upsert_conversation_locked(&conversation_request, false);
        if (conversation_id == SOLAR_OS_CONVERSATION_ID_NONE) {
            continue;
        }
        messaging_message_slot_t *slot =
            &messaging.messages[messaging.message_head];
        if (messaging.message_count == SOLAR_OS_MESSAGING_MESSAGE_CAPACITY &&
            slot->message.unread && messaging.message_unread > 0) {
            messaging.message_unread--;
        }
        memset(slot, 0, sizeof(*slot));
        slot->disk_index = -1;
        slot->revision = messaging.generation;
        slot->message.key = entry->source_id;
        slot->message.conversation_id = conversation_id;
        slot->message.provider = provider;
        slot->message.direction = SOLAR_OS_MESSAGE_INBOUND;
        slot->message.delivery = SOLAR_OS_DELIVERY_RECEIVED;
        slot->message.inbox_id = entry->id;
        slot->message.unread = entry->unread;
        slot->message.truncated = entry->truncated;
        slot->message.received_ms = entry->received_ms;
        strlcpy(slot->message.sender,
                entry->sender,
                sizeof(slot->message.sender));
        strlcpy(slot->message.body, entry->body, sizeof(slot->message.body));
        messaging.message_head =
            (messaging.message_head + 1U) %
            SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
        if (messaging.message_count < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
            messaging.message_count++;
        }
        if (slot->message.unread) {
            messaging.message_unread++;
            const int conversation_index =
                messaging_conversation_id_index_locked(conversation_id);
            if (conversation_index >= 0) {
                messaging.conversations[conversation_index].unread_count++;
            }
        }
    }
    messaging.inbox_backed = true;
    messaging.inbox_capacity = status.capacity;
    messaging.inbox_limit_bytes = status.storage_limit_bytes;
    messaging.storage_error = status.storage_error;
    messaging_note_generation_locked();
    messaging_unlock();
    solar_os_memory_free(entries);
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
    messaging_lock();
    messaging_message_slot_t *slot =
        messaging_find_message_locked(message_key);
    const bool linked =
        slot != NULL && slot->revision == revision && slot->message.unread &&
        projection_present;
    if (linked) {
        slot->message.inbox_id = inbox_id;
        slot->revision++;
    }
    messaging_unlock();
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
    if (messaging.lock == NULL || messaging.io_lock == NULL) {
        if (messaging.lock != NULL) {
            vSemaphoreDelete(messaging.lock);
        }
        if (messaging.io_lock != NULL) {
            vSemaphoreDelete(messaging.io_lock);
        }
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
    if (messaging.conversations == NULL || messaging.messages == NULL ||
        messaging.outbox == NULL || messaging.events == NULL ||
        messaging.record_scratch == NULL) {
        solar_os_memory_free(messaging.conversations);
        solar_os_memory_free(messaging.messages);
        solar_os_memory_free(messaging.outbox);
        solar_os_memory_free(messaging.events);
        solar_os_memory_free(messaging.record_scratch);
        vSemaphoreDelete(messaging.lock);
        vSemaphoreDelete(messaging.io_lock);
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
    messaging.generation = 1U;
    messaging.initialized = true;
    const esp_err_t store_error = messaging_store_load();
    if (store_error == ESP_OK) {
        messaging_remove_legacy_store();
    } else {
        messaging_restore_inbox();
    }
    solar_os_inbox_set_clear_observer(messaging_unlink_all_inbox_projections,
                                      NULL);
    messaging_reconcile_inbox_projections();
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
    messaging_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = messaging.initialized;
    status->rings_in_psram = true;
    status->persistent = messaging.persistent || messaging.inbox_backed;
    status->inbox_backed = messaging.inbox_backed;
    status->conversations = messaging.conversation_count;
    status->messages = messaging.message_count;
    status->unread = messaging.message_unread;
    status->queued_outbox = messaging.outbox_count;
    status->queued_events = messaging.event_count;
    status->persistent_capacity = messaging.persistent ?
        messaging.disk_capacity : messaging.inbox_capacity;
    status->persistent_limit_bytes = messaging.persistent ?
        MESSAGING_STORE_MAX_BYTES : messaging.inbox_limit_bytes;
    status->dropped_messages = messaging.dropped_messages;
    status->dropped_outbox = messaging.dropped_outbox;
    status->generation = messaging.generation;
    status->storage_error = messaging.storage_error;
    messaging_unlock();
    return ESP_OK;
}

esp_err_t solar_os_messaging_provider_register(
    solar_os_messaging_provider_id_t provider,
    const char *name)
{
    if (!messaging_provider_valid(provider) ||
        !messaging_text_valid(name, SOLAR_OS_MESSAGING_LABEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_lock();
    solar_os_messaging_provider_status_t *status =
        &messaging.providers[messaging_provider_index(provider)];
    memset(status, 0, sizeof(*status));
    status->id = provider;
    status->registered = true;
    status->last_error = ESP_OK;
    strlcpy(status->name, name, sizeof(status->name));
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
    solar_os_messaging_provider_status_t *status =
        &messaging.providers[messaging_provider_index(provider)];
    if (!status->registered) {
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
    *status = messaging.providers[messaging_provider_index(provider)];
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
    solar_os_message_key_t message_keys[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    size_t message_count = 0;
    messaging_lock();
    int index = messaging_conversation_index_locked(provider, provider_key);
    if (index < 0) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_conversation_id_t id = messaging.conversations[index].id;
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        const solar_os_messaging_message_t *message =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY].message;
        if (message->conversation_id == id) {
            message_keys[message_count++] = message->key;
        }
    }
    messaging_unlock();
    for (size_t i = 0; i < message_count; i++) {
        error = solar_os_messaging_message_delete(message_keys[i]);
        if (error != ESP_OK) {
            return error;
        }
    }

    messaging_lock();
    index = messaging_conversation_index_locked(provider, provider_key);
    if (index < 0) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
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
    messaging_unlock();
    return ESP_OK;
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
    messaging_lock();
    const int index =
        messaging_conversation_id_index_locked(conversation_id);
    if (index >= 0) {
        *conversation = messaging.conversations[index];
    }
    messaging_unlock();
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
    if (selected->provider == SOLAR_OS_MESSAGING_PROVIDER_MESHCORE) {
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
    messaging_lock();
    const size_t count = messaging.conversation_count < max_conversations ?
        messaging.conversation_count : max_conversations;
    memcpy(conversations,
           messaging.conversations,
           count * sizeof(*conversations));
    messaging_unlock();
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
    messaging_lock();
    const solar_os_message_key_t key =
        messaging_inbound_key_locked(request);
    if (messaging_find_message_locked(key) != NULL) {
        messaging_unlock();
        if (message_key != NULL) {
            *message_key = key;
        }
        return ESP_OK;
    }
    if (request->endpoint_id != 0) {
        solar_os_endpoint_t endpoint;
        messaging_unlock();
        error = solar_os_contacts_get_endpoint(request->endpoint_id, &endpoint);
        if (error != ESP_OK) {
            return error;
        }
        if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) {
            return ESP_ERR_INVALID_STATE;
        }
        messaging_lock();
        if (messaging_find_message_locked(key) != NULL) {
            messaging_unlock();
            if (message_key != NULL) {
                *message_key = key;
            }
            return ESP_OK;
        }
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
    const solar_os_conversation_id_t conversation_id =
        messaging_upsert_conversation_locked(&conversation_request, false);
    if (conversation_id == 0) {
        messaging_unlock();
        return ESP_ERR_NO_MEM;
    }
    messaging_message_slot_t *slot =
        &messaging.messages[messaging.message_head];
    if (messaging.message_count == SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
        if (slot->message.unread && messaging.message_unread > 0) {
            messaging.message_unread--;
        }
        const int old_conversation = messaging_conversation_id_index_locked(
            slot->message.conversation_id);
        if (old_conversation >= 0 && slot->message.unread &&
            messaging.conversations[old_conversation].unread_count > 0) {
            messaging.conversations[old_conversation].unread_count--;
        }
        messaging.dropped_messages++;
    }
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
    messaging_unlock();

    if (messaging.persistent) {
        error = messaging_store_write(key);
        if (error != ESP_OK) {
            return error;
        }
    }
    messaging_publish_inbox_projection(request, key, revision);
    messaging_lock();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_MESSAGE,
                                    request->provider,
                                    conversation_id,
                                    key,
                                    SOLAR_OS_DELIVERY_RECEIVED,
                                    ESP_OK);
    messaging_unlock();
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
    messaging_lock();
    const int conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (conversation_index < 0) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (messaging.outbox_count >= SOLAR_OS_MESSAGING_OUTBOX_CAPACITY) {
        messaging.dropped_outbox++;
        messaging_unlock();
        return ESP_ERR_NO_MEM;
    }
    solar_os_messaging_conversation_t *conversation =
        &messaging.conversations[conversation_index];
    conversation_snapshot = *conversation;
    messaging_unlock();
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
    messaging_lock();
    const int current_conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (current_conversation_index < 0) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    conversation = &messaging.conversations[current_conversation_index];
    if (messaging.outbox_count >= SOLAR_OS_MESSAGING_OUTBOX_CAPACITY) {
        messaging.dropped_outbox++;
        messaging_unlock();
        return ESP_ERR_NO_MEM;
    }
    const solar_os_message_key_t key =
        messaging_next_u64(&messaging.next_message_key);
    messaging_message_slot_t *slot =
        &messaging.messages[messaging.message_head];
    if (messaging.message_count == SOLAR_OS_MESSAGING_MESSAGE_CAPACITY) {
        if (slot->message.unread && messaging.message_unread > 0) {
            messaging.message_unread--;
        }
        messaging.dropped_messages++;
    }
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
    messaging_unlock();
    if (messaging.persistent) {
        error = messaging_store_write(key);
        if (error != ESP_OK) {
            return error;
        }
    }
    if (message_key != NULL) {
        *message_key = key;
    }
    return ESP_OK;
}

size_t solar_os_messaging_message_visit_consistent(
    solar_os_conversation_id_t conversation_id,
    solar_os_messaging_provider_id_t provider,
    solar_os_messaging_message_visitor_t visitor,
    void *user,
    uint32_t *event_cursor,
    uint32_t *generation)
{
    if (visitor == NULL || solar_os_messaging_init() != ESP_OK) {
        return 0;
    }
    solar_os_messaging_message_t *snapshot = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_MESSAGE_CAPACITY,
        sizeof(*snapshot),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.snapshot");
    if (snapshot == NULL) {
        return 0;
    }
    size_t copied = 0;
    messaging_lock();
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        const solar_os_messaging_message_t *message =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY].message;
        if ((conversation_id == 0 ||
             message->conversation_id == conversation_id) &&
            (provider == 0 || message->provider == provider)) {
            snapshot[copied++] = *message;
        }
    }
    if (event_cursor != NULL && messaging.event_count > 0) {
        const size_t newest =
            (messaging.event_head + SOLAR_OS_MESSAGING_EVENT_CAPACITY - 1U) %
            SOLAR_OS_MESSAGING_EVENT_CAPACITY;
        *event_cursor = messaging.events[newest].id;
    }
    if (generation != NULL) {
        *generation = messaging.generation;
    }
    messaging_unlock();
    size_t visited = 0;
    for (; visited < copied; visited++) {
        if (!visitor(&snapshot[visited], user)) {
            break;
        }
    }
    solar_os_memory_free(snapshot);
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
    solar_os_messaging_provider_id_t provider,
    const char *provider_key)
{
    if (!messaging_provider_valid(provider) ||
        !messaging_text_valid(provider_key,
                              SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX,
                              false) ||
        solar_os_messaging_init() != ESP_OK) {
        return 0;
    }
    messaging_lock();
    const int conversation_index =
        messaging_conversation_index_locked(provider, provider_key);
    if (conversation_index < 0) {
        messaging_unlock();
        return 0;
    }
    const solar_os_conversation_id_t conversation_id =
        messaging.conversations[conversation_index].id;
    uint64_t cursor = 0;
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        const solar_os_messaging_message_t *message =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY].message;
        if (message->conversation_id == conversation_id &&
            message->provider_message_key > cursor) {
            cursor = message->provider_message_key;
        }
    }
    messaging_unlock();
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
    messaging_lock();
    const messaging_message_slot_t *slot =
        messaging_find_message_locked(message_key);
    if (slot != NULL) {
        *message = slot->message;
    }
    messaging_unlock();
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

static void messaging_recompute_summaries_locked(void)
{
    messaging.message_unread = 0;
    for (size_t i = 0; i < messaging.conversation_count; i++) {
        messaging.conversations[i].unread_count = 0;
        messaging.conversations[i].last_message_ms = 0;
    }
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        const solar_os_messaging_message_t *message =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY].message;
        const int conversation_index =
            messaging_conversation_id_index_locked(message->conversation_id);
        if (message->unread) {
            messaging.message_unread++;
            if (conversation_index >= 0) {
                messaging.conversations[conversation_index].unread_count++;
            }
        }
        if (conversation_index >= 0 &&
            message->timestamp_ms >
                messaging.conversations[conversation_index].last_message_ms) {
            messaging.conversations[conversation_index].last_message_ms =
                message->timestamp_ms;
        }
    }
}

esp_err_t solar_os_messaging_message_delete(
    solar_os_message_key_t message_key)
{
    if (message_key == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }

    int16_t disk_index = -1;
    uint32_t inbox_id = 0;
    solar_os_messaging_provider_id_t provider = 0;
    solar_os_conversation_id_t conversation_id = 0;
    messaging_lock();
    if (messaging_message_pending_locked(message_key)) {
        messaging_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t oldest = messaging_message_oldest_locked();
    size_t offset = SIZE_MAX;
    for (size_t i = 0; i < messaging.message_count; i++) {
        const size_t index =
            (oldest + i) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
        if (messaging.messages[index].message.key == message_key) {
            offset = i;
            disk_index = messaging.messages[index].disk_index;
            inbox_id = messaging.messages[index].message.inbox_id;
            provider = messaging.messages[index].message.provider;
            conversation_id =
                messaging.messages[index].message.conversation_id;
            break;
        }
    }
    if (offset == SIZE_MAX) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t move = offset; move + 1U < messaging.message_count; move++) {
        const size_t to =
            (oldest + move) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
        const size_t from =
            (oldest + move + 1U) % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
        messaging.messages[to] = messaging.messages[from];
    }
    messaging.message_head =
        (messaging.message_head + SOLAR_OS_MESSAGING_MESSAGE_CAPACITY - 1U) %
        SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
    memset(&messaging.messages[messaging.message_head],
           0,
           sizeof(*messaging.messages));
    messaging.messages[messaging.message_head].disk_index = -1;
    messaging.message_count--;
    messaging_recompute_summaries_locked();
    messaging_note_generation_locked();
    messaging_publish_event_locked(SOLAR_OS_MESSAGING_EVENT_MESSAGE_REMOVED,
                                    provider,
                                    conversation_id,
                                    message_key,
                                    SOLAR_OS_DELIVERY_RECEIVED,
                                    ESP_OK);
    messaging_unlock();

    esp_err_t store_error = ESP_OK;
    if (disk_index >= 0) {
        store_error = messaging_store_tombstone_many(&disk_index, 1);
    }
    esp_err_t inbox_error = ESP_OK;
    if (inbox_id != 0 &&
        solar_os_inbox_matches_source_id(inbox_id, message_key)) {
        inbox_error = solar_os_inbox_delete(inbox_id);
        if (inbox_error == ESP_ERR_NOT_FOUND) {
            inbox_error = ESP_OK;
        }
    }
    return store_error != ESP_OK ? store_error : inbox_error;
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
    return solar_os_inbox_delete_sources(sources, source_count, &deleted);
}

esp_err_t solar_os_messaging_clear(
    solar_os_messaging_provider_id_t provider,
    size_t *removed)
{
    if (removed != NULL) {
        *removed = 0;
    }
    if (provider != 0 && !messaging_provider_valid(provider)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    messaging_message_slot_t *kept = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_MESSAGE_CAPACITY,
        sizeof(*kept),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.clear");
    if (kept == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int16_t disk_indexes[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    size_t removed_count = 0;
    size_t kept_count = 0;

    messaging_lock();
    const size_t outbox_oldest =
        (messaging.outbox_head + SOLAR_OS_MESSAGING_OUTBOX_CAPACITY -
         messaging.outbox_count) % SOLAR_OS_MESSAGING_OUTBOX_CAPACITY;
    for (size_t i = 0; i < messaging.outbox_count; i++) {
        const solar_os_messaging_outbound_t *outbound =
            &messaging.outbox[(outbox_oldest + i) %
                              SOLAR_OS_MESSAGING_OUTBOX_CAPACITY];
        if (provider == 0 || outbound->provider == provider) {
            messaging_unlock();
            solar_os_memory_free(kept);
            return ESP_ERR_INVALID_STATE;
        }
    }
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        const messaging_message_slot_t *slot =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
        if (provider == 0 || slot->message.provider == provider) {
            disk_indexes[removed_count] = slot->disk_index;
            removed_count++;
        } else {
            kept[kept_count++] = *slot;
        }
    }
    if (removed_count > 0) {
        memset(messaging.messages,
               0,
               SOLAR_OS_MESSAGING_MESSAGE_CAPACITY *
                   sizeof(*messaging.messages));
        memcpy(messaging.messages, kept, kept_count * sizeof(*kept));
        for (size_t i = kept_count;
             i < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
             i++) {
            messaging.messages[i].disk_index = -1;
        }
        messaging.message_count = kept_count;
        messaging.message_head =
            kept_count % SOLAR_OS_MESSAGING_MESSAGE_CAPACITY;
        messaging_recompute_summaries_locked();
        messaging_note_generation_locked();
        messaging_publish_event_locked(
            SOLAR_OS_MESSAGING_EVENT_MESSAGES_CLEARED,
            provider,
            0,
            0,
            SOLAR_OS_DELIVERY_RECEIVED,
            ESP_OK);
    }
    messaging_unlock();
    solar_os_memory_free(kept);

    const esp_err_t store_error =
        messaging_store_tombstone_many(disk_indexes, removed_count);
    const esp_err_t inbox_error = messaging_clear_inbox_projections(provider);
    if (removed != NULL) {
        *removed = removed_count;
    }
    return store_error != ESP_OK ? store_error : inbox_error;
}

esp_err_t solar_os_messaging_mark_read(
    solar_os_conversation_id_t conversation_id)
{
    if (conversation_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    solar_os_message_key_t keys[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    uint32_t inbox_ids[SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
    size_t count = 0;
    messaging_lock();
    const int conversation_index =
        messaging_conversation_id_index_locked(conversation_id);
    if (conversation_index < 0) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const size_t oldest = messaging_message_oldest_locked();
    for (size_t i = 0; i < messaging.message_count; i++) {
        messaging_message_slot_t *slot =
            &messaging.messages[(oldest + i) %
                                SOLAR_OS_MESSAGING_MESSAGE_CAPACITY];
        if (slot->message.conversation_id == conversation_id &&
            slot->message.unread) {
            slot->message.unread = false;
            slot->revision++;
            keys[count] = slot->message.key;
            inbox_ids[count] = slot->message.inbox_id;
            count++;
            if (messaging.message_unread > 0) {
                messaging.message_unread--;
            }
        }
    }
    messaging.conversations[conversation_index].unread_count = 0;
    if (count > 0) {
        messaging_note_generation_locked();
    }
    messaging_unlock();
    for (size_t i = 0; i < count; i++) {
        if (messaging.persistent) {
            (void)messaging_store_update(keys[i]);
        }
        if (inbox_ids[i] != 0 &&
            solar_os_inbox_matches_source_id(inbox_ids[i], keys[i])) {
            (void)solar_os_inbox_mark_read(inbox_ids[i], true);
        }
    }
    return ESP_OK;
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
    messaging_lock();
    messaging_message_slot_t *slot =
        messaging_find_message_locked(message_key);
    if (slot == NULL || slot->message.direction != SOLAR_OS_MESSAGE_OUTBOUND ||
        (slot->message.delivery != SOLAR_OS_DELIVERY_QUEUED &&
         slot->message.delivery != SOLAR_OS_DELIVERY_FAILED)) {
        messaging_unlock();
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
        messaging_unlock();
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
    messaging_unlock();
    if (messaging.persistent) {
        (void)messaging_store_update(message_key);
    }
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
    messaging_lock();
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
            found_offset = i;
            key = candidate->message_key;
            if (state == SOLAR_OS_DELIVERY_QUEUED) {
                candidate->attempts++;
            }
            break;
        }
    }
    if (found_offset == SIZE_MAX) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    messaging_message_slot_t *slot = messaging_find_message_locked(key);
    if (slot == NULL) {
        messaging_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    slot->message.delivery = state;
    strlcpy(slot->message.error,
            error != NULL ? error : "",
            sizeof(slot->message.error));
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
    messaging_unlock();
    if (messaging.persistent) {
        (void)messaging_store_update(key);
    }
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
