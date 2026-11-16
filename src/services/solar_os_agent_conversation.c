#include "solar_os_agent_conversation.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define AGENT_CONVERSATION_DIRECTORY ".solar/agent/conversations"
#define AGENT_CONVERSATION_MAGIC 0x56434f53UL
#define AGENT_CONVERSATION_RECORD_MAGIC 0x52434f53UL
#define AGENT_CONVERSATION_VERSION 1U
#define AGENT_CONVERSATION_RECORD_MAX 64U
#define AGENT_CONVERSATION_FLASH_COUNT 3U
#define AGENT_CONVERSATION_SD_COUNT 8U
#define AGENT_CONVERSATION_SLOT_MAX 8U
#define AGENT_CONVERSATION_FLASH_BYTES (10U * 1024U)
#define AGENT_CONVERSATION_SD_BYTES (48U * 1024U)
#define AGENT_CONVERSATION_FLASH_MESSAGE_MAX 4096U
#define AGENT_CONVERSATION_SD_MESSAGE_MAX (16U * 1024U)
#define AGENT_CONVERSATION_HISTORY_BYTES_MAX (8U * 1024U)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t created_at;
    uint64_t updated_at;
    uint16_t turn_count;
    uint16_t record_count;
    uint32_t stored_bytes;
    char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    char title[SOLAR_OS_AGENT_CONVERSATION_TITLE_MAX];
    char provider[24];
    char model[SOLAR_OS_AGENT_MODEL_MAX];
    char provider_response_id[96];
    uint32_t crc32;
} agent_conversation_header_t;

typedef struct {
    uint32_t magic;
    uint8_t role;
    uint8_t reserved;
    uint16_t text_len;
    uint64_t timestamp;
    uint32_t crc32;
} agent_conversation_record_t;

typedef struct {
    long offset;
    uint32_t total_size;
    agent_conversation_record_t header;
} agent_conversation_record_meta_t;

static SemaphoreHandle_t conversation_mutex;
static portMUX_TYPE conversation_mutex_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t conversation_take_mutex(void)
{
    if (conversation_mutex == NULL) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (created == NULL) {
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&conversation_mutex_lock);
        if (conversation_mutex == NULL) {
            conversation_mutex = created;
            created = NULL;
        }
        portEXIT_CRITICAL(&conversation_mutex_lock);
        if (created != NULL) {
            vSemaphoreDelete(created);
        }
    }
    return xSemaphoreTake(conversation_mutex, portMAX_DELAY) == pdTRUE ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

static void conversation_give_mutex(void)
{
    if (conversation_mutex != NULL) {
        (void)xSemaphoreGive(conversation_mutex);
    }
}

static uint32_t conversation_crc32_update(uint32_t crc,
                                          const void *data,
                                          size_t len)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; bit++) {
            crc = (crc & 1U) != 0 ?
                (crc >> 1) ^ 0xedb88320UL : crc >> 1;
        }
    }
    return crc;
}

static uint32_t conversation_header_crc(
    const agent_conversation_header_t *header)
{
    return ~conversation_crc32_update(
        0xffffffffUL,
        header,
        offsetof(agent_conversation_header_t, crc32));
}

static uint32_t conversation_record_crc(
    const agent_conversation_record_t *record,
    const void *text)
{
    uint32_t crc = conversation_crc32_update(
        0xffffffffUL,
        record,
        offsetof(agent_conversation_record_t, crc32));
    crc = conversation_crc32_update(crc, text, record->text_len);
    return ~crc;
}

static bool conversation_id_valid(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    const size_t len = strnlen(id, SOLAR_OS_AGENT_CONVERSATION_ID_MAX);
    if (len == 0 || len >= SOLAR_OS_AGENT_CONVERSATION_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char ch = id[i];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t conversation_directory(char *path, size_t path_len)
{
    return solar_os_storage_default_path(AGENT_CONVERSATION_DIRECTORY,
                                         path,
                                         path_len);
}

static esp_err_t conversation_ensure_directory(void)
{
    static const char *const parts[] = {
        ".solar",
        ".solar/agent",
        AGENT_CONVERSATION_DIRECTORY,
    };
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        esp_err_t err =
            solar_os_storage_default_path(parts[i], path, sizeof(path));
        if (err != ESP_OK) {
            return err;
        }
        if (solar_os_storage_mkdir(path) != ESP_OK && errno != EEXIST) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t conversation_path(const char *id,
                                   const char *suffix,
                                   char *path,
                                   size_t path_len)
{
    if (!conversation_id_valid(id) || suffix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char relative[SOLAR_OS_STORAGE_PATH_MAX];
    const int written = snprintf(relative,
                                 sizeof(relative),
                                 "%s/%s.conv%s",
                                 AGENT_CONVERSATION_DIRECTORY,
                                 id,
                                 suffix);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_storage_default_path(relative, path, path_len);
}

static bool conversation_header_valid(
    const agent_conversation_header_t *header)
{
    return header != NULL &&
        header->magic == AGENT_CONVERSATION_MAGIC &&
        header->version == AGENT_CONVERSATION_VERSION &&
        header->header_size == sizeof(*header) &&
        header->record_count <= AGENT_CONVERSATION_RECORD_MAX &&
        conversation_id_valid(header->id) &&
        header->crc32 == conversation_header_crc(header);
}

static esp_err_t conversation_read_header(FILE *file,
                                          agent_conversation_header_t *header)
{
    if (file == NULL || header == NULL ||
        fseek(file, 0, SEEK_SET) != 0 ||
        fread(header, sizeof(*header), 1, file) != 1 ||
        !conversation_header_valid(header)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static FILE *conversation_open_read(const char *id,
                                    agent_conversation_header_t *header,
                                    char *opened_path,
                                    size_t opened_path_len)
{
    static const char *const suffixes[] = {"", ".bak"};
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (conversation_path(id, suffixes[i], path, sizeof(path)) != ESP_OK) {
            return NULL;
        }
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            continue;
        }
        if (conversation_read_header(file, header) == ESP_OK &&
            strcmp(header->id, id) == 0) {
            if (opened_path != NULL && opened_path_len > 0) {
                strlcpy(opened_path, path, opened_path_len);
            }
            return file;
        }
        fclose(file);
    }
    return NULL;
}

static esp_err_t conversation_read_records(
    FILE *file,
    const agent_conversation_header_t *header,
    agent_conversation_record_meta_t *records,
    size_t capacity,
    size_t *count)
{
    if (file == NULL || header == NULL || records == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (fseek(file, (long)sizeof(*header), SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    char *text = solar_os_memory_alloc(
        AGENT_CONVERSATION_SD_MESSAGE_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.conversation.verify");
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    while (*count < capacity && *count < header->record_count) {
        const long offset = ftell(file);
        agent_conversation_record_t record;
        if (offset < 0 || fread(&record, sizeof(record), 1, file) != 1 ||
            record.magic != AGENT_CONVERSATION_RECORD_MAGIC ||
            record.role > SOLAR_OS_AGENT_MESSAGE_TOOL ||
            record.text_len == 0 ||
            record.text_len > AGENT_CONVERSATION_SD_MESSAGE_MAX ||
            fread(text, record.text_len, 1, file) != 1 ||
            record.crc32 != conversation_record_crc(&record, text)) {
            break;
        }
        records[*count] = (agent_conversation_record_meta_t){
            .offset = offset,
            .total_size = sizeof(record) + record.text_len,
            .header = record,
        };
        (*count)++;
    }
    solar_os_memory_free(text);
    return ESP_OK;
}

static size_t conversation_message_limit(void)
{
    return solar_os_storage_sd_is_mounted() ?
        AGENT_CONVERSATION_SD_MESSAGE_MAX :
        AGENT_CONVERSATION_FLASH_MESSAGE_MAX;
}

static size_t conversation_file_limit(void)
{
    return solar_os_storage_sd_is_mounted() ?
        AGENT_CONVERSATION_SD_BYTES :
        AGENT_CONVERSATION_FLASH_BYTES;
}

static size_t conversation_count_limit(void)
{
    return solar_os_storage_sd_is_mounted() ?
        AGENT_CONVERSATION_SD_COUNT :
        AGENT_CONVERSATION_FLASH_COUNT;
}

static size_t conversation_bounded_length(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    const size_t len = strlen(text);
    const size_t limit = conversation_message_limit();
    return len < limit ? len : limit;
}

static void conversation_make_title(const char *prompt,
                                    char *title,
                                    size_t title_len)
{
    size_t used = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)prompt;
         *p != '\0' && used + 1U < title_len;
         p++) {
        if (used && (*p == '\r' || *p == '\n')) break;
        if (*p == '\r' || *p == '\n' || *p == '\t' || *p == ' ') {
            space = used > 0;
            continue;
        }
        if (space && used + 1U < title_len) {
            title[used++] = ' ';
        }
        space = false;
        title[used++] = (char)*p;
    }
    title[used] = '\0';
}

static bool conversation_slot_number(const char *id, size_t *slot)
{
    if (id == NULL || id[0] < '1' || id[0] > '8' || id[1] != '\0') {
        return false;
    }
    if (slot != NULL) {
        *slot = (size_t)(id[0] - '0');
    }
    return true;
}

static esp_err_t conversation_write_record(FILE *file,
                                           solar_os_agent_message_role_t role,
                                           const char *text,
                                           size_t text_len,
                                           uint64_t timestamp)
{
    if (file == NULL || text == NULL || text_len == 0 ||
        text_len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_conversation_record_t record = {
        .magic = AGENT_CONVERSATION_RECORD_MAGIC,
        .role = (uint8_t)role,
        .text_len = (uint16_t)text_len,
        .timestamp = timestamp,
    };
    record.crc32 = conversation_record_crc(&record, text);
    return fwrite(&record, sizeof(record), 1, file) == 1 &&
        fwrite(text, text_len, 1, file) == 1 ? ESP_OK : ESP_FAIL;
}

static esp_err_t conversation_copy_record(
    FILE *source,
    FILE *destination,
    const agent_conversation_record_meta_t *record)
{
    if (fseek(source, record->offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    uint8_t buffer[256];
    uint32_t remaining = record->total_size;
    while (remaining > 0) {
        const size_t chunk =
            remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (fread(buffer, chunk, 1, source) != 1 ||
            fwrite(buffer, chunk, 1, destination) != 1) {
            return ESP_FAIL;
        }
        remaining -= chunk;
    }
    return ESP_OK;
}

static esp_err_t conversation_sync(FILE *file)
{
    return fflush(file) == 0 && fileno(file) >= 0 &&
        fsync(fileno(file)) == 0 ? ESP_OK : ESP_FAIL;
}

static int conversation_info_compare(const void *left, const void *right)
{
    const solar_os_agent_conversation_info_t *a = left;
    const solar_os_agent_conversation_info_t *b = right;
    if (a->updated_at != b->updated_at) {
        return a->updated_at < b->updated_at ? 1 : -1;
    }
    return strcmp(b->id, a->id);
}

static void conversation_header_to_info(
    const agent_conversation_header_t *header,
    solar_os_agent_conversation_info_t *info)
{
    memset(info, 0, sizeof(*info));
    strlcpy(info->id, header->id, sizeof(info->id));
    strlcpy(info->title, header->title, sizeof(info->title));
    strlcpy(info->provider, header->provider, sizeof(info->provider));
    strlcpy(info->model, header->model, sizeof(info->model));
    info->created_at = header->created_at;
    info->updated_at = header->updated_at;
    info->turn_count = header->turn_count;
    info->stored_bytes = header->stored_bytes;
}

static esp_err_t conversation_allocate_slot(char *id, size_t id_len)
{
    if (id == NULL || id_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_agent_conversation_info_t items[AGENT_CONVERSATION_SLOT_MAX];
    size_t count = 0;
    esp_err_t err = solar_os_agent_conversations_list(
        items,
        AGENT_CONVERSATION_SLOT_MAX,
        &count);
    if (err != ESP_OK) {
        return err;
    }

    bool used[AGENT_CONVERSATION_SLOT_MAX + 1U] = {false};
    for (size_t i = 0; i < count; i++) {
        size_t slot = 0;
        if (conversation_slot_number(items[i].id, &slot)) {
            used[slot] = true;
        }
    }
    const size_t limit = conversation_count_limit();
    for (size_t slot = 1; slot <= limit; slot++) {
        if (!used[slot]) {
            id[0] = (char)('0' + slot);
            id[1] = '\0';
            return ESP_OK;
        }
    }

    for (size_t i = count; i > 0; i--) {
        if (conversation_slot_number(items[i - 1U].id, NULL)) {
            strlcpy(id, items[i - 1U].id, id_len);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static bool conversation_filename_id(const char *name,
                                     char *id,
                                     size_t id_len)
{
    static const char *const suffixes[] = {".conv.bak", ".conv"};
    if (name == NULL || id == NULL || id_len == 0) {
        return false;
    }
    const size_t name_len = strlen(name);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        const size_t suffix_len = strlen(suffixes[i]);
        if (name_len <= suffix_len ||
            strcmp(name + name_len - suffix_len, suffixes[i]) != 0) {
            continue;
        }
        const size_t value_len = name_len - suffix_len;
        if (value_len >= id_len) {
            return false;
        }
        memcpy(id, name, value_len);
        id[value_len] = '\0';
        return conversation_id_valid(id);
    }
    return false;
}

esp_err_t solar_os_agent_conversations_list(
    solar_os_agent_conversation_info_t *items,
    size_t capacity,
    size_t *count)
{
    if (count == NULL || (capacity > 0 && items == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = conversation_directory(directory, sizeof(directory));
    if (err != ESP_OK) {
        return err;
    }
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count < capacity) {
        char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
        if (!conversation_filename_id(entry->d_name, id, sizeof(id))) {
            continue;
        }
        bool duplicate = false;
        for (size_t i = 0; i < *count; i++) {
            if (strcmp(items[i].id, id) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        agent_conversation_header_t header;
        FILE *file = conversation_open_read(id, &header, NULL, 0);
        if (file == NULL) {
            continue;
        }
        fclose(file);
        conversation_header_to_info(&header, &items[(*count)++]);
    }
    closedir(dir);
    qsort(items, *count, sizeof(*items), conversation_info_compare);
    return ESP_OK;
}

esp_err_t solar_os_agent_conversation_get(
    const char *id,
    solar_os_agent_conversation_info_t *info)
{
    if (!conversation_id_valid(id) || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_conversation_header_t header;
    FILE *file = conversation_open_read(id, &header, NULL, 0);
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    fclose(file);
    conversation_header_to_info(&header, info);
    return ESP_OK;
}

esp_err_t solar_os_agent_conversation_visit(
    const char *id,
    solar_os_agent_message_visit_fn visitor,
    void *user_data)
{
    if (!conversation_id_valid(id) || visitor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_conversation_header_t header;
    FILE *file = conversation_open_read(id, &header, NULL, 0);
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    agent_conversation_record_meta_t records[AGENT_CONVERSATION_RECORD_MAX];
    size_t count = 0;
    esp_err_t err = conversation_read_records(file,
                                               &header,
                                               records,
                                               AGENT_CONVERSATION_RECORD_MAX,
                                               &count);
    char *text = NULL;
    if (err == ESP_OK && count > 0) {
        text = solar_os_memory_alloc(conversation_message_limit() + 1U,
                                     SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                     "agent.conversation.visit");
        if (text == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    for (size_t i = 0; err == ESP_OK && i < count; i++) {
        if (fseek(file,
                  records[i].offset + (long)sizeof(agent_conversation_record_t),
                  SEEK_SET) != 0 ||
            fread(text, records[i].header.text_len, 1, file) != 1) {
            err = ESP_FAIL;
            break;
        }
        text[records[i].header.text_len] = '\0';
        err = visitor((solar_os_agent_message_role_t)records[i].header.role,
                      text,
                      records[i].header.text_len,
                      user_data);
    }
    solar_os_memory_free(text);
    fclose(file);
    return err;
}

esp_err_t solar_os_agent_conversation_load_history(
    const char *id,
    bool include_messages,
    solar_os_agent_history_t *history)
{
    if (!conversation_id_valid(id) || history == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(history, 0, sizeof(*history));
    agent_conversation_header_t header;
    FILE *file = conversation_open_read(id, &header, NULL, 0);
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(history->provider_response_id,
            header.provider_response_id,
            sizeof(history->provider_response_id));
    if (!include_messages) {
        fclose(file);
        return ESP_OK;
    }

    agent_conversation_record_meta_t records[AGENT_CONVERSATION_RECORD_MAX];
    size_t record_count = 0;
    esp_err_t err = conversation_read_records(file,
                                               &header,
                                               records,
                                               AGENT_CONVERSATION_RECORD_MAX,
                                               &record_count);
    size_t start = record_count > SOLAR_OS_AGENT_HISTORY_MESSAGE_MAX ?
        record_count - SOLAR_OS_AGENT_HISTORY_MESSAGE_MAX : 0;
    size_t bounded_bytes = 0;
    size_t bounded_start = record_count;
    while (bounded_start > start) {
        const size_t candidate =
            records[bounded_start - 1U].header.text_len + 1U;
        if (bounded_bytes + candidate >
            AGENT_CONVERSATION_HISTORY_BYTES_MAX) {
            break;
        }
        bounded_bytes += candidate;
        bounded_start--;
    }
    start = bounded_start;
    while (start < record_count &&
           records[start].header.role != SOLAR_OS_AGENT_MESSAGE_USER) {
        start++;
    }
    size_t bytes = 0;
    for (size_t i = start; i < record_count; i++) {
        bytes += records[i].header.text_len + 1U;
    }
    if (err == ESP_OK && bytes > 0) {
        history->storage = solar_os_memory_alloc(
            bytes,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
            "agent.conversation.history");
        if (history->storage == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    size_t used = 0;
    for (size_t i = start;
         err == ESP_OK && i < record_count &&
         history->message_count < SOLAR_OS_AGENT_HISTORY_MESSAGE_MAX;
         i++) {
        const size_t len = records[i].header.text_len;
        if (fseek(file,
                  records[i].offset + (long)sizeof(agent_conversation_record_t),
                  SEEK_SET) != 0 ||
            fread(history->storage + used, len, 1, file) != 1) {
            err = ESP_FAIL;
            break;
        }
        history->storage[used + len] = '\0';
        history->messages[history->message_count++] =
            (solar_os_agent_history_message_t){
                .role = (solar_os_agent_message_role_t)records[i].header.role,
                .text = history->storage + used,
                .text_len = len,
            };
        used += len + 1U;
    }
    history->storage_size = used;
    fclose(file);
    if (err != ESP_OK) {
        solar_os_agent_conversation_free_history(history);
    }
    return err;
}

void solar_os_agent_conversation_free_history(
    solar_os_agent_history_t *history)
{
    if (history == NULL) {
        return;
    }
    solar_os_memory_free(history->storage);
    memset(history, 0, sizeof(*history));
}

static esp_err_t conversation_delete_unlocked(const char *id)
{
    if (!conversation_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool removed = false;
    static const char *const suffixes[] = {"", ".tmp", ".bak"};
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (conversation_path(id, suffixes[i], path, sizeof(path)) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (solar_os_storage_remove(path) == ESP_OK) {
            removed = true;
        } else if (errno != ENOENT) {
            return ESP_FAIL;
        }
    }
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_agent_conversation_delete(const char *id)
{
    esp_err_t err = conversation_take_mutex();
    if (err != ESP_OK) {
        return err;
    }
    err = conversation_delete_unlocked(id);
    conversation_give_mutex();
    return err;
}

static void conversation_enforce_retention(const char *keep_id)
{
    solar_os_agent_conversation_info_t items[16];
    size_t count = 0;
    if (solar_os_agent_conversations_list(items,
                                          sizeof(items) / sizeof(items[0]),
                                          &count) != ESP_OK) {
        return;
    }
    const size_t limit = conversation_count_limit();
    size_t remaining = count;
    for (size_t i = count; i > 0 && remaining > limit; i--) {
        if (strcmp(items[i - 1U].id, keep_id) != 0 &&
            conversation_delete_unlocked(items[i - 1U].id) == ESP_OK) {
            remaining--;
        }
    }
}

static esp_err_t conversation_commit_unlocked(
    const char *id,
    const char *provider,
    const char *model,
    const char *user_text,
    const char *assistant_text,
    const char *tool_summary,
    const char *provider_response_id,
    char *committed_id,
    size_t committed_id_len)
{
    if ((id != NULL && !conversation_id_valid(id)) ||
        provider == NULL || model == NULL || user_text == NULL ||
        assistant_text == NULL || committed_id == NULL ||
        committed_id_len < SOLAR_OS_AGENT_CONVERSATION_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = conversation_ensure_directory();
    if (err != ESP_OK) {
        return err;
    }

    char local_id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    if (id != NULL) {
        strlcpy(local_id, id, sizeof(local_id));
    } else {
        err = conversation_allocate_slot(local_id, sizeof(local_id));
        if (err != ESP_OK) {
            return err;
        }
    }
    agent_conversation_header_t header = {
        .magic = AGENT_CONVERSATION_MAGIC,
        .version = AGENT_CONVERSATION_VERSION,
        .header_size = sizeof(header),
    };
    FILE *source = NULL;
    bool source_from_backup = false;
    agent_conversation_record_meta_t records[AGENT_CONVERSATION_RECORD_MAX];
    size_t record_count = 0;
    if (id != NULL) {
        char opened_path[SOLAR_OS_STORAGE_PATH_MAX];
        source = conversation_open_read(local_id,
                                        &header,
                                        opened_path,
                                        sizeof(opened_path));
        if (source == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
        const size_t opened_len = strlen(opened_path);
        static const char backup_suffix[] = ".bak";
        source_from_backup =
            opened_len >= sizeof(backup_suffix) - 1U &&
            strcmp(opened_path + opened_len -
                       (sizeof(backup_suffix) - 1U),
                   backup_suffix) == 0;
        err = conversation_read_records(source,
                                        &header,
                                        records,
                                        AGENT_CONVERSATION_RECORD_MAX,
                                        &record_count);
        if (err != ESP_OK) {
            fclose(source);
            return err;
        }
    } else {
        strlcpy(header.id, local_id, sizeof(header.id));
        strlcpy(header.provider, provider, sizeof(header.provider));
        strlcpy(header.model, model, sizeof(header.model));
        conversation_make_title(user_text, header.title, sizeof(header.title));
        header.created_at = (uint64_t)time(NULL);
    }

    const uint64_t now = (uint64_t)time(NULL);
    header.updated_at = now;
    strlcpy(header.provider, provider, sizeof(header.provider));
    strlcpy(header.model, model, sizeof(header.model));
    strlcpy(header.provider_response_id,
            provider_response_id != NULL ? provider_response_id : "",
            sizeof(header.provider_response_id));

    const size_t user_len = conversation_bounded_length(user_text);
    const size_t assistant_len = conversation_bounded_length(assistant_text);
    const size_t tool_len = conversation_bounded_length(tool_summary);
    if (user_len == 0 || assistant_len == 0) {
        if (source != NULL) {
            fclose(source);
        }
        return ESP_ERR_INVALID_ARG;
    }
    const size_t new_record_count = tool_len > 0 ? 3U : 2U;
    const size_t new_bytes =
        new_record_count * sizeof(agent_conversation_record_t) +
        user_len + assistant_len + tool_len;
    const size_t limit = conversation_file_limit();
    size_t start = record_count;
    size_t selected_bytes = 0;
    while (start > 0) {
        if (record_count - (start - 1U) + new_record_count >
            AGENT_CONVERSATION_RECORD_MAX) {
            break;
        }
        const uint32_t candidate = records[start - 1U].total_size;
        if (sizeof(header) + selected_bytes + candidate + new_bytes > limit) {
            break;
        }
        selected_bytes += candidate;
        start--;
    }
    while (start < record_count &&
           records[start].header.role != SOLAR_OS_AGENT_MESSAGE_USER) {
        selected_bytes -= records[start].total_size;
        start++;
    }

    size_t selected_turns = 0;
    for (size_t i = start; i < record_count; i++) {
        if (records[i].header.role == SOLAR_OS_AGENT_MESSAGE_USER) {
            selected_turns++;
        }
    }
    header.turn_count = (uint16_t)(selected_turns + 1U);
    header.record_count =
        (uint16_t)(record_count - start + new_record_count);
    header.stored_bytes =
        (uint32_t)(sizeof(header) + selected_bytes + new_bytes);
    header.crc32 = conversation_header_crc(&header);

    char final_path[SOLAR_OS_STORAGE_PATH_MAX];
    char temp_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (conversation_path(local_id, "", final_path, sizeof(final_path)) != ESP_OK ||
        conversation_path(local_id, ".tmp", temp_path, sizeof(temp_path)) != ESP_OK ||
        conversation_path(local_id, ".bak", backup_path, sizeof(backup_path)) != ESP_OK) {
        if (source != NULL) {
            fclose(source);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *destination = fopen(temp_path, "wb");
    if (destination == NULL) {
        if (source != NULL) {
            fclose(source);
        }
        return ESP_FAIL;
    }
    err = fwrite(&header, sizeof(header), 1, destination) == 1 ?
        ESP_OK : ESP_FAIL;
    for (size_t i = start; err == ESP_OK && i < record_count; i++) {
        err = conversation_copy_record(source, destination, &records[i]);
    }
    if (err == ESP_OK) {
        err = conversation_write_record(destination,
                                        SOLAR_OS_AGENT_MESSAGE_USER,
                                        user_text,
                                        user_len,
                                        now);
    }
    if (err == ESP_OK && tool_len > 0) {
        err = conversation_write_record(destination,
                                        SOLAR_OS_AGENT_MESSAGE_TOOL,
                                        tool_summary,
                                        tool_len,
                                        now);
    }
    if (err == ESP_OK) {
        err = conversation_write_record(destination,
                                        SOLAR_OS_AGENT_MESSAGE_ASSISTANT,
                                        assistant_text,
                                        assistant_len,
                                        now);
    }
    if (err == ESP_OK) {
        err = conversation_sync(destination);
    }
    if (fclose(destination) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (source != NULL) {
        fclose(source);
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(temp_path);
        return err;
    }

    bool had_original = false;
    if (source_from_backup) {
        (void)solar_os_storage_remove(final_path);
        had_original = true;
    } else {
        (void)solar_os_storage_remove(backup_path);
        if (solar_os_storage_rename(final_path, backup_path) == ESP_OK) {
            had_original = true;
        } else if (errno != ENOENT) {
            (void)solar_os_storage_remove(temp_path);
            return ESP_FAIL;
        }
    }
    if (solar_os_storage_rename(temp_path, final_path) != ESP_OK) {
        if (had_original) {
            (void)solar_os_storage_rename(backup_path, final_path);
        }
        (void)solar_os_storage_remove(temp_path);
        return ESP_FAIL;
    }
    if (had_original) {
        (void)solar_os_storage_remove(backup_path);
    }
    strlcpy(committed_id, local_id, committed_id_len);
    conversation_enforce_retention(local_id);
    return ESP_OK;
}

esp_err_t solar_os_agent_conversation_commit(
    const char *id,
    const char *provider,
    const char *model,
    const char *user_text,
    const char *assistant_text,
    const char *tool_summary,
    const char *provider_response_id,
    char *committed_id,
    size_t committed_id_len)
{
    esp_err_t err = conversation_take_mutex();
    if (err != ESP_OK) {
        return err;
    }
    err = conversation_commit_unlocked(id,
                                       provider,
                                       model,
                                       user_text,
                                       assistant_text,
                                       tool_summary,
                                       provider_response_id,
                                       committed_id,
                                       committed_id_len);
    conversation_give_mutex();
    return err;
}
