#include "solar_os_inbox.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_time.h"

#define INBOX_STORE_MAGIC 0x58424e49UL
#define INBOX_STORE_VERSION 2U
#define INBOX_STORE_HEADER_COPIES 2U
#define INBOX_EPOCH_MIN_MS 1577836800000ULL

typedef struct {
    solar_os_inbox_entry_t entry;
    uint64_t dedupe_hash;
} inbox_slot_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint32_t generation;
    uint32_t head;
    uint32_t count;
    uint32_t next_id;
    uint32_t dropped;
    uint32_t crc32;
} inbox_store_header_t;

typedef struct {
    inbox_slot_t slot;
    uint32_t crc32;
} inbox_store_record_t;

#define INBOX_STORE_RECORDS_OFFSET \
    (INBOX_STORE_HEADER_COPIES * sizeof(inbox_store_header_t))
#define INBOX_STORE_MAX_BYTES \
    (INBOX_STORE_RECORDS_OFFSET + SOLAR_OS_INBOX_CAPACITY * sizeof(inbox_store_record_t))

_Static_assert(INBOX_STORE_MAX_BYTES <= 32U * 1024U,
               "persistent inbox must fit comfortably on the 64 KB flash volume");

static SemaphoreHandle_t inbox_mutex;
static inbox_slot_t *inbox_ring;
static size_t inbox_capacity;
static size_t inbox_count;
static size_t inbox_head;
static size_t inbox_unread;
static size_t inbox_bytes;
static uint32_t inbox_next_id = 1;
static uint32_t inbox_dropped;
static bool inbox_initialized;
static bool inbox_ring_in_psram;
static bool inbox_persistent;
static uint32_t inbox_store_generation;
static esp_err_t inbox_storage_error = ESP_ERR_INVALID_STATE;
static char inbox_store_path[SOLAR_OS_STORAGE_PATH_MAX];
static const char *TAG = "inbox";

static bool inbox_priority_valid(solar_os_inbox_priority_t priority)
{
    return priority >= SOLAR_OS_INBOX_PRIORITY_LOW &&
        priority <= SOLAR_OS_INBOX_PRIORITY_URGENT;
}

static esp_err_t inbox_ensure_mutex(void)
{
    if (inbox_mutex != NULL) {
        return ESP_OK;
    }

    inbox_mutex = xSemaphoreCreateMutex();
    return inbox_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void inbox_lock(void)
{
    if (inbox_mutex != NULL) {
        xSemaphoreTake(inbox_mutex, portMAX_DELAY);
    }
}

static void inbox_unlock(void)
{
    if (inbox_mutex != NULL) {
        xSemaphoreGive(inbox_mutex);
    }
}

static bool inbox_copy_string(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0) {
        return source != NULL && source[0] != '\0';
    }

    const char *value = source != NULL ? source : "";
    const bool truncated = strlen(value) >= capacity;
    strlcpy(destination, value, capacity);
    return truncated;
}

static uint32_t inbox_crc32(const void *data, size_t len)
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

static uint64_t inbox_hash_update(uint64_t hash, const char *text)
{
    const uint8_t *bytes = (const uint8_t *)(text != NULL ? text : "");
    while (*bytes != 0) {
        hash ^= *bytes++;
        hash *= 1099511628211ULL;
    }
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
    return hash;
}

static uint64_t inbox_dedupe_hash(const solar_os_inbox_publish_t *message)
{
    if (message->dedupe_key == NULL || message->dedupe_key[0] == '\0') {
        return 0;
    }

    uint64_t hash = 1469598103934665603ULL;
    hash = inbox_hash_update(hash, message->source);
    hash = inbox_hash_update(hash, message->topic);
    hash = inbox_hash_update(hash, message->sender);
    hash = inbox_hash_update(hash, message->dedupe_key);
    return hash != 0 ? hash : 1U;
}

static esp_err_t inbox_sync_file(FILE *file)
{
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

static void inbox_make_header(inbox_store_header_t *header, uint32_t generation)
{
    memset(header, 0, sizeof(*header));
    header->magic = INBOX_STORE_MAGIC;
    header->version = INBOX_STORE_VERSION;
    header->capacity = SOLAR_OS_INBOX_CAPACITY;
    header->generation = generation;
    header->head = (uint32_t)inbox_head;
    header->count = (uint32_t)inbox_count;
    header->next_id = inbox_next_id;
    header->dropped = inbox_dropped;
    header->crc32 = inbox_crc32(header, offsetof(inbox_store_header_t, crc32));
}

static bool inbox_header_valid(const inbox_store_header_t *header)
{
    return header->magic == INBOX_STORE_MAGIC &&
        header->version == INBOX_STORE_VERSION &&
        header->capacity == SOLAR_OS_INBOX_CAPACITY &&
        header->head < SOLAR_OS_INBOX_CAPACITY &&
        header->count <= SOLAR_OS_INBOX_CAPACITY &&
        header->next_id != 0 &&
        header->crc32 == inbox_crc32(header, offsetof(inbox_store_header_t, crc32));
}

static bool inbox_generation_newer(uint32_t first, uint32_t second)
{
    return (int32_t)(first - second) > 0;
}

static void inbox_make_record(inbox_store_record_t *record, size_t index)
{
    memset(record, 0, sizeof(*record));
    record->slot = inbox_ring[index];
    record->crc32 = inbox_crc32(record, offsetof(inbox_store_record_t, crc32));
}

static bool inbox_record_valid(const inbox_store_record_t *record)
{
    return record->slot.entry.id != 0 &&
        record->crc32 == inbox_crc32(record, offsetof(inbox_store_record_t, crc32));
}

static esp_err_t inbox_prepare_store_path(void)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_default_path(SOLAR_OS_INBOX_STORE_DIR,
                                                  directory,
                                                  sizeof(directory));
    if (err != ESP_OK) {
        return err;
    }
    if (solar_os_storage_mkdir(directory) != ESP_OK && errno != EEXIST) {
        return ESP_FAIL;
    }

    return solar_os_storage_default_path(SOLAR_OS_INBOX_STORE_DIR "/" SOLAR_OS_INBOX_STORE_FILE,
                                         inbox_store_path,
                                         sizeof(inbox_store_path));
}

static esp_err_t inbox_store_reset_locked(void)
{
    FILE *file = fopen(inbox_store_path, "w+b");
    if (file == NULL) {
        return ESP_FAIL;
    }

    uint32_t generation = inbox_store_generation + 1U;
    if (generation == 0) {
        generation = 1;
    }
    inbox_store_header_t header;
    inbox_make_header(&header, generation);
    bool ok = true;
    for (size_t i = 0; i < INBOX_STORE_HEADER_COPIES; i++) {
        if (fwrite(&header, sizeof(header), 1, file) != 1) {
            ok = false;
            break;
        }
    }
    esp_err_t err = ok ? inbox_sync_file(file) : ESP_FAIL;
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        inbox_store_generation = generation;
        inbox_persistent = true;
        inbox_storage_error = ESP_OK;
    } else {
        inbox_storage_error = err;
    }
    return err;
}

static esp_err_t inbox_store_write_locked(size_t index, bool update_header)
{
    if (!inbox_persistent || index >= inbox_capacity) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(inbox_store_path, "r+b");
    if (file == NULL) {
        inbox_storage_error = ESP_FAIL;
        return ESP_FAIL;
    }

    inbox_store_record_t record;
    inbox_make_record(&record, index);
    const long record_offset = (long)(INBOX_STORE_RECORDS_OFFSET +
                                      index * sizeof(inbox_store_record_t));
    esp_err_t err = ESP_OK;
    if (fseek(file, record_offset, SEEK_SET) != 0 ||
        fwrite(&record, sizeof(record), 1, file) != 1) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = inbox_sync_file(file);
    }

    uint32_t generation = inbox_store_generation;
    if (err == ESP_OK && update_header) {
        generation++;
        if (generation == 0) {
            generation = 1;
        }
        inbox_store_header_t header;
        inbox_make_header(&header, generation);
        const long header_offset =
            (long)((generation % INBOX_STORE_HEADER_COPIES) * sizeof(header));
        if (fseek(file, header_offset, SEEK_SET) != 0 ||
            fwrite(&header, sizeof(header), 1, file) != 1) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK) {
            err = inbox_sync_file(file);
        }
    }

    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        inbox_store_generation = generation;
        inbox_storage_error = ESP_OK;
    } else {
        inbox_storage_error = err;
    }
    return err;
}

static esp_err_t inbox_store_load_locked(void)
{
    esp_err_t err = inbox_prepare_store_path();
    if (err != ESP_OK) {
        inbox_persistent = false;
        inbox_storage_error = err;
        return err;
    }

    struct stat info;
    if (stat(inbox_store_path, &info) != 0) {
        if (errno == ENOENT) {
            return inbox_store_reset_locked();
        }
        inbox_storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    if (info.st_size < (off_t)INBOX_STORE_RECORDS_OFFSET ||
        info.st_size > (off_t)INBOX_STORE_MAX_BYTES) {
        SOLAR_OS_LOGW(TAG, "invalid store size %ld; resetting", (long)info.st_size);
        return inbox_store_reset_locked();
    }

    FILE *file = fopen(inbox_store_path, "rb");
    if (file == NULL) {
        inbox_storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    inbox_store_header_t headers[INBOX_STORE_HEADER_COPIES];
    const bool headers_read = fread(headers, sizeof(headers), 1, file) == 1;
    const bool first_valid = headers_read && inbox_header_valid(&headers[0]);
    const bool second_valid = headers_read && inbox_header_valid(&headers[1]);
    if (!first_valid && !second_valid) {
        fclose(file);
        SOLAR_OS_LOGW(TAG, "store headers invalid; resetting");
        return inbox_store_reset_locked();
    }

    const inbox_store_header_t *header = &headers[0];
    if (!first_valid ||
        (second_valid && inbox_generation_newer(headers[1].generation,
                                                headers[0].generation))) {
        header = &headers[1];
    }

    memset(inbox_ring, 0, sizeof(*inbox_ring) * inbox_capacity);
    inbox_unread = 0;
    bool records_valid = true;
    const size_t oldest =
        (header->head + SOLAR_OS_INBOX_CAPACITY - header->count) % SOLAR_OS_INBOX_CAPACITY;
    for (size_t i = 0; i < header->count; i++) {
        const size_t index = (oldest + i) % SOLAR_OS_INBOX_CAPACITY;
        const long offset = (long)(INBOX_STORE_RECORDS_OFFSET +
                                   index * sizeof(inbox_store_record_t));
        inbox_store_record_t record;
        if (fseek(file, offset, SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, file) != 1 ||
            !inbox_record_valid(&record)) {
            records_valid = false;
            break;
        }
        inbox_ring[index] = record.slot;
        if (record.slot.entry.unread) {
            inbox_unread++;
        }
    }
    fclose(file);

    if (!records_valid) {
        SOLAR_OS_LOGW(TAG, "store record invalid; resetting");
        memset(inbox_ring, 0, sizeof(*inbox_ring) * inbox_capacity);
        inbox_count = 0;
        inbox_head = 0;
        inbox_unread = 0;
        inbox_next_id = 1;
        inbox_dropped = 0;
        return inbox_store_reset_locked();
    }

    inbox_head = header->head;
    inbox_count = header->count;
    inbox_next_id = header->next_id;
    inbox_dropped = header->dropped;
    inbox_store_generation = header->generation;
    inbox_persistent = true;
    inbox_storage_error = ESP_OK;
    return ESP_OK;
}

static esp_err_t inbox_allocate_ring(void)
{
    if (inbox_ring != NULL) {
        return ESP_OK;
    }

    inbox_bytes = sizeof(inbox_slot_t) * SOLAR_OS_INBOX_CAPACITY;
    inbox_ring = solar_os_memory_calloc(SOLAR_OS_INBOX_CAPACITY,
                                        sizeof(inbox_slot_t),
                                        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                        "inbox.ring");
    inbox_ring_in_psram = solar_os_memory_is_external(inbox_ring);
    if (inbox_ring == NULL) {
        inbox_bytes = 0;
        return ESP_ERR_NO_MEM;
    }

    inbox_capacity = SOLAR_OS_INBOX_CAPACITY;
    return ESP_OK;
}

esp_err_t solar_os_inbox_init(void)
{
    esp_err_t err = inbox_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    inbox_lock();
    const bool initialized = inbox_initialized;
    inbox_unlock();
    if (initialized) {
        return ESP_OK;
    }

    err = inbox_allocate_ring();
    if (err != ESP_OK) {
        return err;
    }

    inbox_lock();
    const esp_err_t store_err = inbox_store_load_locked();
    if (store_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "persistent store unavailable: %s", esp_err_to_name(store_err));
    }
    inbox_initialized = true;
    inbox_unlock();
    return ESP_OK;
}

esp_err_t solar_os_inbox_publish(const solar_os_inbox_publish_t *message, uint32_t *id)
{
    if (id != NULL) {
        *id = 0;
    }
    if (message == NULL || message->source == NULL || message->source[0] == '\0' ||
        !inbox_priority_valid(message->priority) ||
        ((message->title == NULL || message->title[0] == '\0') &&
         (message->body == NULL || message->body[0] == '\0'))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xPortInIsrContext()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = solar_os_inbox_init();
    if (err != ESP_OK) {
        return err;
    }

    uint64_t reception_ms = 0;
    if (solar_os_time_get_utc_epoch_ms(&reception_ms) != ESP_OK) {
        reception_ms = message->timestamp_ms >= INBOX_EPOCH_MIN_MS ?
            message->timestamp_ms : 0;
    }

    inbox_lock();
    const uint64_t dedupe_hash = inbox_dedupe_hash(message);
    if (dedupe_hash != 0) {
        const size_t oldest =
            (inbox_head + inbox_capacity - inbox_count) % inbox_capacity;
        for (size_t i = 0; i < inbox_count; i++) {
            inbox_slot_t *existing = &inbox_ring[(oldest + i) % inbox_capacity];
            if (existing->dedupe_hash == dedupe_hash) {
                if (id != NULL) {
                    *id = existing->entry.id;
                }
                inbox_unlock();
                return ESP_OK;
            }
        }
    }

    const size_t write_index = inbox_head;
    const inbox_slot_t previous_slot = inbox_ring[write_index];
    const size_t previous_head = inbox_head;
    const size_t previous_count = inbox_count;
    const size_t previous_unread = inbox_unread;
    const uint32_t previous_next_id = inbox_next_id;
    const uint32_t previous_dropped = inbox_dropped;

    inbox_slot_t *slot = &inbox_ring[write_index];
    solar_os_inbox_entry_t *entry = &slot->entry;
    if (inbox_count == inbox_capacity) {
        if (entry->unread && inbox_unread > 0) {
            inbox_unread--;
        }
        inbox_dropped++;
    }

    memset(slot, 0, sizeof(*slot));
    slot->dedupe_hash = dedupe_hash;
    entry->id = inbox_next_id++;
    if (inbox_next_id == 0) {
        inbox_next_id = 1;
    }
    entry->source_id = message->source_id;
    entry->source_context = message->source_context;
    entry->timestamp_ms = reception_ms;
    entry->received_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    entry->priority = message->priority;
    entry->unread = true;
    entry->truncated = inbox_copy_string(entry->source, sizeof(entry->source), message->source);
    entry->truncated |= inbox_copy_string(entry->topic, sizeof(entry->topic), message->topic);
    entry->truncated |= inbox_copy_string(entry->sender, sizeof(entry->sender), message->sender);
    entry->truncated |= inbox_copy_string(entry->title, sizeof(entry->title), message->title);
    entry->truncated |= inbox_copy_string(entry->body, sizeof(entry->body), message->body);

    inbox_head = (inbox_head + 1U) % inbox_capacity;
    if (inbox_count < inbox_capacity) {
        inbox_count++;
    }
    inbox_unread++;
    if (inbox_persistent && inbox_store_write_locked(write_index, true) != ESP_OK) {
        inbox_ring[write_index] = previous_slot;
        inbox_head = previous_head;
        inbox_count = previous_count;
        inbox_unread = previous_unread;
        inbox_next_id = previous_next_id;
        inbox_dropped = previous_dropped;
        inbox_unlock();
        return inbox_storage_error;
    }
    if (id != NULL) {
        *id = entry->id;
    }
    inbox_unlock();
    return ESP_OK;
}

static inbox_slot_t *inbox_find_locked(uint32_t id, size_t *found_index)
{
    if (id == 0 || inbox_ring == NULL || inbox_capacity == 0) {
        return NULL;
    }

    const size_t oldest = (inbox_head + inbox_capacity - inbox_count) % inbox_capacity;
    for (size_t i = 0; i < inbox_count; i++) {
        const size_t index = (oldest + i) % inbox_capacity;
        inbox_slot_t *slot = &inbox_ring[index];
        if (slot->entry.id == id) {
            if (found_index != NULL) {
                *found_index = index;
            }
            return slot;
        }
    }
    return NULL;
}

esp_err_t solar_os_inbox_get(uint32_t id, solar_os_inbox_entry_t *entry, bool mark_read)
{
    if (entry == NULL || id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = solar_os_inbox_init();
    if (err != ESP_OK) {
        return err;
    }

    inbox_lock();
    size_t index = 0;
    inbox_slot_t *stored = inbox_find_locked(id, &index);
    if (stored == NULL) {
        inbox_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *entry = stored->entry;
    if (mark_read && stored->entry.unread) {
        stored->entry.unread = false;
        if (inbox_unread > 0) {
            inbox_unread--;
        }
        if (inbox_persistent && inbox_store_write_locked(index, false) != ESP_OK) {
            stored->entry.unread = true;
            inbox_unread++;
            inbox_unlock();
            return inbox_storage_error;
        }
    }
    inbox_unlock();
    return ESP_OK;
}

esp_err_t solar_os_inbox_mark_read(uint32_t id, bool read)
{
    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = solar_os_inbox_init();
    if (err != ESP_OK) {
        return err;
    }

    inbox_lock();
    size_t index = 0;
    inbox_slot_t *slot = inbox_find_locked(id, &index);
    if (slot == NULL) {
        inbox_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    const bool unread = !read;
    if (slot->entry.unread != unread) {
        const bool previous_unread = slot->entry.unread;
        slot->entry.unread = unread;
        if (unread) {
            inbox_unread++;
        } else if (inbox_unread > 0) {
            inbox_unread--;
        }
        if (inbox_persistent && inbox_store_write_locked(index, false) != ESP_OK) {
            slot->entry.unread = previous_unread;
            if (previous_unread) {
                inbox_unread++;
            } else if (inbox_unread > 0) {
                inbox_unread--;
            }
            inbox_unlock();
            return inbox_storage_error;
        }
    }
    inbox_unlock();
    return ESP_OK;
}

size_t solar_os_inbox_snapshot(solar_os_inbox_entry_t *entries,
                               size_t max_entries,
                               bool unread_only,
                               size_t *total_entries)
{
    if (total_entries != NULL) {
        *total_entries = 0;
    }
    if (solar_os_inbox_init() != ESP_OK) {
        return 0;
    }

    inbox_lock();
    size_t copied = 0;
    size_t matched = 0;
    for (size_t i = 0; i < inbox_count; i++) {
        const size_t index = (inbox_head + inbox_capacity - 1U - i) % inbox_capacity;
        const solar_os_inbox_entry_t *entry = &inbox_ring[index].entry;
        if (unread_only && !entry->unread) {
            continue;
        }
        matched++;
        if (entries != NULL && copied < max_entries) {
            entries[copied++] = *entry;
        }
    }
    if (total_entries != NULL) {
        *total_entries = matched;
    }
    inbox_unlock();
    return copied;
}

esp_err_t solar_os_inbox_get_status(solar_os_inbox_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = solar_os_inbox_init();
    if (err != ESP_OK) {
        return err;
    }

    inbox_lock();
    *status = (solar_os_inbox_status_t){
        .initialized = inbox_initialized,
        .ring_in_psram = inbox_ring_in_psram,
        .capacity = inbox_capacity,
        .count = inbox_count,
        .unread = inbox_unread,
        .bytes = inbox_bytes,
        .dropped = inbox_dropped,
        .persistent = inbox_persistent,
        .storage_limit_bytes = INBOX_STORE_MAX_BYTES,
        .storage_error = inbox_storage_error,
    };
    inbox_unlock();
    return ESP_OK;
}

esp_err_t solar_os_inbox_clear(void)
{
    esp_err_t err = solar_os_inbox_init();
    if (err != ESP_OK) {
        return err;
    }

    inbox_lock();
    memset(inbox_ring, 0, sizeof(*inbox_ring) * inbox_capacity);
    inbox_count = 0;
    inbox_head = 0;
    inbox_unread = 0;
    inbox_dropped = 0;
    inbox_next_id = 1;
    if (inbox_persistent && inbox_store_reset_locked() != ESP_OK) {
        inbox_unlock();
        return inbox_storage_error;
    }
    inbox_unlock();
    return ESP_OK;
}

const char *solar_os_inbox_priority_name(solar_os_inbox_priority_t priority)
{
    switch (priority) {
    case SOLAR_OS_INBOX_PRIORITY_LOW:
        return "low";
    case SOLAR_OS_INBOX_PRIORITY_HIGH:
        return "high";
    case SOLAR_OS_INBOX_PRIORITY_URGENT:
        return "urgent";
    case SOLAR_OS_INBOX_PRIORITY_NORMAL:
    default:
        return "normal";
    }
}
