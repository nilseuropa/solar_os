#include "solar_os_clipboard.h"

#include <string.h>

#include "solar_os_memory.h"
#include "solar_os_sync.h"

static char *clipboard_data;
static size_t clipboard_len;
static StaticSemaphore_t clipboard_mutex_storage;
static SemaphoreHandle_t clipboard_mutex;

static bool clipboard_lock(void)
{
    SemaphoreHandle_t mutex = solar_os_mutex_get(&clipboard_mutex, &clipboard_mutex_storage);
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static void clipboard_unlock(void) { (void)xSemaphoreGive(clipboard_mutex); }

esp_err_t solar_os_clipboard_set(const char *data, size_t len)
{
    if (len > SOLAR_OS_CLIPBOARD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *next = NULL;
    if (len > 0) {
        next = solar_os_memory_alloc(len + 1,
                                     SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                     "clipboard");
        if (next == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(next, data, len);
        next[len] = '\0';
    }

    if (!clipboard_lock()) { solar_os_memory_free(next); return ESP_ERR_NO_MEM; }
    char *previous = clipboard_data;
    clipboard_data = next;
    clipboard_len = len;
    clipboard_unlock();
    solar_os_memory_free(previous);
    return ESP_OK;
}

esp_err_t solar_os_clipboard_read(size_t offset, void *data, size_t capacity,
    size_t *read_size, size_t *total_size)
{
    if (read_size == NULL || total_size == NULL || (data == NULL && capacity > 0)) return ESP_ERR_INVALID_ARG;
    *read_size = 0; *total_size = 0;
    if (!clipboard_lock()) return ESP_ERR_NO_MEM;
    *total_size = clipboard_len;
    if (offset < clipboard_len) {
        size_t size = clipboard_len - offset;
        if (size > capacity) size = capacity;
        if (size > 0) memcpy(data, clipboard_data + offset, size);
        *read_size = size;
    }
    clipboard_unlock();
    return ESP_OK;
}

const char *solar_os_clipboard_data(size_t *len)
{
    if (len != NULL) *len = 0;
    if (!clipboard_lock()) return NULL;
    if (len != NULL) {
        *len = clipboard_len;
    }
    const char *data = clipboard_data;
    clipboard_unlock();
    return data;
}

size_t solar_os_clipboard_size(void)
{
    if (!clipboard_lock()) return 0;
    size_t size = clipboard_len;
    clipboard_unlock();
    return size;
}

void solar_os_clipboard_clear(void)
{
    if (!clipboard_lock()) return;
    char *previous = clipboard_data;
    clipboard_data = NULL;
    clipboard_len = 0;
    clipboard_unlock();
    solar_os_memory_free(previous);
}
