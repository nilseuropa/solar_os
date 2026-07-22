#include "solar_os_inbox.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_memory.h"

static SemaphoreHandle_t inbox_mutex;
static solar_os_inbox_entry_t *inbox_ring;
static size_t inbox_capacity;
static size_t inbox_count;
static size_t inbox_head;
static size_t inbox_unread;
static size_t inbox_bytes;
static uint32_t inbox_next_id = 1;
static uint32_t inbox_dropped;
static bool inbox_initialized;
static bool inbox_ring_in_psram;

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

static esp_err_t inbox_allocate_ring(void)
{
    if (inbox_ring != NULL) {
        return ESP_OK;
    }

    inbox_bytes = sizeof(solar_os_inbox_entry_t) * SOLAR_OS_INBOX_CAPACITY;
    inbox_ring = solar_os_memory_calloc(SOLAR_OS_INBOX_CAPACITY,
                                        sizeof(solar_os_inbox_entry_t),
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

    inbox_lock();
    solar_os_inbox_entry_t *entry = &inbox_ring[inbox_head];
    if (inbox_count == inbox_capacity) {
        if (entry->unread && inbox_unread > 0) {
            inbox_unread--;
        }
        inbox_dropped++;
    }

    memset(entry, 0, sizeof(*entry));
    entry->id = inbox_next_id++;
    if (inbox_next_id == 0) {
        inbox_next_id = 1;
    }
    entry->timestamp_ms = message->timestamp_ms;
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
    if (id != NULL) {
        *id = entry->id;
    }
    inbox_unlock();
    return ESP_OK;
}

static solar_os_inbox_entry_t *inbox_find_locked(uint32_t id)
{
    if (id == 0 || inbox_ring == NULL || inbox_capacity == 0) {
        return NULL;
    }

    const size_t oldest = (inbox_head + inbox_capacity - inbox_count) % inbox_capacity;
    for (size_t i = 0; i < inbox_count; i++) {
        solar_os_inbox_entry_t *entry = &inbox_ring[(oldest + i) % inbox_capacity];
        if (entry->id == id) {
            return entry;
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
    solar_os_inbox_entry_t *stored = inbox_find_locked(id);
    if (stored == NULL) {
        inbox_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *entry = *stored;
    if (mark_read && stored->unread) {
        stored->unread = false;
        if (inbox_unread > 0) {
            inbox_unread--;
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
    solar_os_inbox_entry_t *entry = inbox_find_locked(id);
    if (entry == NULL) {
        inbox_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    const bool unread = !read;
    if (entry->unread != unread) {
        entry->unread = unread;
        if (unread) {
            inbox_unread++;
        } else if (inbox_unread > 0) {
            inbox_unread--;
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
        const solar_os_inbox_entry_t *entry = &inbox_ring[index];
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
    memset(inbox_ring, 0, sizeof(solar_os_inbox_entry_t) * inbox_capacity);
    inbox_count = 0;
    inbox_head = 0;
    inbox_unread = 0;
    inbox_dropped = 0;
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
