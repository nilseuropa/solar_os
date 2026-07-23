#include "solar_os_chat.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_inbox.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define CHAT_NVS_NAMESPACE "chat"
#define CHAT_NVS_URL_KEY "url"
#define CHAT_NVS_TOKEN_KEY "token"
#define CHAT_NVS_USER_KEY "user"
#define CHAT_NVS_DEVICE_KEY "device"
#define CHAT_NVS_ENABLED_KEY "enabled"
#define CHAT_DEFAULT_USER "user"
#define CHAT_DEFAULT_DEVICE "sol"
#define CHAT_DEFAULT_CHANNEL "general"
#define CHAT_EVENT_WAIT_POLL_MS 20U
#define CHAT_STORE_MAGIC 0x54414843UL
#define CHAT_STORE_VERSION 1U
#define CHAT_STORE_HEADER_COPIES 2U
#define CHAT_STORE_RESERVED_BYTES (36U * 1024U)

typedef struct {
    solar_os_chat_message_t message;
    int16_t disk_index;
} chat_message_slot_t;

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
} chat_store_header_t;

typedef struct {
    solar_os_chat_message_t message;
    uint32_t crc32;
} chat_store_record_t;

#define CHAT_STORE_RECORDS_OFFSET \
    (CHAT_STORE_HEADER_COPIES * sizeof(chat_store_header_t))
#define CHAT_STORE_MAX_BYTES \
    (CHAT_STORE_RECORDS_OFFSET + \
     SOLAR_OS_CHAT_MESSAGE_CAPACITY * sizeof(chat_store_record_t))

_Static_assert(CHAT_STORE_MAX_BYTES <= 384U * 1024U,
               "persistent chat history must remain bounded");

typedef struct {
    bool initialized;
    bool configured;
    bool enabled;
    bool sync_running;
    bool connected;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
    char last_error[SOLAR_OS_CHAT_ERROR_MAX];
    esp_err_t last_esp_error;
    uint32_t config_revision;
    uint32_t next_event_id;
    uint32_t next_message_id;
    uint32_t next_command_id;
    uint32_t legacy_cursor;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    solar_os_chat_event_t *events;
    size_t event_head;
    size_t event_count;
    chat_message_slot_t *messages;
    chat_store_record_t *record_scratch;
    size_t message_head;
    size_t message_count;
    size_t message_unread;
    size_t disk_capacity;
    size_t disk_head;
    size_t disk_count;
    uint32_t disk_generation;
    uint32_t message_dropped_count;
    bool persistent;
    bool inbox_backed;
    size_t inbox_capacity;
    size_t inbox_limit_bytes;
    esp_err_t storage_error;
    char store_path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_chat_command_t *outbox;
    size_t outbox_head;
    size_t outbox_count;
    solar_os_chat_channel_t channels[SOLAR_OS_CHAT_CHANNEL_CAPACITY];
    size_t channel_count;
    SemaphoreHandle_t lock;
} solar_os_chat_store_t;

typedef struct {
    bool enabled;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
} solar_os_chat_saved_config_t;

static solar_os_chat_store_t chat_store;
static const char *TAG = "chat";

static void chat_lock(void)
{
    if (chat_store.lock != NULL) {
        (void)xSemaphoreTake(chat_store.lock, portMAX_DELAY);
    }
}

static void chat_unlock(void)
{
    if (chat_store.lock != NULL) {
        xSemaphoreGive(chat_store.lock);
    }
}

static uint32_t chat_crc32(const void *data, size_t len)
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

static uint64_t chat_hash_update(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
    return hash;
}

static uint64_t chat_hash_string(uint64_t hash, const char *text)
{
    const char *value = text != NULL ? text : "";
    return chat_hash_update(hash, value, strlen(value));
}

static uint64_t chat_message_key_locked(const solar_os_chat_event_t *event)
{
    if (event->message_key != 0) {
        return event->message_key;
    }
    if (event->timestamp == 0) {
        return 0;
    }

    uint64_t hash = 1469598103934665603ULL;
    hash = chat_hash_string(hash, chat_store.url);
    hash = chat_hash_string(hash, event->channel);
    hash = chat_hash_string(hash, event->from);
    hash = chat_hash_update(hash, &event->timestamp, sizeof(event->timestamp));
    hash = chat_hash_string(hash, event->text);
    return hash != 0 ? hash : 1U;
}

static void chat_make_store_header(chat_store_header_t *header,
                                   uint32_t generation)
{
    memset(header, 0, sizeof(*header));
    header->magic = CHAT_STORE_MAGIC;
    header->version = CHAT_STORE_VERSION;
    header->capacity = (uint16_t)chat_store.disk_capacity;
    header->generation = generation;
    header->head = (uint32_t)chat_store.disk_head;
    header->count = (uint32_t)chat_store.disk_count;
    header->next_id = chat_store.next_message_id;
    header->dropped = chat_store.message_dropped_count;
    header->crc32 = chat_crc32(header, offsetof(chat_store_header_t, crc32));
}

static bool chat_store_header_valid(const chat_store_header_t *header)
{
    return header->magic == CHAT_STORE_MAGIC &&
        header->version == CHAT_STORE_VERSION &&
        header->capacity > 0 &&
        header->capacity <= SOLAR_OS_CHAT_MESSAGE_CAPACITY &&
        header->head < header->capacity &&
        header->count <= header->capacity &&
        header->next_id != 0 &&
        header->crc32 == chat_crc32(header,
                                    offsetof(chat_store_header_t, crc32));
}

static bool chat_generation_newer(uint32_t first, uint32_t second)
{
    return (int32_t)(first - second) > 0;
}

static void chat_make_store_record(chat_store_record_t *record,
                                   const solar_os_chat_message_t *message)
{
    memset(record, 0, sizeof(*record));
    record->message = *message;
    record->crc32 = chat_crc32(record, offsetof(chat_store_record_t, crc32));
}

static bool chat_store_record_valid(const chat_store_record_t *record)
{
    return record->message.id != 0 &&
        record->message.message_key != 0 &&
        record->message.channel[0] != '\0' &&
        record->crc32 == chat_crc32(record,
                                    offsetof(chat_store_record_t, crc32));
}

static esp_err_t chat_sync_file(FILE *file)
{
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t chat_prepare_store_path(void)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!solar_os_storage_sd_is_mounted()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_default_path(SOLAR_OS_CHAT_STORE_DIR,
                                                  directory,
                                                  sizeof(directory));
    if (err != ESP_OK) {
        return err;
    }
    if (solar_os_storage_mkdir(directory) != ESP_OK && errno != EEXIST) {
        return ESP_FAIL;
    }
    return solar_os_storage_default_path(SOLAR_OS_CHAT_STORE_DIR "/" SOLAR_OS_CHAT_STORE_FILE,
                                         chat_store.store_path,
                                         sizeof(chat_store.store_path));
}

static size_t chat_select_disk_capacity(uint64_t reclaimed_bytes)
{
    solar_os_storage_usage_t usage;
    if (solar_os_storage_get_usage(&usage) != ESP_OK) {
        return 0;
    }
    const uint64_t available = usage.free_bytes + reclaimed_bytes;
    if (available <= CHAT_STORE_RESERVED_BYTES + CHAT_STORE_RECORDS_OFFSET) {
        return 0;
    }
    uint64_t bytes = available - CHAT_STORE_RESERVED_BYTES -
        CHAT_STORE_RECORDS_OFFSET;
    size_t capacity = (size_t)(bytes / sizeof(chat_store_record_t));
    if (capacity > SOLAR_OS_CHAT_MESSAGE_CAPACITY) {
        capacity = SOLAR_OS_CHAT_MESSAGE_CAPACITY;
    }
    return capacity;
}

static esp_err_t chat_store_reset_locked(void)
{
    if (chat_store.disk_capacity == 0) {
        chat_store.persistent = false;
        chat_store.storage_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(chat_store.store_path, "w+b");
    if (file == NULL) {
        chat_store.storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    uint32_t generation = chat_store.disk_generation + 1U;
    if (generation == 0) {
        generation = 1;
    }
    chat_store.disk_head = 0;
    chat_store.disk_count = 0;
    chat_store_header_t header;
    chat_make_store_header(&header, generation);
    bool ok = true;
    for (size_t i = 0; i < CHAT_STORE_HEADER_COPIES; i++) {
        if (fwrite(&header, sizeof(header), 1, file) != 1) {
            ok = false;
            break;
        }
    }
    esp_err_t err = ok ? chat_sync_file(file) : ESP_FAIL;
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        chat_store.disk_generation = generation;
        chat_store.persistent = true;
        chat_store.storage_error = ESP_OK;
    } else {
        chat_store.persistent = false;
        chat_store.storage_error = err;
    }
    return err;
}

static void chat_clear_disk_mapping_locked(size_t disk_index)
{
    for (size_t i = 0; i < SOLAR_OS_CHAT_MESSAGE_CAPACITY; i++) {
        if (chat_store.messages[i].disk_index == (int16_t)disk_index) {
            chat_store.messages[i].disk_index = -1;
        }
    }
}

static int chat_store_write_message_locked(const solar_os_chat_message_t *message)
{
    if (!chat_store.persistent || chat_store.disk_capacity == 0 || message == NULL) {
        chat_store.storage_error = ESP_ERR_INVALID_STATE;
        return -1;
    }

    FILE *file = fopen(chat_store.store_path, "r+b");
    if (file == NULL) {
        chat_store.storage_error = ESP_FAIL;
        return -1;
    }
    const size_t disk_index = chat_store.disk_head;
    chat_store_record_t *record = chat_store.record_scratch;
    if (message == &record->message) {
        record->crc32 = chat_crc32(record, offsetof(chat_store_record_t, crc32));
    } else {
        chat_make_store_record(record, message);
    }
    const long record_offset = (long)(CHAT_STORE_RECORDS_OFFSET +
                                      disk_index * sizeof(*record));
    esp_err_t err = ESP_OK;
    if (fseek(file, record_offset, SEEK_SET) != 0 ||
        fwrite(record, sizeof(*record), 1, file) != 1 ||
        chat_sync_file(file) != ESP_OK) {
        err = ESP_FAIL;
    }

    uint32_t generation = chat_store.disk_generation;
    if (err == ESP_OK) {
        generation++;
        if (generation == 0) {
            generation = 1;
        }
        chat_store.disk_head = (disk_index + 1U) % chat_store.disk_capacity;
        if (chat_store.disk_count < chat_store.disk_capacity) {
            chat_store.disk_count++;
        }
        chat_store_header_t header;
        chat_make_store_header(&header, generation);
        const long header_offset =
            (long)((generation % CHAT_STORE_HEADER_COPIES) * sizeof(header));
        if (fseek(file, header_offset, SEEK_SET) != 0 ||
            fwrite(&header, sizeof(header), 1, file) != 1 ||
            chat_sync_file(file) != ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        chat_clear_disk_mapping_locked(disk_index);
        chat_store.disk_generation = generation;
        chat_store.storage_error = ESP_OK;
    } else {
        chat_store.storage_error = err;
    }
    return err == ESP_OK ? (int)disk_index : -1;
}

static esp_err_t chat_store_update_messages_locked(chat_message_slot_t *const *slots,
                                                   size_t count)
{
    if (!chat_store.persistent || slots == NULL || count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *file = fopen(chat_store.store_path, "r+b");
    if (file == NULL) {
        chat_store.storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        const chat_message_slot_t *slot = slots[i];
        if (slot == NULL || slot->disk_index < 0 ||
            (size_t)slot->disk_index >= chat_store.disk_capacity) {
            continue;
        }
        chat_store_record_t *record = chat_store.record_scratch;
        chat_make_store_record(record, &slot->message);
        const long offset = (long)(CHAT_STORE_RECORDS_OFFSET +
                                   (size_t)slot->disk_index * sizeof(*record));
        if (fseek(file, offset, SEEK_SET) != 0 ||
            fwrite(record, sizeof(*record), 1, file) != 1) {
            err = ESP_FAIL;
            break;
        }
    }
    if (err == ESP_OK) {
        err = chat_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    chat_store.storage_error = err;
    return err;
}

static esp_err_t chat_store_update_message_locked(chat_message_slot_t *slot)
{
    chat_message_slot_t *slots[] = {slot};
    return chat_store_update_messages_locked(slots, 1);
}

static esp_err_t chat_store_load_locked(void)
{
    esp_err_t err = chat_prepare_store_path();
    if (err != ESP_OK) {
        chat_store.persistent = false;
        chat_store.storage_error = err;
        return err;
    }

    struct stat info;
    if (stat(chat_store.store_path, &info) != 0) {
        if (errno != ENOENT) {
            chat_store.storage_error = ESP_FAIL;
            return ESP_FAIL;
        }
        chat_store.disk_capacity = chat_select_disk_capacity(0);
        return chat_store_reset_locked();
    }
    if (info.st_size < (off_t)CHAT_STORE_RECORDS_OFFSET ||
        info.st_size > (off_t)CHAT_STORE_MAX_BYTES) {
        SOLAR_OS_LOGW(TAG, "invalid store size %ld; resetting", (long)info.st_size);
        chat_store.disk_capacity = chat_select_disk_capacity((uint64_t)info.st_size);
        return chat_store_reset_locked();
    }

    FILE *file = fopen(chat_store.store_path, "rb");
    if (file == NULL) {
        chat_store.storage_error = ESP_FAIL;
        return ESP_FAIL;
    }
    chat_store_header_t headers[CHAT_STORE_HEADER_COPIES];
    const bool headers_read = fread(headers, sizeof(headers), 1, file) == 1;
    const bool first_valid = headers_read && chat_store_header_valid(&headers[0]);
    const bool second_valid = headers_read && chat_store_header_valid(&headers[1]);
    if (!first_valid && !second_valid) {
        fclose(file);
        chat_store.disk_capacity = chat_select_disk_capacity((uint64_t)info.st_size);
        SOLAR_OS_LOGW(TAG, "store headers invalid; resetting");
        return chat_store_reset_locked();
    }
    const chat_store_header_t *header = &headers[0];
    if (!first_valid ||
        (second_valid && chat_generation_newer(headers[1].generation,
                                               headers[0].generation))) {
        header = &headers[1];
    }

    chat_store.disk_capacity = header->capacity;
    chat_store.disk_head = header->head;
    chat_store.disk_count = header->count;
    chat_store.disk_generation = header->generation;
    chat_store.next_message_id = header->next_id;
    chat_store.message_dropped_count = header->dropped;
    const size_t oldest =
        (header->head + header->capacity - header->count) % header->capacity;
    bool records_valid = true;
    for (size_t i = 0; i < header->count; i++) {
        const size_t disk_index = (oldest + i) % header->capacity;
        const long offset = (long)(CHAT_STORE_RECORDS_OFFSET +
                                   disk_index * sizeof(chat_store_record_t));
        chat_store_record_t *record = chat_store.record_scratch;
        if (fseek(file, offset, SEEK_SET) != 0 ||
            fread(record, sizeof(*record), 1, file) != 1 ||
            !chat_store_record_valid(record)) {
            records_valid = false;
            break;
        }
        chat_message_slot_t *slot = &chat_store.messages[chat_store.message_head];
        memset(slot, 0, sizeof(*slot));
        slot->message = record->message;
        slot->disk_index = (int16_t)disk_index;
        chat_store.message_head =
            (chat_store.message_head + 1U) % SOLAR_OS_CHAT_MESSAGE_CAPACITY;
        if (chat_store.message_count < SOLAR_OS_CHAT_MESSAGE_CAPACITY) {
            chat_store.message_count++;
        }
        if (slot->message.unread) {
            chat_store.message_unread++;
        }
    }
    fclose(file);
    if (!records_valid) {
        memset(chat_store.messages,
               0,
               SOLAR_OS_CHAT_MESSAGE_CAPACITY * sizeof(*chat_store.messages));
        for (size_t i = 0; i < SOLAR_OS_CHAT_MESSAGE_CAPACITY; i++) {
            chat_store.messages[i].disk_index = -1;
        }
        chat_store.message_head = 0;
        chat_store.message_count = 0;
        chat_store.message_unread = 0;
        chat_store.next_message_id = 1;
        chat_store.message_dropped_count = 0;
        SOLAR_OS_LOGW(TAG, "store record invalid; resetting");
        return chat_store_reset_locked();
    }
    chat_store.persistent = true;
    chat_store.storage_error = ESP_OK;
    return ESP_OK;
}

static bool chat_string_is_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool chat_url_is_valid(const char *url)
{
    if (!chat_string_is_valid(url, SOLAR_OS_CHAT_URL_MAX, false)) {
        return false;
    }
    return strncmp(url, "chat://", 7) == 0 ||
        strncmp(url, "chats://", 8) == 0 ||
        strncmp(url, "tcp://", 6) == 0 ||
        strncmp(url, "tls://", 6) == 0;
}

static uint32_t chat_next_id(uint32_t *next)
{
    uint32_t id = (*next)++;
    if (id == 0) {
        id = (*next)++;
    }
    return id;
}

static size_t chat_message_oldest_locked(void)
{
    return (chat_store.message_head + SOLAR_OS_CHAT_MESSAGE_CAPACITY -
            chat_store.message_count) % SOLAR_OS_CHAT_MESSAGE_CAPACITY;
}

static chat_message_slot_t *chat_find_message_locked(uint64_t message_key)
{
    if (message_key == 0) {
        return NULL;
    }
    const size_t oldest = chat_message_oldest_locked();
    for (size_t i = 0; i < chat_store.message_count; i++) {
        chat_message_slot_t *slot =
            &chat_store.messages[(oldest + i) % SOLAR_OS_CHAT_MESSAGE_CAPACITY];
        if (slot->message.message_key == message_key) {
            return slot;
        }
    }
    return NULL;
}

static uint64_t chat_latest_message_timestamp_locked(void)
{
    uint64_t latest = 0;
    const size_t oldest = chat_message_oldest_locked();
    for (size_t i = 0; i < chat_store.message_count; i++) {
        const solar_os_chat_message_t *message =
            &chat_store.messages[(oldest + i) % SOLAR_OS_CHAT_MESSAGE_CAPACITY].message;
        if (message->timestamp > latest) {
            latest = message->timestamp;
        }
    }
    return latest;
}

static void chat_publish_event_locked(const solar_os_chat_event_t *event,
                                      uint64_t message_key)
{
    solar_os_chat_event_t *stored = &chat_store.events[chat_store.event_head];
    *stored = *event;
    if (message_key != 0) {
        stored->message_key = message_key;
    }
    stored->id = chat_next_id(&chat_store.next_event_id);
    chat_store.event_head =
        (chat_store.event_head + 1U) % SOLAR_OS_CHAT_STORE_CAPACITY;
    if (chat_store.event_count < SOLAR_OS_CHAT_STORE_CAPACITY) {
        chat_store.event_count++;
    } else {
        chat_store.dropped_count++;
    }
    chat_store.rx_count++;
}

static esp_err_t chat_clear_messages_locked(void)
{
    memset(chat_store.messages,
           0,
           SOLAR_OS_CHAT_MESSAGE_CAPACITY * sizeof(*chat_store.messages));
    for (size_t i = 0; i < SOLAR_OS_CHAT_MESSAGE_CAPACITY; i++) {
        chat_store.messages[i].disk_index = -1;
    }
    chat_store.message_head = 0;
    chat_store.message_count = 0;
    chat_store.message_unread = 0;
    chat_store.next_message_id = 1;
    chat_store.message_dropped_count = 0;
    if (chat_store.persistent) {
        return chat_store_reset_locked();
    }
    return ESP_OK;
}

static void chat_clear_outbox_locked(void)
{
    chat_store.outbox_head = 0;
    chat_store.outbox_count = 0;
    if (chat_store.outbox != NULL) {
        memset(chat_store.outbox,
               0,
               SOLAR_OS_CHAT_OUTBOX_CAPACITY * sizeof(*chat_store.outbox));
    }
}

static int chat_channel_index_locked(const char *channel)
{
    for (size_t i = 0; i < chat_store.channel_count; i++) {
        if (strcmp(chat_store.channels[i].name, channel) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int chat_add_channel_locked(const char *channel, bool joined)
{
    int index = chat_channel_index_locked(channel);
    if (index >= 0) {
        if (joined) {
            chat_store.channels[index].joined = true;
        }
        return index;
    }
    if (chat_store.channel_count >= SOLAR_OS_CHAT_CHANNEL_CAPACITY) {
        return -1;
    }
    index = (int)chat_store.channel_count++;
    strlcpy(chat_store.channels[index].name,
            channel,
            sizeof(chat_store.channels[index].name));
    chat_store.channels[index].joined = joined;
    return index;
}

static void chat_snapshot_config_locked(solar_os_chat_saved_config_t *config)
{
    config->enabled = chat_store.enabled;
    strlcpy(config->url, chat_store.url, sizeof(config->url));
    strlcpy(config->token, chat_store.token, sizeof(config->token));
    strlcpy(config->user, chat_store.user, sizeof(config->user));
    strlcpy(config->device, chat_store.device, sizeof(config->device));
}

static esp_err_t chat_save_config(const solar_os_chat_saved_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CHAT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(nvs, CHAT_NVS_URL_KEY, config->url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_TOKEN_KEY, config->token);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_USER_KEY, config->user);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_DEVICE_KEY, config->device);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, CHAT_NVS_ENABLED_KEY, config->enabled ? 1U : 0U);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void chat_load_config(void)
{
    strlcpy(chat_store.user, CHAT_DEFAULT_USER, sizeof(chat_store.user));
    strlcpy(chat_store.device, CHAT_DEFAULT_DEVICE, sizeof(chat_store.device));

    nvs_handle_t nvs;
    if (nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    char url[SOLAR_OS_CHAT_URL_MAX] = {0};
    char token[SOLAR_OS_CHAT_TOKEN_MAX] = {0};
    char user[SOLAR_OS_CHAT_USER_MAX] = {0};
    char device[SOLAR_OS_CHAT_DEVICE_MAX] = {0};
    size_t len = sizeof(url);
    esp_err_t ret = nvs_get_str(nvs, CHAT_NVS_URL_KEY, url, &len);
    if (ret == ESP_OK && chat_url_is_valid(url)) {
        len = sizeof(token);
        if (nvs_get_str(nvs, CHAT_NVS_TOKEN_KEY, token, &len) == ESP_ERR_NVS_NOT_FOUND) {
            token[0] = '\0';
        }
        len = sizeof(user);
        if (nvs_get_str(nvs, CHAT_NVS_USER_KEY, user, &len) != ESP_OK) {
            strlcpy(user, CHAT_DEFAULT_USER, sizeof(user));
        }
        len = sizeof(device);
        if (nvs_get_str(nvs, CHAT_NVS_DEVICE_KEY, device, &len) != ESP_OK) {
            strlcpy(device, CHAT_DEFAULT_DEVICE, sizeof(device));
        }
        if (chat_string_is_valid(token, sizeof(token), true) &&
            chat_string_is_valid(user, sizeof(user), false) &&
            chat_string_is_valid(device, sizeof(device), false)) {
            strlcpy(chat_store.url, url, sizeof(chat_store.url));
            strlcpy(chat_store.token, token, sizeof(chat_store.token));
            strlcpy(chat_store.user, user, sizeof(chat_store.user));
            strlcpy(chat_store.device, device, sizeof(chat_store.device));
            chat_store.configured = true;
            uint8_t enabled = 1U;
            const esp_err_t enabled_ret =
                nvs_get_u8(nvs, CHAT_NVS_ENABLED_KEY, &enabled);
            chat_store.enabled = enabled_ret == ESP_ERR_NVS_NOT_FOUND || enabled != 0;
            chat_store.config_revision = 1U;
        }
    }
    nvs_close(nvs);
}

static uint64_t chat_context_id_locked(void)
{
    if (!chat_store.configured || chat_store.url[0] == '\0') {
        return 0;
    }
    uint64_t hash = 1469598103934665603ULL;
    hash = chat_hash_string(hash, chat_store.url);
    return hash != 0 ? hash : 1U;
}

static void chat_restore_inbox_fallback(void)
{
    solar_os_inbox_status_t status;
    if (solar_os_inbox_get_status(&status) != ESP_OK || !status.persistent) {
        return;
    }

    solar_os_inbox_entry_t *entries = solar_os_memory_calloc(
        SOLAR_OS_INBOX_CAPACITY,
        sizeof(*entries),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "chat.inbox.restore");
    if (entries == NULL) {
        return;
    }
    const size_t count = solar_os_inbox_snapshot(entries,
                                                 SOLAR_OS_INBOX_CAPACITY,
                                                 false,
                                                 NULL);
    const uint64_t context_id = chat_context_id_locked();
    for (size_t reverse = count; reverse > 0; reverse--) {
        const solar_os_inbox_entry_t *entry = &entries[reverse - 1U];
        if (strcmp(entry->source, "chat") != 0 || entry->source_id == 0 ||
            entry->source_context != context_id) {
            continue;
        }
        chat_message_slot_t *slot = &chat_store.messages[chat_store.message_head];
        memset(slot, 0, sizeof(*slot));
        slot->disk_index = -1;
        slot->message.id = chat_next_id(&chat_store.next_message_id);
        slot->message.inbox_id = entry->id;
        slot->message.message_key = entry->source_id;
        /* Inbox fallback stores receipt time, not the transport timestamp. */
        slot->message.timestamp = 0;
        slot->message.unread = entry->unread;
        slot->message.truncated = entry->truncated;
        strlcpy(slot->message.channel,
                entry->topic[0] != '\0' ? entry->topic : CHAT_DEFAULT_CHANNEL,
                sizeof(slot->message.channel));
        strlcpy(slot->message.from, entry->sender, sizeof(slot->message.from));
        strlcpy(slot->message.text, entry->body, sizeof(slot->message.text));
        chat_store.message_head =
            (chat_store.message_head + 1U) % SOLAR_OS_CHAT_MESSAGE_CAPACITY;
        if (chat_store.message_count < SOLAR_OS_CHAT_MESSAGE_CAPACITY) {
            chat_store.message_count++;
        }
        if (slot->message.unread) {
            chat_store.message_unread++;
        }
    }
    solar_os_memory_free(entries);
    chat_store.inbox_backed = true;
    chat_store.inbox_capacity = status.capacity;
    chat_store.inbox_limit_bytes = status.storage_limit_bytes;
    chat_store.storage_error = status.storage_error;
}

static esp_err_t chat_queue_command_locked(solar_os_chat_command_type_t type,
                                           const char *channel,
                                           const char *text)
{
    if (chat_store.outbox_count >= SOLAR_OS_CHAT_OUTBOX_CAPACITY) {
        chat_store.dropped_count++;
        return ESP_ERR_NO_MEM;
    }
    solar_os_chat_command_t *command = &chat_store.outbox[chat_store.outbox_head];
    memset(command, 0, sizeof(*command));
    command->id = chat_next_id(&chat_store.next_command_id);
    command->type = type;
    if (channel != NULL) {
        strlcpy(command->channel, channel, sizeof(command->channel));
    }
    if (text != NULL) {
        strlcpy(command->text, text, sizeof(command->text));
    }
    chat_store.outbox_head =
        (chat_store.outbox_head + 1U) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
    chat_store.outbox_count++;
    return ESP_OK;
}

esp_err_t solar_os_chat_init(void)
{
    if (chat_store.initialized) {
        return ESP_OK;
    }
    chat_store.lock = xSemaphoreCreateMutex();
    if (chat_store.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    chat_store.events = solar_os_memory_calloc(SOLAR_OS_CHAT_STORE_CAPACITY,
                                               sizeof(*chat_store.events),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "chat.store");
    chat_store.messages = solar_os_memory_calloc(SOLAR_OS_CHAT_MESSAGE_CAPACITY,
                                                 sizeof(*chat_store.messages),
                                                 SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                 "chat.messages");
    chat_store.record_scratch = solar_os_memory_calloc(1,
                                                       sizeof(*chat_store.record_scratch),
                                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                       "chat.record");
    chat_store.outbox = solar_os_memory_calloc(SOLAR_OS_CHAT_OUTBOX_CAPACITY,
                                               sizeof(*chat_store.outbox),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "chat.outbox");
    if (chat_store.events == NULL || chat_store.messages == NULL ||
        chat_store.record_scratch == NULL || chat_store.outbox == NULL) {
        solar_os_memory_free(chat_store.events);
        solar_os_memory_free(chat_store.messages);
        solar_os_memory_free(chat_store.record_scratch);
        solar_os_memory_free(chat_store.outbox);
        chat_store.events = NULL;
        chat_store.messages = NULL;
        chat_store.record_scratch = NULL;
        chat_store.outbox = NULL;
        vSemaphoreDelete(chat_store.lock);
        chat_store.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < SOLAR_OS_CHAT_MESSAGE_CAPACITY; i++) {
        chat_store.messages[i].disk_index = -1;
    }
    chat_store.next_event_id = 1U;
    chat_store.next_message_id = 1U;
    chat_store.next_command_id = 1U;
    chat_load_config();
    (void)chat_add_channel_locked(CHAT_DEFAULT_CHANNEL, true);
    const esp_err_t store_err = chat_store_load_locked();
    if (store_err != ESP_OK) {
        chat_restore_inbox_fallback();
        if (!chat_store.inbox_backed) {
            SOLAR_OS_LOGW(TAG,
                          "persistent message store unavailable: %s",
                          esp_err_to_name(store_err));
        }
    }
    chat_store.initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token,
                                  const char *user,
                                  const char *device)
{
    if ((url != NULL && url[0] != '\0' && !chat_url_is_valid(url)) ||
        (token != NULL &&
         !chat_string_is_valid(token, SOLAR_OS_CHAT_TOKEN_MAX, true)) ||
        (user != NULL &&
         !chat_string_is_valid(user, SOLAR_OS_CHAT_USER_MAX, false)) ||
        (device != NULL &&
         !chat_string_is_valid(device, SOLAR_OS_CHAT_DEVICE_MAX, false))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    bool endpoint_changed = false;
    if (url != NULL && url[0] != '\0') {
        endpoint_changed = strcmp(chat_store.url, url) != 0;
        strlcpy(chat_store.url, url, sizeof(chat_store.url));
        chat_store.configured = true;
    }
    if (token != NULL) {
        strlcpy(chat_store.token, token, sizeof(chat_store.token));
    }
    if (user != NULL) {
        strlcpy(chat_store.user, user, sizeof(chat_store.user));
    }
    if (device != NULL) {
        strlcpy(chat_store.device, device, sizeof(chat_store.device));
    }
    if (!chat_store.configured || !chat_url_is_valid(chat_store.url)) {
        chat_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (endpoint_changed) {
        chat_clear_outbox_locked();
        chat_store.event_head = 0;
        chat_store.event_count = 0;
        memset(chat_store.events,
               0,
               SOLAR_OS_CHAT_STORE_CAPACITY * sizeof(*chat_store.events));
        const esp_err_t clear_err = chat_clear_messages_locked();
        if (clear_err != ESP_OK) {
            chat_store.persistent = false;
            chat_store.storage_error = clear_err;
        }
    }
    chat_store.config_revision++;
    if (chat_store.config_revision == 0) {
        chat_store.config_revision = 1U;
    }
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    ret = chat_save_config(&config);
    if (ret != ESP_OK) {
        chat_lock();
        chat_store.last_esp_error = ret;
        strlcpy(chat_store.last_error,
                esp_err_to_name(ret),
                sizeof(chat_store.last_error));
        chat_unlock();
    }
    return ret;
}

esp_err_t solar_os_chat_connect(const char *url,
                                const char *token,
                                const char *user,
                                const char *device)
{
    esp_err_t ret = solar_os_chat_configure(url, token, user, device);
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    chat_store.enabled = true;
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    return chat_save_config(&config);
}

esp_err_t solar_os_chat_disconnect(void)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    chat_store.enabled = false;
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    return chat_save_config(&config);
}

esp_err_t solar_os_chat_join(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    if (index < 0 && chat_store.channel_count >= SOLAR_OS_CHAT_CHANNEL_CAPACITY) {
        ret = ESP_ERR_NO_MEM;
    } else {
        ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_JOIN, channel, NULL);
        if (ret == ESP_OK) {
            (void)chat_add_channel_locked(channel, true);
        }
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_leave(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_LEAVE, channel, NULL);
    if (ret == ESP_OK && index >= 0) {
        chat_store.channels[index].joined = false;
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_delete_channel(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL, channel, NULL);
    if (ret == ESP_OK && index >= 0) {
        chat_store.channels[index].joined = false;
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_send(const char *channel, const char *text)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false) ||
        text == NULL || text[0] == '\0' || strlen(text) >= SOLAR_OS_CHAT_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_MESSAGE, channel, text);
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_read_event_after(uint32_t *cursor,
                                         solar_os_chat_event_t *event)
{
    if (cursor == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ESP_ERR_NOT_FOUND;
    chat_lock();
    const size_t oldest = chat_store.event_count == SOLAR_OS_CHAT_STORE_CAPACITY ?
        chat_store.event_head : 0U;
    for (size_t logical = 0; logical < chat_store.event_count; logical++) {
        const size_t index = (oldest + logical) % SOLAR_OS_CHAT_STORE_CAPACITY;
        if (*cursor == 0 || chat_store.events[index].id > *cursor) {
            *event = chat_store.events[index];
            *cursor = event->id;
            ret = ESP_OK;
            break;
        }
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_read_event(solar_os_chat_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        esp_err_t ret = solar_os_chat_read_event_after(&chat_store.legacy_cursor, event);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (timeout_ms == 0 || xTaskGetTickCount() - start >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(CHAT_EVENT_WAIT_POLL_MS));
    }
}

size_t solar_os_chat_message_visit(solar_os_chat_message_visitor_t visitor,
                                   void *user,
                                   uint32_t *event_cursor)
{
    if (visitor == NULL || solar_os_chat_init() != ESP_OK) {
        return 0;
    }

    chat_lock();
    /* Transport timestamps are not necessarily wall-clock values and may reset
     * when a gateway restarts. The ring order is the stable receipt order. */
    const size_t oldest = chat_message_oldest_locked();
    if (event_cursor != NULL) {
        *event_cursor = chat_store.next_event_id > 1U ?
            chat_store.next_event_id - 1U : 0U;
    }
    size_t visited = 0;
    for (; visited < chat_store.message_count; visited++) {
        chat_message_slot_t *slot =
            &chat_store.messages[(oldest + visited) % SOLAR_OS_CHAT_MESSAGE_CAPACITY];
        if (!visitor(&slot->message, user)) {
            break;
        }
    }
    chat_unlock();
    return visited;
}

esp_err_t solar_os_chat_mark_message_read(uint64_t message_key, bool read)
{
    if (message_key == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_chat_init();
    if (err != ESP_OK) {
        return err;
    }

    uint32_t inbox_id = 0;
    chat_lock();
    chat_message_slot_t *slot = chat_find_message_locked(message_key);
    if (slot == NULL) {
        chat_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const bool unread = !read;
    if (slot->message.unread != unread) {
        const bool previous = slot->message.unread;
        slot->message.unread = unread;
        if (unread) {
            chat_store.message_unread++;
        } else if (chat_store.message_unread > 0) {
            chat_store.message_unread--;
        }
        if (slot->disk_index >= 0 &&
            chat_store_update_message_locked(slot) != ESP_OK) {
            slot->message.unread = previous;
            if (previous) {
                chat_store.message_unread++;
            } else if (chat_store.message_unread > 0) {
                chat_store.message_unread--;
            }
            err = chat_store.storage_error;
        }
    }
    if (err == ESP_OK) {
        inbox_id = slot->message.inbox_id;
    }
    chat_unlock();
    if (err == ESP_OK && inbox_id != 0) {
        (void)solar_os_inbox_mark_read(inbox_id, read);
    }
    return err;
}

esp_err_t solar_os_chat_mark_channel_read(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_chat_init();
    if (err != ESP_OK) {
        return err;
    }

    chat_message_slot_t *changed[SOLAR_OS_CHAT_MESSAGE_CAPACITY];
    uint32_t inbox_ids[SOLAR_OS_CHAT_MESSAGE_CAPACITY];
    size_t changed_count = 0;
    size_t inbox_count = 0;
    chat_lock();
    const size_t oldest = chat_message_oldest_locked();
    for (size_t i = 0; i < chat_store.message_count; i++) {
        chat_message_slot_t *slot =
            &chat_store.messages[(oldest + i) % SOLAR_OS_CHAT_MESSAGE_CAPACITY];
        if (!slot->message.unread || strcmp(slot->message.channel, channel) != 0) {
            continue;
        }
        slot->message.unread = false;
        if (chat_store.message_unread > 0) {
            chat_store.message_unread--;
        }
        changed[changed_count++] = slot;
        if (slot->message.inbox_id != 0) {
            inbox_ids[inbox_count++] = slot->message.inbox_id;
        }
    }
    if (changed_count > 0 && chat_store.persistent &&
        chat_store_update_messages_locked(changed, changed_count) != ESP_OK) {
        for (size_t i = 0; i < changed_count; i++) {
            changed[i]->message.unread = true;
            chat_store.message_unread++;
        }
        err = chat_store.storage_error;
        inbox_count = 0;
    }
    chat_unlock();
    for (size_t i = 0; i < inbox_count; i++) {
        (void)solar_os_inbox_mark_read(inbox_ids[i], true);
    }
    return err;
}

esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = chat_store.initialized;
    status->configured = chat_store.configured;
    status->enabled = chat_store.enabled;
    status->running = chat_store.sync_running;
    status->connected = chat_store.connected;
    status->token_set = chat_store.token[0] != '\0';
    status->state = chat_store.connected ? SOLAR_OS_CHAT_STATE_CONNECTED :
        (chat_store.sync_running && chat_store.enabled && chat_store.configured ?
             SOLAR_OS_CHAT_STATE_CONNECTING : SOLAR_OS_CHAT_STATE_DISCONNECTED);
    strlcpy(status->url, chat_store.url, sizeof(status->url));
    strlcpy(status->user, chat_store.user, sizeof(status->user));
    strlcpy(status->device, chat_store.device, sizeof(status->device));
    strlcpy(status->last_error, chat_store.last_error, sizeof(status->last_error));
    status->last_esp_error = chat_store.last_esp_error;
    status->config_revision = chat_store.config_revision;
    status->rx_count = chat_store.rx_count;
    status->tx_count = chat_store.tx_count;
    status->dropped_count = chat_store.dropped_count;
    status->queued_events = chat_store.event_count;
    status->queued_outbox = chat_store.outbox_count;
    status->stored_messages = chat_store.message_count;
    status->unread_messages = chat_store.message_unread;
    status->persistent_capacity = chat_store.persistent ? chat_store.disk_capacity :
        chat_store.inbox_capacity;
    status->persistent_limit_bytes = chat_store.persistent ?
        CHAT_STORE_RECORDS_OFFSET +
            chat_store.disk_capacity * sizeof(chat_store_record_t) :
        chat_store.inbox_limit_bytes;
    status->persistent = chat_store.persistent || chat_store.inbox_backed;
    status->persistent_inbox_backed = chat_store.inbox_backed;
    status->storage_error = chat_store.storage_error;
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_get_config(solar_os_chat_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    memset(config, 0, sizeof(*config));
    config->configured = chat_store.configured;
    config->enabled = chat_store.enabled;
    config->revision = chat_store.config_revision;
    strlcpy(config->url, chat_store.url, sizeof(config->url));
    strlcpy(config->token, chat_store.token, sizeof(config->token));
    strlcpy(config->user, chat_store.user, sizeof(config->user));
    strlcpy(config->device, chat_store.device, sizeof(config->device));
    chat_unlock();
    return ESP_OK;
}

size_t solar_os_chat_channel_snapshot(solar_os_chat_channel_t *channels,
                                      size_t max_channels)
{
    if (solar_os_chat_init() != ESP_OK || channels == NULL || max_channels == 0) {
        return 0;
    }
    chat_lock();
    const size_t count = chat_store.channel_count < max_channels ?
        chat_store.channel_count : max_channels;
    memcpy(channels, chat_store.channels, count * sizeof(*channels));
    chat_unlock();
    return count;
}

esp_err_t solar_os_chat_sync_set_status(bool running,
                                        bool connected,
                                        esp_err_t error,
                                        const char *message)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    chat_store.sync_running = running;
    chat_store.connected = connected;
    chat_store.last_esp_error = error;
    if (error == ESP_OK) {
        chat_store.last_error[0] = '\0';
    } else {
        strlcpy(chat_store.last_error,
                message != NULL ? message : esp_err_to_name(error),
                sizeof(chat_store.last_error));
    }
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_sync_publish(const solar_os_chat_event_t *event,
                                     bool *inserted,
                                     uint64_t *message_key)
{
    if (inserted != NULL) {
        *inserted = false;
    }
    if (message_key != NULL) {
        *message_key = 0;
    }
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    if (event->type != SOLAR_OS_CHAT_EVENT_MESSAGE) {
        chat_publish_event_locked(event, 0);
        chat_unlock();
        return ESP_OK;
    }

    uint64_t key = chat_message_key_locked(event);
    if (key == 0) {
        uint64_t hash = 1469598103934665603ULL;
        hash = chat_hash_string(hash, chat_store.url);
        hash = chat_hash_string(hash, event->channel);
        hash = chat_hash_string(hash, event->from);
        hash = chat_hash_string(hash, event->text);
        hash = chat_hash_update(hash,
                                &chat_store.next_message_id,
                                sizeof(chat_store.next_message_id));
        key = hash != 0 ? hash : 1U;
    }
    if (message_key != NULL) {
        *message_key = key;
    }
    if (chat_find_message_locked(key) != NULL) {
        chat_unlock();
        return ESP_OK;
    }

    const uint64_t latest_timestamp = chat_latest_message_timestamp_locked();
    const bool backfill = event->timestamp != 0 && latest_timestamp != 0 &&
        event->timestamp < latest_timestamp;
    const size_t write_index = chat_store.message_head;
    const uint32_t previous_next_id = chat_store.next_message_id;
    const size_t previous_disk_head = chat_store.disk_head;
    const size_t previous_disk_count = chat_store.disk_count;
    const uint32_t previous_generation = chat_store.disk_generation;

    solar_os_chat_message_t *incoming = &chat_store.record_scratch->message;
    memset(chat_store.record_scratch, 0, sizeof(*chat_store.record_scratch));
    incoming->id = chat_next_id(&chat_store.next_message_id);
    incoming->message_key = key;
    incoming->timestamp = event->timestamp;
    incoming->unread = true;
    incoming->truncated = event->truncated;
    strlcpy(incoming->channel,
            event->channel[0] != '\0' ? event->channel : CHAT_DEFAULT_CHANNEL,
            sizeof(incoming->channel));
    strlcpy(incoming->from, event->from, sizeof(incoming->from));
    strlcpy(incoming->text, event->text, sizeof(incoming->text));

    if (chat_store.persistent && !backfill &&
        chat_store_write_message_locked(incoming) < 0) {
        chat_store.next_message_id = previous_next_id;
        chat_store.disk_head = previous_disk_head;
        chat_store.disk_count = previous_disk_count;
        chat_store.disk_generation = previous_generation;
        ret = chat_store.storage_error;
        chat_unlock();
        return ret;
    }

    chat_message_slot_t *slot = &chat_store.messages[write_index];
    if (chat_store.message_count == SOLAR_OS_CHAT_MESSAGE_CAPACITY) {
        if (slot->message.unread && chat_store.message_unread > 0) {
            chat_store.message_unread--;
        }
        chat_store.message_dropped_count++;
    }
    slot->message = *incoming;
    slot->disk_index = !backfill && chat_store.persistent ?
        (int16_t)((chat_store.disk_head + chat_store.disk_capacity - 1U) %
                  chat_store.disk_capacity) : -1;
    chat_store.message_head =
        (chat_store.message_head + 1U) % SOLAR_OS_CHAT_MESSAGE_CAPACITY;
    if (chat_store.message_count < SOLAR_OS_CHAT_MESSAGE_CAPACITY) {
        chat_store.message_count++;
    }
    chat_store.message_unread++;

    if (!backfill) {
        chat_publish_event_locked(event, key);
        if (inserted != NULL) {
            *inserted = true;
        }
    }
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_sync_set_inbox_id(uint64_t message_key,
                                          uint32_t inbox_id)
{
    if (message_key == 0 || inbox_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_chat_init();
    if (err != ESP_OK) {
        return err;
    }

    bool already_read = false;
    chat_lock();
    chat_message_slot_t *slot = chat_find_message_locked(message_key);
    if (slot == NULL) {
        chat_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t previous = slot->message.inbox_id;
    slot->message.inbox_id = inbox_id;
    if (slot->disk_index >= 0 &&
        chat_store_update_message_locked(slot) != ESP_OK) {
        slot->message.inbox_id = previous;
        err = chat_store.storage_error;
    } else {
        already_read = !slot->message.unread;
    }
    chat_unlock();
    if (err == ESP_OK && already_read) {
        (void)solar_os_inbox_mark_read(inbox_id, true);
    }
    return err;
}

uint64_t solar_os_chat_context_id(void)
{
    if (solar_os_chat_init() != ESP_OK) {
        return 0;
    }
    chat_lock();
    const uint64_t context_id = chat_context_id_locked();
    chat_unlock();
    return context_id;
}

esp_err_t solar_os_chat_outbox_peek(solar_os_chat_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    if (chat_store.outbox_count == 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        const size_t oldest =
            (chat_store.outbox_head + SOLAR_OS_CHAT_OUTBOX_CAPACITY -
             chat_store.outbox_count) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
        *command = chat_store.outbox[oldest];
        ret = ESP_OK;
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_outbox_ack(uint32_t id)
{
    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    if (chat_store.outbox_count == 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        const size_t oldest =
            (chat_store.outbox_head + SOLAR_OS_CHAT_OUTBOX_CAPACITY -
             chat_store.outbox_count) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
        if (chat_store.outbox[oldest].id != id) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            memset(&chat_store.outbox[oldest], 0, sizeof(chat_store.outbox[oldest]));
            chat_store.outbox_count--;
            chat_store.tx_count++;
            ret = ESP_OK;
        }
    }
    chat_unlock();
    return ret;
}

const char *solar_os_chat_state_name(solar_os_chat_state_t state)
{
    switch (state) {
    case SOLAR_OS_CHAT_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_CHAT_STATE_CONNECTED:
        return "connected";
    case SOLAR_OS_CHAT_STATE_DISCONNECTED:
    default:
        return "disconnected";
    }
}

const char *solar_os_chat_event_type_name(solar_os_chat_event_type_t type)
{
    switch (type) {
    case SOLAR_OS_CHAT_EVENT_CONNECTED:
        return "connected";
    case SOLAR_OS_CHAT_EVENT_DISCONNECTED:
        return "disconnected";
    case SOLAR_OS_CHAT_EVENT_ERROR:
        return "error";
    case SOLAR_OS_CHAT_EVENT_CHANNEL:
        return "channel";
    case SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED:
        return "deleted";
    case SOLAR_OS_CHAT_EVENT_JOINED:
        return "joined";
    case SOLAR_OS_CHAT_EVENT_LEFT:
        return "left";
    case SOLAR_OS_CHAT_EVENT_MESSAGE:
        return "message";
    case SOLAR_OS_CHAT_EVENT_PRESENCE:
        return "presence";
    case SOLAR_OS_CHAT_EVENT_COMMAND_SENT:
        return "sent";
    case SOLAR_OS_CHAT_EVENT_RAW:
    default:
        return "raw";
    }
}
