#include "solar_os_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_memory.h"

#define LOG_NVS_NAMESPACE "log"
#define LOG_NVS_LEVEL_KEY "level"
#define LOG_NVS_CDC_KEY "cdc"
#define LOG_NVS_CDC_EXPLICIT_KEY "cdc_set"

static SemaphoreHandle_t log_mutex;
static solar_os_log_entry_t *log_ring;
static size_t log_capacity;
static size_t log_count;
static size_t log_head;
static size_t log_bytes;
static uint32_t log_next_sequence = 1;
static uint32_t log_dropped;
static bool log_initialized;
static bool log_ring_in_psram;
static bool log_config_loaded;
static bool log_cdc_enabled;
static solar_os_log_level_t log_level = SOLAR_OS_LOG_LEVEL_INFO;

static bool log_level_valid(solar_os_log_level_t level)
{
    return level >= SOLAR_OS_LOG_LEVEL_ERROR && level <= SOLAR_OS_LOG_LEVEL_DEBUG;
}

static esp_log_level_t log_esp_level(solar_os_log_level_t level)
{
    switch (level) {
    case SOLAR_OS_LOG_LEVEL_ERROR:
        return ESP_LOG_ERROR;
    case SOLAR_OS_LOG_LEVEL_WARN:
        return ESP_LOG_WARN;
    case SOLAR_OS_LOG_LEVEL_DEBUG:
        return ESP_LOG_DEBUG;
    case SOLAR_OS_LOG_LEVEL_INFO:
    default:
        return ESP_LOG_INFO;
    }
}

static void log_mirror_to_cdc(solar_os_log_level_t level, const char *tag, const char *message)
{
    switch (level) {
    case SOLAR_OS_LOG_LEVEL_ERROR:
        ESP_LOGE(tag, "%s", message);
        break;
    case SOLAR_OS_LOG_LEVEL_WARN:
        ESP_LOGW(tag, "%s", message);
        break;
    case SOLAR_OS_LOG_LEVEL_DEBUG:
        ESP_LOGD(tag, "%s", message);
        break;
    case SOLAR_OS_LOG_LEVEL_INFO:
    default:
        ESP_LOGI(tag, "%s", message);
        break;
    }
}

static esp_err_t log_ensure_mutex(void)
{
    if (log_mutex != NULL) {
        return ESP_OK;
    }

    log_mutex = xSemaphoreCreateMutex();
    return log_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void log_lock(void)
{
    if (log_mutex != NULL) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
    }
}

static void log_unlock(void)
{
    if (log_mutex != NULL) {
        xSemaphoreGive(log_mutex);
    }
}

static void log_load_config(void)
{
    if (log_config_loaded) {
        return;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LOG_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_INITIALIZED) {
        return;
    }
    if (ret != ESP_OK) {
        log_config_loaded = true;
        return;
    }

    uint8_t stored_level = (uint8_t)log_level;
    if (nvs_get_u8(nvs, LOG_NVS_LEVEL_KEY, &stored_level) == ESP_OK &&
        stored_level <= (uint8_t)SOLAR_OS_LOG_LEVEL_DEBUG) {
        log_level = (solar_os_log_level_t)stored_level;
    }

    uint8_t cdc_explicit = 0;
    if (nvs_get_u8(nvs, LOG_NVS_CDC_EXPLICIT_KEY, &cdc_explicit) == ESP_OK &&
        cdc_explicit != 0) {
        uint8_t stored_cdc = log_cdc_enabled ? 1U : 0U;
        if (nvs_get_u8(nvs, LOG_NVS_CDC_KEY, &stored_cdc) == ESP_OK) {
            log_cdc_enabled = stored_cdc != 0;
        }
    }

    nvs_close(nvs);
    log_config_loaded = true;
}

static esp_err_t log_save_level_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, LOG_NVS_LEVEL_KEY, (uint8_t)log_level);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t log_save_cdc_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, LOG_NVS_CDC_KEY, log_cdc_enabled ? 1U : 0U);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, LOG_NVS_CDC_EXPLICIT_KEY, 1U);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t log_allocate_ring(void)
{
    if (log_ring != NULL) {
        return ESP_OK;
    }

    const size_t bytes = sizeof(solar_os_log_entry_t) * SOLAR_OS_LOG_CAPACITY;
    log_ring = solar_os_memory_calloc(SOLAR_OS_LOG_CAPACITY,
                                      sizeof(solar_os_log_entry_t),
                                      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                      "log.ring");
    log_ring_in_psram = solar_os_memory_is_external(log_ring);
    if (log_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }

    log_capacity = SOLAR_OS_LOG_CAPACITY;
    log_bytes = bytes;
    return ESP_OK;
}

static void log_store_locked(solar_os_log_level_t level,
                             const char *tag,
                             const char *message,
                             bool truncated)
{
    if (log_ring == NULL || log_capacity == 0) {
        return;
    }

    solar_os_log_entry_t *entry = &log_ring[log_head];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = log_next_sequence++;
    if (log_next_sequence == 0) {
        log_next_sequence = 1;
    }
    entry->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    entry->level = level;
    entry->truncated = truncated;
    strlcpy(entry->tag, tag != NULL && tag[0] != '\0' ? tag : "solar_os", sizeof(entry->tag));
    strlcpy(entry->message, message != NULL ? message : "", sizeof(entry->message));

    log_head = (log_head + 1U) % log_capacity;
    if (log_count < log_capacity) {
        log_count++;
    } else {
        log_dropped++;
    }
}

esp_err_t solar_os_log_init(void)
{
    esp_err_t ret = log_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    log_lock();
    const bool already_initialized = log_initialized;
    if (already_initialized && log_config_loaded) {
        log_unlock();
        return ESP_OK;
    }
    log_unlock();

    if (!log_config_loaded) {
        log_load_config();
    }

    if (already_initialized) {
        return ESP_OK;
    }

    ret = log_allocate_ring();
    if (ret != ESP_OK) {
        return ret;
    }

    log_lock();
    log_initialized = true;
    log_unlock();
    return ESP_OK;
}

esp_err_t solar_os_log_clear(void)
{
    esp_err_t ret = solar_os_log_init();
    if (ret != ESP_OK) {
        return ret;
    }

    log_lock();
    if (log_ring != NULL) {
        memset(log_ring, 0, sizeof(solar_os_log_entry_t) * log_capacity);
    }
    log_count = 0;
    log_head = 0;
    log_dropped = 0;
    log_unlock();
    return ESP_OK;
}

esp_err_t solar_os_log_set_level(solar_os_log_level_t level)
{
    if (!log_level_valid(level)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_log_init();
    if (ret != ESP_OK) {
        return ret;
    }

    log_lock();
    log_level = level;
    log_unlock();

    esp_log_level_set("solar_os", log_esp_level(level));
    return log_save_level_config();
}

esp_err_t solar_os_log_set_cdc_enabled(bool enabled)
{
    esp_err_t ret = solar_os_log_init();
    if (ret != ESP_OK) {
        return ret;
    }

    log_lock();
    log_cdc_enabled = enabled;
    log_unlock();
    return log_save_cdc_config();
}

esp_err_t solar_os_log_get_status(solar_os_log_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_log_init();
    if (ret != ESP_OK) {
        return ret;
    }

    log_lock();
    *status = (solar_os_log_status_t){
        .initialized = log_initialized,
        .cdc_enabled = log_cdc_enabled,
        .ring_in_psram = log_ring_in_psram,
        .level = log_level,
        .capacity = log_capacity,
        .count = log_count,
        .bytes = log_bytes,
        .dropped = log_dropped,
    };
    log_unlock();
    return ESP_OK;
}

size_t solar_os_log_snapshot(solar_os_log_entry_t *entries,
                             size_t max_entries,
                             size_t *total_entries)
{
    if (solar_os_log_init() != ESP_OK) {
        if (total_entries != NULL) {
            *total_entries = 0;
        }
        return 0;
    }

    log_lock();
    const size_t total = log_count;
    if (total_entries != NULL) {
        *total_entries = total;
    }

    if (entries == NULL || max_entries == 0 || total == 0 || log_capacity == 0) {
        log_unlock();
        return 0;
    }

    const size_t selected = total < max_entries ? total : max_entries;
    const size_t oldest = (log_head + log_capacity - total) % log_capacity;
    const size_t skip = total - selected;
    size_t index = (oldest + skip) % log_capacity;
    for (size_t i = 0; i < selected; i++) {
        entries[i] = log_ring[index];
        index = (index + 1U) % log_capacity;
    }
    log_unlock();
    return selected;
}

size_t solar_os_log_snapshot_since(uint32_t after_sequence,
                                   solar_os_log_level_t threshold,
                                   solar_os_log_entry_t *entries,
                                   size_t max_entries,
                                   size_t *total_entries)
{
    if (total_entries != NULL) {
        *total_entries = 0;
    }
    if (!log_level_valid(threshold) || solar_os_log_init() != ESP_OK) {
        return 0;
    }

    log_lock();
    if (log_ring == NULL || log_count == 0 || log_capacity == 0) {
        log_unlock();
        return 0;
    }

    const size_t oldest = (log_head + log_capacity - log_count) % log_capacity;
    size_t copied = 0;
    size_t matched = 0;
    for (size_t i = 0; i < log_count; i++) {
        const size_t index = (oldest + i) % log_capacity;
        const solar_os_log_entry_t *entry = &log_ring[index];
        if (entry->sequence <= after_sequence || entry->level > threshold) {
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
    log_unlock();
    return copied;
}

esp_err_t solar_os_log_vwrite(solar_os_log_level_t level,
                              const char *tag,
                              const char *fmt,
                              va_list args)
{
    if (!log_level_valid(level) || fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xPortInIsrContext()) {
        return ESP_ERR_INVALID_STATE;
    }

    bool cdc_enabled = true;
    solar_os_log_level_t threshold = SOLAR_OS_LOG_LEVEL_INFO;
    const esp_err_t init_ret = solar_os_log_init();
    if (init_ret == ESP_OK) {
        log_lock();
        cdc_enabled = log_cdc_enabled;
        threshold = log_level;
        log_unlock();
    }

    if (level > threshold) {
        return ESP_OK;
    }

    char message[SOLAR_OS_LOG_MESSAGE_MAX];
    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(message, sizeof(message), fmt, copy);
    va_end(copy);
    const bool truncated = written < 0 || written >= (int)sizeof(message);
    if (written < 0) {
        strlcpy(message, "log format error", sizeof(message));
    }

    if (init_ret == ESP_OK) {
        log_lock();
        log_store_locked(level, tag, message, truncated);
        cdc_enabled = log_cdc_enabled;
        log_unlock();
    }

    if (cdc_enabled) {
        log_mirror_to_cdc(level, tag != NULL ? tag : "solar_os", message);
    }

    return init_ret;
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const esp_err_t ret = solar_os_log_vwrite(level, tag, fmt, args);
    va_end(args);
    return ret;
}

esp_err_t solar_os_log_buffer_hex(solar_os_log_level_t level,
                                  const char *tag,
                                  const void *data,
                                  size_t len)
{
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t offset = 0; offset < len; offset += 16) {
        char line[64];
        size_t pos = 0;
        const size_t line_len = len - offset > 16 ? 16 : len - offset;

        for (size_t i = 0; i < line_len && pos + 4 < sizeof(line); i++) {
            const int written = snprintf(&line[pos],
                                         sizeof(line) - pos,
                                         "%s%02x",
                                         i == 0 ? "" : " ",
                                         bytes[offset + i]);
            if (written <= 0) {
                break;
            }
            pos += (size_t)written;
        }

        esp_err_t ret = solar_os_log_write(level, tag, "%04x: %s", (unsigned)offset, line);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

const char *solar_os_log_level_name(solar_os_log_level_t level)
{
    switch (level) {
    case SOLAR_OS_LOG_LEVEL_ERROR:
        return "error";
    case SOLAR_OS_LOG_LEVEL_WARN:
        return "warn";
    case SOLAR_OS_LOG_LEVEL_INFO:
        return "info";
    case SOLAR_OS_LOG_LEVEL_DEBUG:
        return "debug";
    default:
        return "unknown";
    }
}

bool solar_os_log_parse_level(const char *text, solar_os_log_level_t *level)
{
    if (text == NULL || level == NULL) {
        return false;
    }

    if (strcmp(text, "error") == 0 || strcmp(text, "err") == 0) {
        *level = SOLAR_OS_LOG_LEVEL_ERROR;
        return true;
    }
    if (strcmp(text, "warn") == 0 || strcmp(text, "warning") == 0) {
        *level = SOLAR_OS_LOG_LEVEL_WARN;
        return true;
    }
    if (strcmp(text, "info") == 0) {
        *level = SOLAR_OS_LOG_LEVEL_INFO;
        return true;
    }
    if (strcmp(text, "debug") == 0) {
        *level = SOLAR_OS_LOG_LEVEL_DEBUG;
        return true;
    }
    return false;
}
