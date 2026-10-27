#include "solar_os_credentials.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_saved_config.h"
#include "solar_os_memory.h"

#define CREDENTIALS_NVS_NAMESPACE "credentials"
#define CREDENTIALS_NVS_BLOB_KEY "records"
#define CREDENTIALS_STORE_MAGIC 0x44455243UL
#define CREDENTIALS_STORE_VERSION 2U
#define CREDENTIALS_STORE_LEGACY_VERSION 1U

typedef struct {
    bool active;
    solar_os_credential_info_t info;
    size_t secret_len;
    uint8_t secret[SOLAR_OS_CREDENTIAL_SECRET_MAX];
} credentials_slot_t;

typedef struct __attribute__((packed)) {
    uint8_t active;
    uint32_t id;
    uint8_t provider;
    uint8_t kind;
    uint16_t secret_len;
    char label[SOLAR_OS_CREDENTIAL_LABEL_MAX + 1U];
    uint8_t secret[SOLAR_OS_CREDENTIAL_SECRET_MAX];
} credentials_disk_record_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t generation;
    uint32_t next_id;
    uint16_t count;
    uint16_t reserved;
    credentials_disk_record_t records[SOLAR_OS_CREDENTIAL_CAPACITY];
    uint32_t crc32;
} credentials_store_blob_t;

typedef struct {
    bool initialized;
    bool loaded;
    bool records_in_psram;
    bool persistent;
    uint32_t generation;
    uint32_t next_id;
    size_t count;
    esp_err_t storage_error;
    credentials_slot_t *records;
    credentials_store_blob_t *scratch;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
} solar_os_credentials_store_t;

static solar_os_credentials_store_t credentials_store;

void solar_os_credentials_wipe(void *buffer, size_t length)
{
    volatile uint8_t *bytes = buffer;
    while (bytes != NULL && length > 0U) {
        *bytes++ = 0U;
        length--;
    }
}

static uint32_t credentials_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffUL;
    for (size_t index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8U; bit++) {
            crc = (crc & 1U) != 0U ?
                (crc >> 1U) ^ 0xedb88320UL : crc >> 1U;
        }
    }
    return ~crc;
}

static void credentials_lock(void)
{
    if (credentials_store.lock != NULL) {
        (void)xSemaphoreTake(credentials_store.lock, portMAX_DELAY);
    }
}

static void credentials_unlock(void)
{
    if (credentials_store.lock != NULL) {
        xSemaphoreGive(credentials_store.lock);
    }
}

static uint32_t credentials_next_generation(uint32_t generation)
{
    generation++;
    return generation != 0U ? generation : 1U;
}

static bool credentials_provider_valid(
    solar_os_messaging_provider_id_t provider)
{
    return provider != 0 && provider <= SOLAR_OS_MESSAGING_INSTANCE_ID_MAX;
}

static bool credentials_kind_valid(solar_os_credential_kind_t kind)
{
    return kind >= SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY &&
        kind <= SOLAR_OS_CREDENTIAL_TOKEN;
}

static int credentials_find_id_locked(solar_os_credential_id_t id)
{
    if (id == SOLAR_OS_CREDENTIAL_ID_NONE) {
        return -1;
    }
    for (size_t index = 0; index < SOLAR_OS_CREDENTIAL_CAPACITY; index++) {
        if (credentials_store.records[index].active &&
            credentials_store.records[index].info.id == id) {
            return (int)index;
        }
    }
    return -1;
}

static int credentials_find_record_locked(
    solar_os_messaging_provider_id_t provider,
    solar_os_credential_kind_t kind,
    const char *label)
{
    for (size_t index = 0; index < SOLAR_OS_CREDENTIAL_CAPACITY; index++) {
        const credentials_slot_t *slot = &credentials_store.records[index];
        if (slot->active &&
            slot->info.provider == provider &&
            slot->info.kind == kind &&
            strcmp(slot->info.label, label) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int credentials_free_slot_locked(void)
{
    for (size_t index = 0; index < SOLAR_OS_CREDENTIAL_CAPACITY; index++) {
        if (!credentials_store.records[index].active) {
            return (int)index;
        }
    }
    return -1;
}

static solar_os_credential_id_t credentials_allocate_id_locked(void)
{
    for (size_t attempts = 0; attempts <= SOLAR_OS_CREDENTIAL_CAPACITY;
         attempts++) {
        const solar_os_credential_id_t id = credentials_store.next_id;
        credentials_store.next_id =
            credentials_store.next_id == UINT32_MAX ?
            1U : credentials_store.next_id + 1U;
        if (id != SOLAR_OS_CREDENTIAL_ID_NONE &&
            credentials_find_id_locked(id) < 0) {
            return id;
        }
    }
    return SOLAR_OS_CREDENTIAL_ID_NONE;
}

static size_t credentials_blob_size(size_t count)
{
    return offsetof(credentials_store_blob_t, records) +
        count * sizeof(credentials_disk_record_t) +
        sizeof(uint32_t);
}

static size_t credentials_runtime_to_blob_locked(
    credentials_store_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = CREDENTIALS_STORE_MAGIC;
    blob->version = CREDENTIALS_STORE_VERSION;
    blob->record_size = sizeof(credentials_disk_record_t);
    blob->generation = credentials_store.generation;
    blob->next_id = credentials_store.next_id;
    blob->count = (uint16_t)credentials_store.count;
    size_t written = 0U;
    for (size_t index = 0; index < SOLAR_OS_CREDENTIAL_CAPACITY; index++) {
        const credentials_slot_t *source = &credentials_store.records[index];
        if (!source->active) {
            continue;
        }
        credentials_disk_record_t *target = &blob->records[written++];
        target->active = 1U;
        target->id = source->info.id;
        target->provider = (uint8_t)source->info.provider;
        target->kind = (uint8_t)source->info.kind;
        target->secret_len = (uint16_t)source->secret_len;
        strlcpy(target->label, source->info.label, sizeof(target->label));
        memcpy(target->secret, source->secret, source->secret_len);
    }
    const size_t length = credentials_blob_size(written);
    const uint32_t crc = credentials_crc32(blob, length - sizeof(crc));
    memcpy((uint8_t *)blob + length - sizeof(crc), &crc, sizeof(crc));
    return length;
}

static bool credentials_blob_valid(const credentials_store_blob_t *blob,
                                   size_t length)
{
    const bool legacy =
        blob->version == CREDENTIALS_STORE_LEGACY_VERSION;
    const size_t expected =
        legacy ? sizeof(*blob) : credentials_blob_size(blob->count);
    uint32_t stored_crc = 0U;
    if (length >= sizeof(stored_crc)) {
        memcpy(&stored_crc,
               (const uint8_t *)blob + length - sizeof(stored_crc),
               sizeof(stored_crc));
    }
    if (blob->magic != CREDENTIALS_STORE_MAGIC ||
        (blob->version != CREDENTIALS_STORE_VERSION && !legacy) ||
        blob->record_size != sizeof(credentials_disk_record_t) ||
        blob->generation == 0U ||
        blob->next_id == 0U ||
        blob->count > SOLAR_OS_CREDENTIAL_CAPACITY ||
        length != expected ||
        stored_crc != credentials_crc32(blob, length - sizeof(stored_crc))) {
        return false;
    }

    size_t count = 0U;
    const size_t records =
        legacy ? SOLAR_OS_CREDENTIAL_CAPACITY : blob->count;
    for (size_t index = 0; index < records; index++) {
        const credentials_disk_record_t *record = &blob->records[index];
        if (record->active == 0U) {
            continue;
        }
        if (record->active != 1U ||
            record->id == SOLAR_OS_CREDENTIAL_ID_NONE ||
            !credentials_provider_valid(
                (solar_os_messaging_provider_id_t)record->provider) ||
            !credentials_kind_valid(
                (solar_os_credential_kind_t)record->kind) ||
            record->secret_len > SOLAR_OS_CREDENTIAL_SECRET_MAX ||
            memchr(record->label, '\0', sizeof(record->label)) == NULL) {
            return false;
        }
        count++;
    }
    return count == blob->count;
}

static void credentials_blob_to_runtime(
    const credentials_store_blob_t *blob)
{
    memset(credentials_store.records,
           0,
           SOLAR_OS_CREDENTIAL_CAPACITY * sizeof(*credentials_store.records));
    const size_t records =
        blob->version == CREDENTIALS_STORE_LEGACY_VERSION ?
            SOLAR_OS_CREDENTIAL_CAPACITY : blob->count;
    size_t target_index = 0U;
    for (size_t index = 0; index < records; index++) {
        const credentials_disk_record_t *source = &blob->records[index];
        if (source->active == 0U) {
            continue;
        }
        credentials_slot_t *target =
            &credentials_store.records[target_index++];
        target->active = true;
        target->info.id = source->id;
        target->info.provider =
            (solar_os_messaging_provider_id_t)source->provider;
        target->info.kind = (solar_os_credential_kind_t)source->kind;
        strlcpy(target->info.label,
                source->label,
                sizeof(target->info.label));
        target->secret_len = source->secret_len;
        memcpy(target->secret, source->secret, source->secret_len);
    }
    credentials_store.generation = blob->generation;
    credentials_store.next_id = blob->next_id;
    credentials_store.count = blob->count;
}

static solar_os_saved_config_t credentials_file;
static bool credentials_file_valid(const void *data, size_t length)
{ return credentials_blob_valid(data, length); }

static esp_err_t credentials_load(void)
{
    size_t length = sizeof(*credentials_store.scratch);
    esp_err_t error = solar_os_saved_config_load(&credentials_file, "credentials",
        CREDENTIALS_NVS_NAMESPACE, CREDENTIALS_NVS_BLOB_KEY, credentials_store.scratch,
        sizeof(*credentials_store.scratch), &length, credentials_file_valid);
    if (error == ESP_OK) credentials_blob_to_runtime(credentials_store.scratch);
    solar_os_credentials_wipe(credentials_store.scratch, sizeof(*credentials_store.scratch));
    return error;
}

static esp_err_t credentials_write_blob(const credentials_store_blob_t *blob, size_t length)
{
    esp_err_t error = solar_os_saved_config_save(&credentials_file, "credentials", blob, length);
    if (error == ESP_OK) solar_os_saved_config_erase_legacy(CREDENTIALS_NVS_NAMESPACE, CREDENTIALS_NVS_BLOB_KEY);
    return error;
}

static esp_err_t credentials_persist_current(void)
{
    if (credentials_store.io_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(credentials_store.io_lock, portMAX_DELAY);
    esp_err_t error = ESP_FAIL;

    for (unsigned attempt = 0; attempt < 3U; attempt++) {
        credentials_lock();
        const uint32_t snapshot_generation = credentials_store.generation;
        const size_t length =
            credentials_runtime_to_blob_locked(credentials_store.scratch);
        credentials_unlock();

        error = credentials_write_blob(credentials_store.scratch, length);
        solar_os_credentials_wipe(credentials_store.scratch,
                                  sizeof(*credentials_store.scratch));

        credentials_lock();
        credentials_store.persistent = error == ESP_OK;
        credentials_store.storage_error = error;
        const bool current =
            snapshot_generation == credentials_store.generation;
        credentials_unlock();
        if (error != ESP_OK || current) {
            break;
        }
    }

    xSemaphoreGive(credentials_store.io_lock);
    return error;
}

esp_err_t solar_os_credentials_init(void)
{
    if (credentials_store.initialized) {
        return ESP_OK;
    }
    credentials_store.lock = xSemaphoreCreateMutex();
    credentials_store.io_lock = xSemaphoreCreateMutex();
    credentials_store.records =
        solar_os_memory_calloc(SOLAR_OS_CREDENTIAL_CAPACITY,
                               sizeof(*credentials_store.records),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "credentials.records");
    credentials_store.scratch =
        solar_os_memory_calloc(1U,
                               sizeof(*credentials_store.scratch),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "credentials.persist");
    if (credentials_store.lock == NULL ||
        credentials_store.io_lock == NULL ||
        credentials_store.records == NULL ||
        credentials_store.scratch == NULL) {
        return ESP_ERR_NO_MEM;
    }
    credentials_store.records_in_psram =
        solar_os_memory_is_external(credentials_store.records) &&
        solar_os_memory_is_external(credentials_store.scratch);
    credentials_store.generation = 1U;
    credentials_store.next_id = 1U;
    credentials_store.storage_error = ESP_ERR_INVALID_STATE;

    esp_err_t error = credentials_load();
    credentials_store.loaded = error == ESP_OK || error == ESP_ERR_NOT_FOUND ||
        error == ESP_ERR_NVS_NOT_FOUND || error == ESP_ERR_NVS_NOT_INITIALIZED;
    if (error == ESP_OK) {
        credentials_store.persistent = true;
        credentials_store.storage_error = ESP_OK;
    } else {
        credentials_store.persistent = false;
        credentials_store.storage_error = error;
    }
    credentials_store.initialized = true;
    if (error == ESP_ERR_NVS_NOT_FOUND || error == ESP_ERR_NOT_FOUND ||
        error == ESP_ERR_NVS_NOT_INITIALIZED) {
        (void)credentials_persist_current();
    }
    return ESP_OK;
}

esp_err_t solar_os_credentials_get_status(solar_os_credentials_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_credentials_init();
    if (error != ESP_OK) {
        return error;
    }
    credentials_lock();
    *status = (solar_os_credentials_status_t){
        .initialized = credentials_store.initialized,
        .records_in_psram = credentials_store.records_in_psram,
        .persistent = credentials_store.persistent,
        .capacity = SOLAR_OS_CREDENTIAL_CAPACITY,
        .count = credentials_store.count,
        .generation = credentials_store.generation,
        .storage_error = credentials_store.storage_error,
    };
    credentials_unlock();
    return ESP_OK;
}

size_t solar_os_credentials_snapshot(solar_os_credential_info_t *records,
                                     size_t max_records)
{
    if (solar_os_credentials_init() != ESP_OK) {
        return 0U;
    }
    size_t copied = 0U;
    credentials_lock();
    for (size_t index = 0;
         index < SOLAR_OS_CREDENTIAL_CAPACITY && copied < max_records;
         index++) {
        if (credentials_store.records[index].active) {
            if (records != NULL) {
                records[copied] = credentials_store.records[index].info;
            }
            copied++;
        }
    }
    credentials_unlock();
    return copied;
}

esp_err_t solar_os_credentials_find(solar_os_messaging_provider_id_t provider,
                                    solar_os_credential_kind_t kind,
                                    const char *label,
                                    solar_os_credential_info_t *record)
{
    if (!credentials_provider_valid(provider) ||
        !credentials_kind_valid(kind) ||
        label == NULL ||
        record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_credentials_init();
    if (error != ESP_OK) {
        return error;
    }
    credentials_lock();
    const int index = credentials_find_record_locked(provider, kind, label);
    if (index >= 0) {
        *record = credentials_store.records[index].info;
    }
    credentials_unlock();
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_credentials_put(solar_os_messaging_provider_id_t provider,
                                   solar_os_credential_kind_t kind,
                                   const char *label,
                                   const void *secret,
                                   size_t secret_len,
                                   bool replace,
                                   solar_os_credential_id_t *record_id)
{
    if (!credentials_provider_valid(provider) ||
        !credentials_kind_valid(kind) ||
        label == NULL ||
        label[0] == '\0' ||
        strlen(label) > SOLAR_OS_CREDENTIAL_LABEL_MAX ||
        secret == NULL ||
        secret_len == 0U ||
        secret_len > SOLAR_OS_CREDENTIAL_SECRET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_credentials_init();
    if (error != ESP_OK) {
        return error;
    }

    solar_os_credential_id_t id = SOLAR_OS_CREDENTIAL_ID_NONE;
    if (!credentials_store.loaded) return credentials_store.storage_error;
    credentials_lock();
    int index = credentials_find_record_locked(provider, kind, label);
    if (index >= 0 && !replace) {
        credentials_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0) {
        index = credentials_free_slot_locked();
        if (index < 0) {
            credentials_unlock();
            return ESP_ERR_NO_MEM;
        }
        id = credentials_allocate_id_locked();
        if (id == SOLAR_OS_CREDENTIAL_ID_NONE) {
            credentials_unlock();
            return ESP_ERR_NO_MEM;
        }
        credentials_store.records[index].active = true;
        credentials_store.records[index].info.id = id;
        credentials_store.count++;
    } else {
        id = credentials_store.records[index].info.id;
        solar_os_credentials_wipe(credentials_store.records[index].secret,
                                  sizeof(credentials_store.records[index].secret));
    }

    credentials_slot_t *slot = &credentials_store.records[index];
    slot->info.provider = provider;
    slot->info.kind = kind;
    strlcpy(slot->info.label, label, sizeof(slot->info.label));
    slot->secret_len = secret_len;
    memcpy(slot->secret, secret, secret_len);
    credentials_store.generation =
        credentials_next_generation(credentials_store.generation);
    credentials_unlock();

    error = credentials_persist_current();
    if (record_id != NULL) {
        *record_id = id;
    }
    return error;
}

esp_err_t solar_os_credentials_read_secret(solar_os_credential_id_t record_id,
                                           void *secret,
                                           size_t secret_capacity,
                                           size_t *secret_len)
{
    if (record_id == SOLAR_OS_CREDENTIAL_ID_NONE ||
        secret == NULL ||
        secret_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *secret_len = 0U;
    esp_err_t error = solar_os_credentials_init();
    if (error != ESP_OK) {
        return error;
    }

    credentials_lock();
    const int index = credentials_find_id_locked(record_id);
    if (index < 0) {
        credentials_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const credentials_slot_t *slot = &credentials_store.records[index];
    *secret_len = slot->secret_len;
    if (secret_capacity < slot->secret_len) {
        credentials_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(secret, slot->secret, slot->secret_len);
    credentials_unlock();
    return ESP_OK;
}

esp_err_t solar_os_credentials_remove(solar_os_credential_id_t record_id)
{
    if (record_id == SOLAR_OS_CREDENTIAL_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_credentials_init();
    if (error != ESP_OK) {
        return error;
    }
    if (!credentials_store.loaded) return credentials_store.storage_error;
    credentials_lock();
    const int index = credentials_find_id_locked(record_id);
    if (index >= 0) {
        solar_os_credentials_wipe(&credentials_store.records[index],
                                  sizeof(credentials_store.records[index]));
        if (credentials_store.count > 0U) {
            credentials_store.count--;
        }
        credentials_store.generation =
            credentials_next_generation(credentials_store.generation);
    }
    credentials_unlock();
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return credentials_persist_current();
}

esp_err_t solar_os_credentials_remove_provider(solar_os_messaging_provider_id_t provider)
{
    if (!credentials_provider_valid(provider)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_credentials_init(); if (error != ESP_OK) return error;
    if (!credentials_store.loaded) return credentials_store.storage_error;
    credentials_lock();
    bool changed = false;
    for (size_t i = 0; i < SOLAR_OS_CREDENTIAL_CAPACITY; i++) {
        credentials_slot_t *slot = &credentials_store.records[i];
        if (!slot->active || slot->info.provider != provider) continue;
        solar_os_credentials_wipe(slot, sizeof(*slot));
        credentials_store.count--;
        changed = true;
    }
    if (changed) credentials_store.generation = credentials_next_generation(credentials_store.generation);
    bool pending = !credentials_store.persistent;
    credentials_unlock();
    return changed || pending ? credentials_persist_current() : solar_os_saved_config_cleanup(&credentials_file);
}

const char *solar_os_credential_kind_name(solar_os_credential_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY:
        return "asymmetric-identity";
    case SOLAR_OS_CREDENTIAL_SHARED_KEY:
        return "shared-key";
    case SOLAR_OS_CREDENTIAL_TOKEN:
        return "token";
    default:
        return "unknown";
    }
}
