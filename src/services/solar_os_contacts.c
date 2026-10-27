#include "solar_os_contacts.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"

#define CONTACTS_STORE_MAGIC 0x544e4f43UL
#define CONTACTS_STORE_VERSION 2U
#define CONTACTS_LEGACY_CONTACT_CAPACITY 64U
#define CONTACTS_LEGACY_ENDPOINT_CAPACITY 80U
#define CONTACTS_STORE_HEADER_COPIES 2U
#define CONTACTS_STORE_DATA_COPIES 2U
#define CONTACTS_STORE_IO_CHUNK 4096U

typedef struct {
    bool active;
    solar_os_contact_t record;
} contacts_contact_slot_t;

typedef struct {
    bool active;
    solar_os_endpoint_t record;
} contacts_endpoint_slot_t;

typedef struct __attribute__((packed)) {
    uint8_t active;
    uint32_t id;
    uint32_t flags;
    uint64_t created_ms;
    uint64_t updated_ms;
    char display_name[SOLAR_OS_CONTACT_NAME_MAX + 1U];
} contacts_disk_contact_t;

typedef struct __attribute__((packed)) {
    uint8_t active;
    uint32_t id;
    uint32_t contact_id;
    uint8_t provider;
    uint8_t trust;
    uint32_t capabilities;
    uint64_t last_seen_ms;
    uint8_t address_len;
    uint8_t address[SOLAR_OS_MESSAGING_ADDRESS_MAX];
    uint8_t provider_metadata_len;
    uint8_t provider_metadata[SOLAR_OS_CONTACT_PROVIDER_METADATA_MAX];
} contacts_disk_endpoint_t;

typedef struct __attribute__((packed)) {
    uint32_t generation;
    uint32_t next_contact_id;
    uint32_t next_endpoint_id;
    uint32_t evicted;
    uint16_t contact_count;
    uint16_t endpoint_count;
    contacts_disk_contact_t contacts[SOLAR_OS_CONTACT_CAPACITY];
    contacts_disk_endpoint_t endpoints[SOLAR_OS_ENDPOINT_CAPACITY];
} contacts_store_data_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint32_t data_size;
    uint32_t generation;
    uint8_t data_slot;
    uint8_t reserved[3];
    uint32_t data_crc32;
    uint32_t header_crc32;
} contacts_store_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t data_size;
    uint32_t generation;
    uint8_t data_slot;
    uint8_t reserved[3];
    uint32_t data_crc32;
    uint32_t header_crc32;
} contacts_legacy_header_t;

#define CONTACTS_LEGACY_DATA_SIZE \
    (offsetof(contacts_store_data_t, contacts) + \
     CONTACTS_LEGACY_CONTACT_CAPACITY * sizeof(contacts_disk_contact_t) + \
     CONTACTS_LEGACY_ENDPOINT_CAPACITY * sizeof(contacts_disk_endpoint_t))

#define CONTACTS_STORE_DATA_OFFSET \
    (CONTACTS_STORE_HEADER_COPIES * sizeof(contacts_store_header_t))
#define CONTACTS_STORE_BYTES \
    (CONTACTS_STORE_DATA_OFFSET + \
     CONTACTS_STORE_DATA_COPIES * sizeof(contacts_store_data_t))

_Static_assert(CONTACTS_STORE_BYTES < SOLAR_OS_CONTACT_STORE_LIMIT_BYTES,
               "contacts store must remain below 192 KiB");

typedef struct {
    bool initialized;
    bool records_in_psram;
    bool persistent;
    bool legacy_format;
    uint32_t generation;
    uint32_t disk_generation;
    uint8_t disk_slot;
    uint32_t next_contact_id;
    uint32_t next_endpoint_id;
    uint32_t evicted;
    size_t contact_count;
    size_t endpoint_count;
    esp_err_t storage_error;
    char store_path[SOLAR_OS_STORAGE_PATH_MAX];
    char staging_path[SOLAR_OS_STORAGE_PATH_MAX + 8U];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX + 8U];
    contacts_contact_slot_t *contacts;
    contacts_endpoint_slot_t *endpoints;
    contacts_store_data_t *scratch;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
} solar_os_contacts_store_t;

static solar_os_contacts_store_t contacts_store;

static uint32_t contacts_crc32(const void *data, size_t length)
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

static void contacts_lock(void)
{
    if (contacts_store.lock != NULL) {
        (void)xSemaphoreTake(contacts_store.lock, portMAX_DELAY);
    }
}

static void contacts_unlock(void)
{
    if (contacts_store.lock != NULL) {
        xSemaphoreGive(contacts_store.lock);
    }
}

static uint64_t contacts_now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static uint32_t contacts_next_generation(uint32_t generation)
{
    generation++;
    return generation != 0U ? generation : 1U;
}

static void contacts_changed_locked(void)
{
    contacts_store.generation =
        contacts_next_generation(contacts_store.generation);
}

static bool contacts_provider_valid(solar_os_messaging_provider_id_t provider)
{
    return provider != 0 && provider <= SOLAR_OS_MESSAGING_INSTANCE_ID_MAX;
}

static bool contacts_trust_valid(solar_os_contact_trust_t trust)
{
    return trust >= SOLAR_OS_CONTACT_TRUST_DISCOVERED &&
        trust <= SOLAR_OS_CONTACT_TRUST_BLOCKED;
}

static int contacts_find_contact_locked(solar_os_contact_id_t id)
{
    if (id == SOLAR_OS_CONTACT_ID_NONE) {
        return -1;
    }
    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        if (contacts_store.contacts[index].active &&
            contacts_store.contacts[index].record.id == id) {
            return (int)index;
        }
    }
    return -1;
}

static int contacts_find_endpoint_id_locked(solar_os_endpoint_id_t id)
{
    if (id == SOLAR_OS_ENDPOINT_ID_NONE) {
        return -1;
    }
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        if (contacts_store.endpoints[index].active &&
            contacts_store.endpoints[index].record.id == id) {
            return (int)index;
        }
    }
    return -1;
}

static int contacts_find_endpoint_address_locked(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address,
    size_t address_len)
{
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_endpoint_slot_t *slot = &contacts_store.endpoints[index];
        if (slot->active &&
            slot->record.provider == provider &&
            slot->record.address.length == address_len &&
            memcmp(slot->record.address.bytes, address, address_len) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int contacts_free_contact_slot_locked(void)
{
    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        if (!contacts_store.contacts[index].active) {
            return (int)index;
        }
    }
    return -1;
}

static int contacts_free_endpoint_slot_locked(void)
{
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        if (!contacts_store.endpoints[index].active) {
            return (int)index;
        }
    }
    return -1;
}

static bool contacts_id_in_use_locked(solar_os_contact_id_t id)
{
    return contacts_find_contact_locked(id) >= 0;
}

static bool contacts_endpoint_id_in_use_locked(solar_os_endpoint_id_t id)
{
    return contacts_find_endpoint_id_locked(id) >= 0;
}

static solar_os_contact_id_t contacts_allocate_id_locked(void)
{
    for (size_t attempts = 0; attempts <= SOLAR_OS_CONTACT_CAPACITY; attempts++) {
        const solar_os_contact_id_t id = contacts_store.next_contact_id;
        contacts_store.next_contact_id =
            contacts_store.next_contact_id == UINT32_MAX ?
            1U : contacts_store.next_contact_id + 1U;
        if (id != SOLAR_OS_CONTACT_ID_NONE && !contacts_id_in_use_locked(id)) {
            return id;
        }
    }
    return SOLAR_OS_CONTACT_ID_NONE;
}

static solar_os_endpoint_id_t contacts_allocate_endpoint_id_locked(void)
{
    for (size_t attempts = 0; attempts <= SOLAR_OS_ENDPOINT_CAPACITY; attempts++) {
        const solar_os_endpoint_id_t id = contacts_store.next_endpoint_id;
        contacts_store.next_endpoint_id =
            contacts_store.next_endpoint_id == UINT32_MAX ?
            1U : contacts_store.next_endpoint_id + 1U;
        if (id != SOLAR_OS_ENDPOINT_ID_NONE &&
            !contacts_endpoint_id_in_use_locked(id)) {
            return id;
        }
    }
    return SOLAR_OS_ENDPOINT_ID_NONE;
}

static bool contacts_contact_is_evictable_locked(
    solar_os_contact_id_t contact_id)
{
    const int contact_index = contacts_find_contact_locked(contact_id);
    if (contact_index < 0 ||
        (contacts_store.contacts[contact_index].record.flags &
         SOLAR_OS_CONTACT_FLAG_PINNED) != 0U) {
        return false;
    }
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_endpoint_slot_t *endpoint =
            &contacts_store.endpoints[index];
        if (endpoint->active &&
            endpoint->record.contact_id == contact_id &&
            endpoint->record.trust != SOLAR_OS_CONTACT_TRUST_DISCOVERED) {
            return false;
        }
    }
    return true;
}

static void contacts_remove_locked(solar_os_contact_id_t id)
{
    const int contact_index = contacts_find_contact_locked(id);
    if (contact_index < 0) {
        return;
    }
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        contacts_endpoint_slot_t *endpoint = &contacts_store.endpoints[index];
        if (endpoint->active && endpoint->record.contact_id == id) {
            memset(endpoint, 0, sizeof(*endpoint));
            if (contacts_store.endpoint_count > 0U) {
                contacts_store.endpoint_count--;
            }
        }
    }
    memset(&contacts_store.contacts[contact_index],
           0,
           sizeof(contacts_store.contacts[contact_index]));
    if (contacts_store.contact_count > 0U) {
        contacts_store.contact_count--;
    }
}

static bool contacts_evict_oldest_locked(void)
{
    int oldest = -1;
    uint64_t oldest_time = UINT64_MAX;
    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        const contacts_contact_slot_t *slot = &contacts_store.contacts[index];
        if (slot->active &&
            contacts_contact_is_evictable_locked(slot->record.id) &&
            (oldest < 0 || slot->record.updated_ms < oldest_time)) {
            oldest = (int)index;
            oldest_time = slot->record.updated_ms;
        }
    }
    if (oldest < 0) {
        return false;
    }
    contacts_remove_locked(contacts_store.contacts[oldest].record.id);
    contacts_store.evicted++;
    return true;
}

static void contacts_fill_contact_locked(
    const contacts_contact_slot_t *slot,
    solar_os_contact_t *contact)
{
    *contact = slot->record;
    contact->endpoint_count = 0U;
    contact->primary_provider = 0;
    contact->primary_trust = SOLAR_OS_CONTACT_TRUST_DISCOVERED;
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_endpoint_slot_t *endpoint =
            &contacts_store.endpoints[index];
        if (!endpoint->active ||
            endpoint->record.contact_id != contact->id) {
            continue;
        }
        if (contact->endpoint_count == 0U) {
            contact->primary_provider = endpoint->record.provider;
            contact->primary_trust = endpoint->record.trust;
        }
        contact->endpoint_count++;
    }
}

static void contacts_runtime_to_disk_locked(contacts_store_data_t *data)
{
    memset(data, 0, sizeof(*data));
    data->generation = contacts_store.generation;
    data->next_contact_id = contacts_store.next_contact_id;
    data->next_endpoint_id = contacts_store.next_endpoint_id;
    data->evicted = contacts_store.evicted;
    data->contact_count = (uint16_t)contacts_store.contact_count;
    data->endpoint_count = (uint16_t)contacts_store.endpoint_count;

    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        const contacts_contact_slot_t *source =
            &contacts_store.contacts[index];
        contacts_disk_contact_t *target = &data->contacts[index];
        if (!source->active) {
            continue;
        }
        target->active = 1U;
        target->id = source->record.id;
        target->flags = source->record.flags;
        target->created_ms = source->record.created_ms;
        target->updated_ms = source->record.updated_ms;
        strlcpy(target->display_name,
                source->record.display_name,
                sizeof(target->display_name));
    }

    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_endpoint_slot_t *source =
            &contacts_store.endpoints[index];
        contacts_disk_endpoint_t *target = &data->endpoints[index];
        if (!source->active) {
            continue;
        }
        target->active = 1U;
        target->id = source->record.id;
        target->contact_id = source->record.contact_id;
        target->provider = (uint8_t)source->record.provider;
        target->trust = (uint8_t)source->record.trust;
        target->capabilities = source->record.capabilities;
        target->last_seen_ms = source->record.last_seen_ms;
        target->address_len = source->record.address.length;
        memcpy(target->address,
               source->record.address.bytes,
               source->record.address.length);
        target->provider_metadata_len = source->record.provider_metadata_len;
        memcpy(target->provider_metadata,
               source->record.provider_metadata,
               source->record.provider_metadata_len);
    }
}

static bool contacts_disk_has_contact(const contacts_store_data_t *data,
                                      solar_os_contact_id_t contact_id)
{
    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        if (data->contacts[index].active != 0U &&
            data->contacts[index].id == contact_id) {
            return true;
        }
    }
    return false;
}

static bool contacts_store_data_valid(const contacts_store_data_t *data)
{
    if (data->generation == 0U ||
        data->next_contact_id == 0U ||
        data->next_endpoint_id == 0U ||
        data->contact_count > SOLAR_OS_CONTACT_CAPACITY ||
        data->endpoint_count > SOLAR_OS_ENDPOINT_CAPACITY) {
        return false;
    }

    size_t contacts = 0U;
    size_t endpoints = 0U;
    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        const contacts_disk_contact_t *contact = &data->contacts[index];
        if (contact->active == 0U) {
            continue;
        }
        if (contact->active != 1U || contact->id == 0U ||
            memchr(contact->display_name,
                   '\0',
                   sizeof(contact->display_name)) == NULL) {
            return false;
        }
        contacts++;
    }
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_disk_endpoint_t *endpoint = &data->endpoints[index];
        if (endpoint->active == 0U) {
            continue;
        }
        if (endpoint->active != 1U ||
            endpoint->id == 0U ||
            !contacts_disk_has_contact(data, endpoint->contact_id) ||
            !contacts_provider_valid(
                (solar_os_messaging_provider_id_t)endpoint->provider) ||
            !contacts_trust_valid((solar_os_contact_trust_t)endpoint->trust) ||
            endpoint->address_len == 0U ||
            endpoint->address_len > SOLAR_OS_MESSAGING_ADDRESS_MAX ||
            endpoint->provider_metadata_len >
                SOLAR_OS_CONTACT_PROVIDER_METADATA_MAX) {
            return false;
        }
        endpoints++;
    }
    return contacts == data->contact_count &&
        endpoints == data->endpoint_count;
}

static void contacts_disk_to_runtime(const contacts_store_data_t *data)
{
    memset(contacts_store.contacts,
           0,
           SOLAR_OS_CONTACT_CAPACITY * sizeof(*contacts_store.contacts));
    memset(contacts_store.endpoints,
           0,
           SOLAR_OS_ENDPOINT_CAPACITY * sizeof(*contacts_store.endpoints));

    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        const contacts_disk_contact_t *source = &data->contacts[index];
        contacts_contact_slot_t *target = &contacts_store.contacts[index];
        if (source->active == 0U) {
            continue;
        }
        target->active = true;
        target->record.id = source->id;
        target->record.flags = source->flags;
        target->record.created_ms = source->created_ms;
        target->record.updated_ms = source->updated_ms;
        strlcpy(target->record.display_name,
                source->display_name,
                sizeof(target->record.display_name));
    }
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_disk_endpoint_t *source = &data->endpoints[index];
        contacts_endpoint_slot_t *target = &contacts_store.endpoints[index];
        if (source->active == 0U) {
            continue;
        }
        target->active = true;
        target->record.id = source->id;
        target->record.contact_id = source->contact_id;
        target->record.provider =
            (solar_os_messaging_provider_id_t)source->provider;
        target->record.trust = (solar_os_contact_trust_t)source->trust;
        target->record.capabilities = source->capabilities;
        target->record.last_seen_ms = source->last_seen_ms;
        target->record.address.length = source->address_len;
        memcpy(target->record.address.bytes,
               source->address,
               source->address_len);
        target->record.provider_metadata_len =
            source->provider_metadata_len;
        memcpy(target->record.provider_metadata,
               source->provider_metadata,
               source->provider_metadata_len);
    }
    contacts_store.generation = data->generation;
    contacts_store.next_contact_id = data->next_contact_id;
    contacts_store.next_endpoint_id = data->next_endpoint_id;
    contacts_store.evicted = data->evicted;
    contacts_store.contact_count = data->contact_count;
    contacts_store.endpoint_count = data->endpoint_count;
}

static bool contacts_header_valid(const contacts_store_header_t *header)
{
    return header->magic == CONTACTS_STORE_MAGIC &&
        header->version == CONTACTS_STORE_VERSION &&
        header->data_size == sizeof(contacts_store_data_t) &&
        header->generation != 0U &&
        header->data_slot < CONTACTS_STORE_DATA_COPIES &&
        header->header_crc32 ==
            contacts_crc32(header,
                           offsetof(contacts_store_header_t, header_crc32));
}

static bool contacts_generation_newer(uint32_t first, uint32_t second)
{
    return (int32_t)(first - second) > 0;
}

static esp_err_t contacts_sync_file(FILE *file)
{
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

/* Keep PSRAM-backed snapshot transfers within the bounded I/O size used
 * by other storage clients. */
static bool contacts_read_data(FILE *file, void *data, size_t size)
{
    uint8_t *bytes = data;
    for (size_t offset = 0; offset < size;) {
        const size_t remaining = size - offset;
        const size_t chunk = remaining < CONTACTS_STORE_IO_CHUNK ?
            remaining : CONTACTS_STORE_IO_CHUNK;
        if (fread(bytes + offset, 1U, chunk, file) != chunk) return false;
        offset += chunk;
    }
    return true;
}

static bool contacts_write_data(FILE *file, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t offset = 0; offset < size;) {
        const size_t remaining = size - offset;
        const size_t chunk = remaining < CONTACTS_STORE_IO_CHUNK ?
            remaining : CONTACTS_STORE_IO_CHUNK;
        if (fwrite(bytes + offset, 1U, chunk, file) != chunk) {
            SOLAR_OS_LOGW("contacts", "store write failed at %u/%u bytes: errno=%d",
                          (unsigned)offset, (unsigned)size, errno);
            return false;
        }
        offset += chunk;
    }
    return true;
}

static esp_err_t contacts_prepare_store_path(void)
{
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t error = solar_os_storage_default_path(SOLAR_OS_CONTACT_STORE_DIR,
                                                     directory,
                                                     sizeof(directory));
    if (error != ESP_OK) {
        return error;
    }
    error = solar_os_storage_mkdir(directory);
    if (error != ESP_OK && errno != EEXIST) {
        return error;
    }
    error = solar_os_storage_default_path(
        SOLAR_OS_CONTACT_STORE_DIR "/" SOLAR_OS_CONTACT_STORE_FILE,
        contacts_store.store_path,
        sizeof(contacts_store.store_path));
    if (error == ESP_OK) {
        snprintf(contacts_store.staging_path, sizeof(contacts_store.staging_path),
                 "%s.tmp", contacts_store.store_path);
        snprintf(contacts_store.backup_path, sizeof(contacts_store.backup_path),
                 "%s.bak", contacts_store.store_path);
    }
    return error;
}

static esp_err_t contacts_restore(void)
{
    FILE *file = fopen(contacts_store.store_path, "rb");
    if (file == NULL && errno == ENOENT &&
        rename(contacts_store.backup_path, contacts_store.store_path) == 0) {
        file = fopen(contacts_store.store_path, "rb");
    }
    if (file == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    contacts_store_header_t headers[CONTACTS_STORE_HEADER_COPIES];
    memset(headers, 0, sizeof(headers));
    for (size_t index = 0; index < CONTACTS_STORE_HEADER_COPIES; index++) {
        if (fread(&headers[index], sizeof(headers[index]), 1U, file) != 1U) {
            clearerr(file);
            memset(&headers[index], 0, sizeof(headers[index]));
        }
    }

    int order[CONTACTS_STORE_HEADER_COPIES] = {-1, -1};
    for (size_t index = 0; index < CONTACTS_STORE_HEADER_COPIES; index++) {
        if (!contacts_header_valid(&headers[index])) {
            continue;
        }
        if (order[0] < 0 ||
            contacts_generation_newer(headers[index].generation,
                                      headers[order[0]].generation)) {
            order[1] = order[0];
            order[0] = (int)index;
        } else {
            order[1] = (int)index;
        }
    }

    esp_err_t error = ESP_ERR_INVALID_CRC;
    for (size_t candidate = 0;
         candidate < CONTACTS_STORE_HEADER_COPIES && order[candidate] >= 0;
         candidate++) {
        const contacts_store_header_t *header = &headers[order[candidate]];
        const long offset = (long)(CONTACTS_STORE_DATA_OFFSET +
            header->data_slot * sizeof(*contacts_store.scratch));
        if (fseek(file, offset, SEEK_SET) != 0 ||
            !contacts_read_data(file, contacts_store.scratch,
                                sizeof(*contacts_store.scratch))) {
            clearerr(file);
            continue;
        }
        if (header->data_crc32 !=
                contacts_crc32(contacts_store.scratch,
                               sizeof(*contacts_store.scratch)) ||
            !contacts_store_data_valid(contacts_store.scratch)) {
            continue;
        }
        contacts_disk_to_runtime(contacts_store.scratch);
        contacts_store.disk_generation = header->generation;
        contacts_store.disk_slot = header->data_slot;
        error = ESP_OK;
        break;
    }
    if (error != ESP_OK) {
        contacts_legacy_header_t legacy[CONTACTS_STORE_HEADER_COPIES];
        if (fseek(file, 0, SEEK_SET) == 0 &&
            fread(legacy, sizeof(legacy), 1U, file) == 1U) {
            int first = 0;
            if (contacts_generation_newer(legacy[1].generation,
                                          legacy[0].generation)) first = 1;
            for (int candidate = 0; candidate < 2; candidate++) {
                const contacts_legacy_header_t *header =
                    &legacy[candidate == 0 ? first : 1 - first];
                if (header->magic != CONTACTS_STORE_MAGIC ||
                    header->version != 1U ||
                    header->data_size != CONTACTS_LEGACY_DATA_SIZE ||
                    header->generation == 0U ||
                    header->data_slot >= CONTACTS_STORE_DATA_COPIES ||
                    header->header_crc32 != contacts_crc32(
                        header, offsetof(contacts_legacy_header_t, header_crc32))) {
                    continue;
                }
                const long offset = (long)(sizeof(legacy) +
                    header->data_slot * CONTACTS_LEGACY_DATA_SIZE);
                memset(contacts_store.scratch, 0, sizeof(*contacts_store.scratch));
                clearerr(file);
                if (fseek(file, offset, SEEK_SET) != 0 ||
                    !contacts_read_data(file, contacts_store.scratch,
                                        CONTACTS_LEGACY_DATA_SIZE) ||
                    header->data_crc32 != contacts_crc32(
                        contacts_store.scratch, CONTACTS_LEGACY_DATA_SIZE) ||
                    contacts_store.scratch->contact_count > CONTACTS_LEGACY_CONTACT_CAPACITY ||
                    contacts_store.scratch->endpoint_count > CONTACTS_LEGACY_ENDPOINT_CAPACITY) {
                    continue;
                }
                uint8_t *old_endpoints = (uint8_t *)contacts_store.scratch +
                    offsetof(contacts_store_data_t, contacts) +
                    CONTACTS_LEGACY_CONTACT_CAPACITY * sizeof(contacts_disk_contact_t);
                memmove(contacts_store.scratch->endpoints, old_endpoints,
                        CONTACTS_LEGACY_ENDPOINT_CAPACITY * sizeof(contacts_disk_endpoint_t));
                memset(contacts_store.scratch->contacts + CONTACTS_LEGACY_CONTACT_CAPACITY,
                       0, (SOLAR_OS_CONTACT_CAPACITY - CONTACTS_LEGACY_CONTACT_CAPACITY) *
                          sizeof(contacts_disk_contact_t));
                if (!contacts_store_data_valid(contacts_store.scratch)) continue;
                contacts_disk_to_runtime(contacts_store.scratch);
                contacts_store.disk_generation = header->generation;
                contacts_store.disk_slot = header->data_slot;
                contacts_store.legacy_format = true;
                error = ESP_OK;
                break;
            }
        }
    }
    fclose(file);
    return error;
}

static esp_err_t contacts_write_snapshot(const contacts_store_data_t *data,
                                         uint32_t disk_generation,
                                         uint8_t data_slot)
{
    const bool migrate = contacts_store.legacy_format;
    const char *path = migrate ? contacts_store.staging_path : contacts_store.store_path;
    FILE *file = fopen(path, migrate ? "w+b" : "r+b");
    if (file == NULL) {
        file = fopen(path, "w+b");
    }
    if (file == NULL) {
        return ESP_FAIL;
    }

    const long data_offset = (long)(CONTACTS_STORE_DATA_OFFSET +
        data_slot * sizeof(*data));
    esp_err_t error = ESP_OK;
    if (fseek(file, data_offset, SEEK_SET) != 0 ||
        !contacts_write_data(file, data, sizeof(*data)) ||
        contacts_sync_file(file) != ESP_OK) {
        error = ESP_FAIL;
    }

    if (error == ESP_OK) {
        contacts_store_header_t header = {
            .magic = CONTACTS_STORE_MAGIC,
            .version = CONTACTS_STORE_VERSION,
            .data_size = sizeof(*data),
            .generation = disk_generation,
            .data_slot = data_slot,
            .data_crc32 = contacts_crc32(data, sizeof(*data)),
        };
        header.header_crc32 =
            contacts_crc32(&header,
                           offsetof(contacts_store_header_t, header_crc32));
        const size_t header_slot =
            disk_generation % CONTACTS_STORE_HEADER_COPIES;
        if (fseek(file,
                  (long)(header_slot * sizeof(header)),
                  SEEK_SET) != 0 ||
            fwrite(&header, sizeof(header), 1U, file) != 1U ||
            contacts_sync_file(file) != ESP_OK) {
            error = ESP_FAIL;
        }
    }

    if (fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    if (migrate && error == ESP_OK) {
        error = solar_os_storage_replace_file(contacts_store.staging_path,
            contacts_store.store_path, contacts_store.backup_path);
        if (error == ESP_OK) contacts_store.legacy_format = false;
    }
    return error;
}

static esp_err_t contacts_persist_current(void)
{
    if (contacts_store.io_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(contacts_store.io_lock, portMAX_DELAY);

    esp_err_t error = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 3U; attempt++) {
        if (contacts_store.store_path[0] == '\0') {
            error = contacts_prepare_store_path();
            if (error != ESP_OK) {
                break;
            }
        }

        contacts_lock();
        const uint32_t snapshot_generation = contacts_store.generation;
        const uint32_t disk_generation =
            contacts_next_generation(contacts_store.disk_generation);
        const uint8_t data_slot =
            (uint8_t)((contacts_store.disk_slot + 1U) %
                      CONTACTS_STORE_DATA_COPIES);
        contacts_runtime_to_disk_locked(contacts_store.scratch);
        contacts_unlock();

        error = contacts_write_snapshot(contacts_store.scratch,
                                        disk_generation,
                                        data_slot);

        contacts_lock();
        if (error == ESP_OK) {
            contacts_store.disk_generation = disk_generation;
            contacts_store.disk_slot = data_slot;
            contacts_store.persistent = true;
            contacts_store.storage_error = ESP_OK;
        } else {
            contacts_store.persistent = false;
            contacts_store.storage_error = error;
        }
        const bool current =
            snapshot_generation == contacts_store.generation;
        contacts_unlock();
        if (error != ESP_OK || current) {
            break;
        }
    }

    xSemaphoreGive(contacts_store.io_lock);
    return error;
}

esp_err_t solar_os_contacts_init(void)
{
    if (contacts_store.initialized) {
        return ESP_OK;
    }

    contacts_store.lock = xSemaphoreCreateMutex();
    contacts_store.io_lock = xSemaphoreCreateMutex();
    contacts_store.contacts =
        solar_os_memory_calloc(SOLAR_OS_CONTACT_CAPACITY,
                               sizeof(*contacts_store.contacts),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "contacts.records");
    contacts_store.endpoints =
        solar_os_memory_calloc(SOLAR_OS_ENDPOINT_CAPACITY,
                               sizeof(*contacts_store.endpoints),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "contacts.endpoints");
    contacts_store.scratch =
        solar_os_memory_calloc(1U,
                               sizeof(*contacts_store.scratch),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "contacts.persist");
    if (contacts_store.lock == NULL ||
        contacts_store.io_lock == NULL ||
        contacts_store.contacts == NULL ||
        contacts_store.endpoints == NULL ||
        contacts_store.scratch == NULL) {
        return ESP_ERR_NO_MEM;
    }

    contacts_store.records_in_psram =
        solar_os_memory_is_external(contacts_store.contacts) &&
        solar_os_memory_is_external(contacts_store.endpoints) &&
        solar_os_memory_is_external(contacts_store.scratch);
    contacts_store.generation = 1U;
    contacts_store.next_contact_id = 1U;
    contacts_store.next_endpoint_id = 1U;
    contacts_store.storage_error = ESP_ERR_INVALID_STATE;

    esp_err_t error = contacts_prepare_store_path();
    if (error == ESP_OK) {
        error = contacts_restore();
    }
    if (error == ESP_OK) {
        contacts_store.persistent = true;
        contacts_store.storage_error = ESP_OK;
    } else {
        contacts_store.persistent = false;
        contacts_store.storage_error = error;
    }
    contacts_store.initialized = true;

    if (error == ESP_ERR_NOT_FOUND) {
        (void)contacts_persist_current();
    }
    return ESP_OK;
}
esp_err_t solar_os_contacts_get_status(solar_os_contacts_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }

    contacts_lock();
    *status = (solar_os_contacts_status_t){
        .initialized = contacts_store.initialized,
        .records_in_psram = contacts_store.records_in_psram,
        .persistent = contacts_store.persistent,
        .contact_capacity = SOLAR_OS_CONTACT_CAPACITY,
        .endpoint_capacity = SOLAR_OS_ENDPOINT_CAPACITY,
        .contact_count = contacts_store.contact_count,
        .endpoint_count = contacts_store.endpoint_count,
        .generation = contacts_store.generation,
        .evicted = contacts_store.evicted,
        .storage_bytes = CONTACTS_STORE_BYTES,
        .storage_error = contacts_store.storage_error,
    };
    contacts_unlock();
    return ESP_OK;
}

size_t solar_os_contacts_snapshot(solar_os_contact_t *contacts,
                                  size_t max_contacts,
                                  bool filter_trust,
                                  solar_os_contact_trust_t trust,
                                  size_t *total_contacts)
{
    if (solar_os_contacts_init() != ESP_OK ||
        (filter_trust && !contacts_trust_valid(trust))) {
        if (total_contacts != NULL) {
            *total_contacts = 0U;
        }
        return 0U;
    }

    size_t copied = 0U;
    size_t total = 0U;
    contacts_lock();
    for (size_t index = 0; index < SOLAR_OS_CONTACT_CAPACITY; index++) {
        const contacts_contact_slot_t *slot = &contacts_store.contacts[index];
        if (!slot->active) {
            continue;
        }
        solar_os_contact_t snapshot;
        contacts_fill_contact_locked(slot, &snapshot);
        bool match = !filter_trust;
        if (filter_trust) {
            for (size_t endpoint_index = 0;
                 endpoint_index < SOLAR_OS_ENDPOINT_CAPACITY;
                 endpoint_index++) {
                const contacts_endpoint_slot_t *endpoint =
                    &contacts_store.endpoints[endpoint_index];
                if (endpoint->active &&
                    endpoint->record.contact_id == snapshot.id &&
                    endpoint->record.trust == trust) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) {
            continue;
        }
        total++;
        if (contacts != NULL && copied < max_contacts) {
            contacts[copied++] = snapshot;
        }
    }
    contacts_unlock();

    if (total_contacts != NULL) {
        *total_contacts = total;
    }
    return copied;
}

esp_err_t solar_os_contacts_get(solar_os_contact_id_t id,
                                solar_os_contact_t *contact)
{
    if (contact == NULL || id == SOLAR_OS_CONTACT_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }

    contacts_lock();
    const int index = contacts_find_contact_locked(id);
    if (index >= 0) {
        contacts_fill_contact_locked(&contacts_store.contacts[index], contact);
    }
    contacts_unlock();
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

size_t solar_os_contacts_endpoint_snapshot(solar_os_contact_id_t contact_id,
                                           solar_os_endpoint_t *endpoints,
                                           size_t max_endpoints)
{
    if (solar_os_contacts_init() != ESP_OK) {
        return 0U;
    }
    size_t copied = 0U;
    contacts_lock();
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        const contacts_endpoint_slot_t *slot = &contacts_store.endpoints[index];
        if (slot->active &&
            (contact_id == SOLAR_OS_CONTACT_ID_NONE ||
             slot->record.contact_id == contact_id)) {
            if (endpoints != NULL && copied < max_endpoints) {
                endpoints[copied] = slot->record;
            }
            if (copied < max_endpoints) {
                copied++;
            }
        }
    }
    contacts_unlock();
    return copied;
}

esp_err_t solar_os_contacts_get_endpoint(solar_os_endpoint_id_t id,
                                         solar_os_endpoint_t *endpoint)
{
    if (endpoint == NULL || id == SOLAR_OS_ENDPOINT_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }
    contacts_lock();
    const int index = contacts_find_endpoint_id_locked(id);
    if (index >= 0) {
        *endpoint = contacts_store.endpoints[index].record;
    }
    contacts_unlock();
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_contacts_find_endpoint(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address,
    size_t address_len,
    solar_os_endpoint_t *endpoint)
{
    if (!contacts_provider_valid(provider) ||
        address == NULL ||
        address_len == 0U ||
        address_len > SOLAR_OS_MESSAGING_ADDRESS_MAX ||
        endpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }
    contacts_lock();
    const int index =
        contacts_find_endpoint_address_locked(provider, address, address_len);
    if (index >= 0) {
        *endpoint = contacts_store.endpoints[index].record;
    }
    contacts_unlock();
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void contacts_default_name(solar_os_messaging_provider_id_t provider,
                                  const uint8_t *address,
                                  size_t address_len,
                                  char *name,
                                  size_t name_len)
{
    const char *prefix = solar_os_messaging_provider_name(provider);
    const size_t start = address_len > 4U ? address_len - 4U : 0U;
    int written = snprintf(name, name_len, "%s-", prefix);
    size_t used = written > 0 ? (size_t)written : 0U;
    for (size_t index = start;
         index < address_len && used + 2U < name_len;
         index++) {
        written = snprintf(name + used, name_len - used, "%02x", address[index]);
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }
}

static esp_err_t contacts_upsert_discovered(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address,
    size_t address_len,
    const char *display_name,
    uint32_t capabilities,
    uint64_t last_seen_ms,
    const void *provider_metadata,
    size_t provider_metadata_len,
    solar_os_contact_id_t *contact_id,
    solar_os_endpoint_id_t *endpoint_id,
    bool persist,
    bool allow_eviction)
{
    if (!contacts_provider_valid(provider) ||
        address == NULL ||
        address_len == 0U ||
        address_len > SOLAR_OS_MESSAGING_ADDRESS_MAX ||
        provider_metadata_len > SOLAR_OS_CONTACT_PROVIDER_METADATA_MAX ||
        (provider_metadata_len > 0U && provider_metadata == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }

    solar_os_contact_id_t result_contact = SOLAR_OS_CONTACT_ID_NONE;
    solar_os_endpoint_id_t result_endpoint = SOLAR_OS_ENDPOINT_ID_NONE;
    contacts_lock();
    int endpoint_index =
        contacts_find_endpoint_address_locked(provider, address, address_len);
    if (endpoint_index >= 0) {
        contacts_endpoint_slot_t *endpoint =
            &contacts_store.endpoints[endpoint_index];
        endpoint->record.capabilities = capabilities;
        endpoint->record.last_seen_ms =
            last_seen_ms != 0U ? last_seen_ms : contacts_now_ms();
        endpoint->record.provider_metadata_len =
            (uint8_t)provider_metadata_len;
        memset(endpoint->record.provider_metadata,
               0,
               sizeof(endpoint->record.provider_metadata));
        if (provider_metadata_len > 0U) {
            memcpy(endpoint->record.provider_metadata,
                   provider_metadata,
                   provider_metadata_len);
        }
        const int contact_index =
            contacts_find_contact_locked(endpoint->record.contact_id);
        if (contact_index >= 0) {
            contacts_store.contacts[contact_index].record.updated_ms =
                endpoint->record.last_seen_ms;
            result_contact =
                contacts_store.contacts[contact_index].record.id;
        }
        result_endpoint = endpoint->record.id;
        contacts_changed_locked();
        contacts_unlock();
        if (persist) (void)contacts_persist_current();
        if (contact_id != NULL) {
            *contact_id = result_contact;
        }
        if (endpoint_id != NULL) {
            *endpoint_id = result_endpoint;
        }
        return ESP_OK;
    }

    int contact_index = contacts_free_contact_slot_locked();
    if (contact_index < 0 && allow_eviction && contacts_evict_oldest_locked()) {
        contact_index = contacts_free_contact_slot_locked();
    }
    endpoint_index = contacts_free_endpoint_slot_locked();
    if (contact_index < 0 || endpoint_index < 0) {
        contacts_unlock();
        return ESP_ERR_NO_MEM;
    }

    result_contact = contacts_allocate_id_locked();
    result_endpoint = contacts_allocate_endpoint_id_locked();
    if (result_contact == SOLAR_OS_CONTACT_ID_NONE ||
        result_endpoint == SOLAR_OS_ENDPOINT_ID_NONE) {
        contacts_unlock();
        return ESP_ERR_NO_MEM;
    }

    const uint64_t now =
        last_seen_ms != 0U ? last_seen_ms : contacts_now_ms();
    contacts_contact_slot_t *contact = &contacts_store.contacts[contact_index];
    memset(contact, 0, sizeof(*contact));
    contact->active = true;
    contact->record.id = result_contact;
    contact->record.created_ms = now;
    contact->record.updated_ms = now;
    if (display_name != NULL && display_name[0] != '\0') {
        strlcpy(contact->record.display_name,
                display_name,
                sizeof(contact->record.display_name));
    } else {
        contacts_default_name(provider,
                              address,
                              address_len,
                              contact->record.display_name,
                              sizeof(contact->record.display_name));
    }

    contacts_endpoint_slot_t *endpoint =
        &contacts_store.endpoints[endpoint_index];
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->active = true;
    endpoint->record.id = result_endpoint;
    endpoint->record.contact_id = result_contact;
    endpoint->record.provider = provider;
    endpoint->record.trust = SOLAR_OS_CONTACT_TRUST_DISCOVERED;
    endpoint->record.capabilities = capabilities;
    endpoint->record.last_seen_ms = now;
    endpoint->record.address.length = (uint8_t)address_len;
    memcpy(endpoint->record.address.bytes, address, address_len);
    endpoint->record.provider_metadata_len = (uint8_t)provider_metadata_len;
    if (provider_metadata_len > 0U) {
        memcpy(endpoint->record.provider_metadata,
               provider_metadata,
               provider_metadata_len);
    }

    contacts_store.contact_count++;
    contacts_store.endpoint_count++;
    contacts_changed_locked();
    contacts_unlock();
    if (persist) (void)contacts_persist_current();

    if (contact_id != NULL) {
        *contact_id = result_contact;
    }
    if (endpoint_id != NULL) {
        *endpoint_id = result_endpoint;
    }
    return ESP_OK;
}

esp_err_t solar_os_contacts_upsert_discovered(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address, size_t address_len, const char *display_name,
    uint32_t capabilities, uint64_t last_seen_ms,
    const void *provider_metadata, size_t provider_metadata_len,
    solar_os_contact_id_t *contact_id, solar_os_endpoint_id_t *endpoint_id)
{
    return contacts_upsert_discovered(provider, address, address_len, display_name,
        capabilities, last_seen_ms, provider_metadata, provider_metadata_len,
        contact_id, endpoint_id, true, true);
}

esp_err_t solar_os_contacts_import_discovered(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address, size_t address_len, const char *display_name,
    uint32_t capabilities, uint64_t last_seen_ms,
    const void *provider_metadata, size_t provider_metadata_len,
    solar_os_contact_id_t *contact_id, solar_os_endpoint_id_t *endpoint_id)
{
    return contacts_upsert_discovered(provider, address, address_len, display_name,
        capabilities, last_seen_ms, provider_metadata, provider_metadata_len,
        contact_id, endpoint_id, false, false);
}

esp_err_t solar_os_contacts_flush(void)
{
    esp_err_t error = solar_os_contacts_init();
    return error == ESP_OK ? contacts_persist_current() : error;
}

esp_err_t solar_os_contacts_rename(solar_os_contact_id_t id,
                                   const char *display_name)
{
    if (id == SOLAR_OS_CONTACT_ID_NONE ||
        display_name == NULL ||
        display_name[0] == '\0' ||
        strlen(display_name) > SOLAR_OS_CONTACT_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }
    contacts_lock();
    const int index = contacts_find_contact_locked(id);
    if (index >= 0) {
        strlcpy(contacts_store.contacts[index].record.display_name,
                display_name,
                sizeof(contacts_store.contacts[index].record.display_name));
        contacts_store.contacts[index].record.updated_ms = contacts_now_ms();
        contacts_changed_locked();
    }
    contacts_unlock();
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)contacts_persist_current();
    return ESP_OK;
}

esp_err_t solar_os_contacts_set_trust(solar_os_contact_id_t contact_id,
                                      solar_os_endpoint_id_t endpoint_id,
                                      solar_os_contact_trust_t trust)
{
    if (contact_id == SOLAR_OS_CONTACT_ID_NONE ||
        !contacts_trust_valid(trust)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }
    contacts_lock();
    const int contact_index = contacts_find_contact_locked(contact_id);
    size_t changed = 0U;
    if (contact_index >= 0) {
        for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
            contacts_endpoint_slot_t *endpoint =
                &contacts_store.endpoints[index];
            if (endpoint->active &&
                endpoint->record.contact_id == contact_id &&
                (endpoint_id == SOLAR_OS_ENDPOINT_ID_NONE ||
                 endpoint->record.id == endpoint_id)) {
                endpoint->record.trust = trust;
                changed++;
            }
        }
        if (changed > 0U) {
            contacts_store.contacts[contact_index].record.updated_ms =
                contacts_now_ms();
            contacts_changed_locked();
        }
    }
    contacts_unlock();
    if (contact_index < 0 || changed == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)contacts_persist_current();
    return ESP_OK;
}

esp_err_t solar_os_contacts_remove(solar_os_contact_id_t id)
{
    if (id == SOLAR_OS_CONTACT_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }
    contacts_lock();
    const bool found = contacts_find_contact_locked(id) >= 0;
    if (found) {
        contacts_remove_locked(id);
        contacts_changed_locked();
    }
    contacts_unlock();
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)contacts_persist_current();
    return ESP_OK;
}

esp_err_t solar_os_contacts_link(solar_os_contact_id_t target_id,
                                 solar_os_contact_id_t source_id)
{
    if (target_id == SOLAR_OS_CONTACT_ID_NONE ||
        source_id == SOLAR_OS_CONTACT_ID_NONE ||
        target_id == source_id) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }

    contacts_lock();
    const int target_index = contacts_find_contact_locked(target_id);
    const int source_index = contacts_find_contact_locked(source_id);
    if (target_index < 0 || source_index < 0) {
        contacts_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t index = 0; index < SOLAR_OS_ENDPOINT_CAPACITY; index++) {
        contacts_endpoint_slot_t *endpoint = &contacts_store.endpoints[index];
        if (endpoint->active && endpoint->record.contact_id == source_id) {
            endpoint->record.contact_id = target_id;
        }
    }
    contacts_store.contacts[target_index].record.updated_ms = contacts_now_ms();
    memset(&contacts_store.contacts[source_index],
           0,
           sizeof(contacts_store.contacts[source_index]));
    if (contacts_store.contact_count > 0U) {
        contacts_store.contact_count--;
    }
    contacts_changed_locked();
    contacts_unlock();
    (void)contacts_persist_current();
    return ESP_OK;
}
