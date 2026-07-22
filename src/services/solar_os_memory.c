#include "solar_os_memory.h"

#include <limits.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "solar_os_log.h"

static const char *TAG = "memory";
static portMUX_TYPE memory_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_memory_class_stats_t memory_stats[SOLAR_OS_MEMORY_CLASS_COUNT];
static bool last_failure_valid;
static solar_os_memory_class_t last_failure_class;
static size_t last_failure_size;
static char last_failure_tag[SOLAR_OS_MEMORY_TAG_MAX];

static bool memory_class_valid(solar_os_memory_class_t memory_class)
{
    return memory_class >= SOLAR_OS_MEMORY_INTERNAL_CRITICAL &&
           memory_class < SOLAR_OS_MEMORY_CLASS_COUNT;
}

const char *solar_os_memory_class_name(solar_os_memory_class_t memory_class)
{
    switch (memory_class) {
    case SOLAR_OS_MEMORY_INTERNAL_CRITICAL:
        return "internal-critical";
    case SOLAR_OS_MEMORY_INTERNAL_PREFERRED:
        return "internal-preferred";
    case SOLAR_OS_MEMORY_DMA:
        return "dma";
    case SOLAR_OS_MEMORY_EXTERNAL_REQUIRED:
        return "external-required";
    case SOLAR_OS_MEMORY_EXTERNAL_PREFERRED:
        return "external-preferred";
    case SOLAR_OS_MEMORY_TRANSIENT:
        return "transient";
    case SOLAR_OS_MEMORY_CLASS_COUNT:
    default:
        return "invalid";
    }
}

static void memory_record_request(solar_os_memory_class_t memory_class, size_t size)
{
    portENTER_CRITICAL(&memory_stats_lock);
    memory_stats[memory_class].requests++;
    memory_stats[memory_class].requested_bytes += size;
    portEXIT_CRITICAL(&memory_stats_lock);
}

static void memory_record_success(solar_os_memory_class_t memory_class, bool fallback)
{
    portENTER_CRITICAL(&memory_stats_lock);
    memory_stats[memory_class].successes++;
    if (fallback) {
        memory_stats[memory_class].fallbacks++;
    }
    portEXIT_CRITICAL(&memory_stats_lock);
}

static void memory_record_failure(solar_os_memory_class_t memory_class,
                                  size_t size,
                                  const char *tag)
{
    portENTER_CRITICAL(&memory_stats_lock);
    memory_stats[memory_class].failures++;
    last_failure_valid = true;
    last_failure_class = memory_class;
    last_failure_size = size;
    strlcpy(last_failure_tag, tag != NULL ? tag : "unknown", sizeof(last_failure_tag));
    portEXIT_CRITICAL(&memory_stats_lock);
}

static bool memory_internal_reserve_allows(size_t size)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_now = heap_caps_get_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);

    if (size > SIZE_MAX - SOLAR_OS_MEMORY_INTERNAL_RESERVE_BYTES) {
        return false;
    }
    const size_t required = size + SOLAR_OS_MEMORY_INTERNAL_RESERVE_BYTES;
    return free_now >= required && largest >= required;
}

static bool memory_internal_fallback_allowed(size_t size)
{
    return size <= SOLAR_OS_MEMORY_INTERNAL_FALLBACK_MAX_BYTES &&
           memory_internal_reserve_allows(size);
}

static void *memory_allocate(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag,
                             bool zero)
{
    if (count == 0 || size == 0 || !memory_class_valid(memory_class) ||
        count > SIZE_MAX / size) {
        return NULL;
    }

    const size_t requested = count * size;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    void *ptr = NULL;
    bool fallback = false;

    memory_record_request(memory_class, requested);

    switch (memory_class) {
    case SOLAR_OS_MEMORY_INTERNAL_CRITICAL:
        ptr = zero ? heap_caps_calloc(count, size, internal_caps) :
                     heap_caps_malloc(requested, internal_caps);
        break;
    case SOLAR_OS_MEMORY_INTERNAL_PREFERRED:
        if (memory_internal_reserve_allows(requested)) {
            ptr = zero ? heap_caps_calloc(count, size, internal_caps) :
                         heap_caps_malloc(requested, internal_caps);
        }
        if (ptr == NULL) {
            ptr = zero ? heap_caps_calloc(count, size, external_caps) :
                         heap_caps_malloc(requested, external_caps);
            fallback = ptr != NULL;
        }
        break;
    case SOLAR_OS_MEMORY_DMA:
        ptr = zero ? heap_caps_calloc(count, size, internal_caps | MALLOC_CAP_DMA) :
                     heap_caps_malloc(requested, internal_caps | MALLOC_CAP_DMA);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_REQUIRED:
        ptr = zero ? heap_caps_calloc(count, size, external_caps) :
                     heap_caps_malloc(requested, external_caps);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_PREFERRED:
    case SOLAR_OS_MEMORY_TRANSIENT: {
        const bool external_available = heap_caps_get_total_size(external_caps) > 0;
        if (external_available) {
            ptr = zero ? heap_caps_calloc(count, size, external_caps) :
                         heap_caps_malloc(requested, external_caps);
        }
        if (ptr == NULL &&
            (!external_available || memory_internal_fallback_allowed(requested))) {
            ptr = zero ? heap_caps_calloc(count, size, internal_caps) :
                         heap_caps_malloc(requested, internal_caps);
            fallback = ptr != NULL;
        }
        break;
    }
    case SOLAR_OS_MEMORY_CLASS_COUNT:
    default:
        break;
    }

    if (ptr != NULL) {
        memory_record_success(memory_class, fallback);
        return ptr;
    }

    memory_record_failure(memory_class, requested, tag);
    SOLAR_OS_LOGW(TAG,
                  "%s allocation failed tag=%s size=%u internal_free=%u internal_max=%u external_free=%u external_max=%u",
                  solar_os_memory_class_name(memory_class),
                  tag != NULL ? tag : "unknown",
                  (unsigned)requested,
                  (unsigned)heap_caps_get_free_size(internal_caps),
                  (unsigned)heap_caps_get_largest_free_block(internal_caps),
                  (unsigned)heap_caps_get_free_size(external_caps),
                  (unsigned)heap_caps_get_largest_free_block(external_caps));
    return NULL;
}

void *solar_os_memory_alloc(size_t size,
                            solar_os_memory_class_t memory_class,
                            const char *tag)
{
    return memory_allocate(1, size, memory_class, tag, false);
}

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    return memory_allocate(count, size, memory_class, tag, true);
}

void *solar_os_memory_realloc(void *ptr,
                              size_t size,
                              solar_os_memory_class_t memory_class,
                              const char *tag)
{
    if (ptr == NULL) {
        return solar_os_memory_alloc(size, memory_class, tag);
    }
    if (size == 0) {
        solar_os_memory_free(ptr);
        return NULL;
    }
    if (!memory_class_valid(memory_class)) {
        return NULL;
    }

    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    void *next = NULL;
    bool fallback = false;

    memory_record_request(memory_class, size);

    switch (memory_class) {
    case SOLAR_OS_MEMORY_INTERNAL_CRITICAL:
        next = heap_caps_realloc(ptr, size, internal_caps);
        break;
    case SOLAR_OS_MEMORY_INTERNAL_PREFERRED:
        if (memory_internal_reserve_allows(size)) {
            next = heap_caps_realloc(ptr, size, internal_caps);
        }
        if (next == NULL) {
            next = heap_caps_realloc(ptr, size, external_caps);
            fallback = next != NULL;
        }
        break;
    case SOLAR_OS_MEMORY_DMA:
        next = heap_caps_realloc(ptr, size, internal_caps | MALLOC_CAP_DMA);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_REQUIRED:
        next = heap_caps_realloc(ptr, size, external_caps);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_PREFERRED:
    case SOLAR_OS_MEMORY_TRANSIENT: {
        const bool external_available = heap_caps_get_total_size(external_caps) > 0;
        if (external_available) {
            next = heap_caps_realloc(ptr, size, external_caps);
        }
        if (next == NULL &&
            (!external_available || memory_internal_fallback_allowed(size))) {
            next = heap_caps_realloc(ptr, size, internal_caps);
            fallback = next != NULL;
        }
        break;
    }
    case SOLAR_OS_MEMORY_CLASS_COUNT:
    default:
        break;
    }

    if (next != NULL) {
        memory_record_success(memory_class, fallback);
        return next;
    }

    memory_record_failure(memory_class, size, tag);
    SOLAR_OS_LOGW(TAG,
                  "%s reallocation failed tag=%s size=%u internal_free=%u internal_max=%u external_free=%u external_max=%u",
                  solar_os_memory_class_name(memory_class),
                  tag != NULL ? tag : "unknown",
                  (unsigned)size,
                  (unsigned)heap_caps_get_free_size(internal_caps),
                  (unsigned)heap_caps_get_largest_free_block(internal_caps),
                  (unsigned)heap_caps_get_free_size(external_caps),
                  (unsigned)heap_caps_get_largest_free_block(external_caps));
    return NULL;
}

void solar_os_memory_free(void *ptr)
{
    heap_caps_free(ptr);
}

static void memory_region_status(uint32_t caps, solar_os_memory_region_status_t *status)
{
    status->total = heap_caps_get_total_size(caps);
    status->free = heap_caps_get_free_size(caps);
    status->minimum_free = heap_caps_get_minimum_free_size(caps);
    status->largest_free = heap_caps_get_largest_free_block(caps);
}

void solar_os_memory_get_status(solar_os_memory_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    memory_region_status(MALLOC_CAP_INTERNAL, &status->internal);
    memory_region_status(MALLOC_CAP_SPIRAM, &status->external);
    memory_region_status(MALLOC_CAP_DMA, &status->dma);
    status->internal_reserve = SOLAR_OS_MEMORY_INTERNAL_RESERVE_BYTES;
    status->internal_fallback_max = SOLAR_OS_MEMORY_INTERNAL_FALLBACK_MAX_BYTES;

    portENTER_CRITICAL(&memory_stats_lock);
    memcpy(status->classes, memory_stats, sizeof(memory_stats));
    status->last_failure_valid = last_failure_valid;
    status->last_failure_class = last_failure_class;
    status->last_failure_size = last_failure_size;
    memcpy(status->last_failure_tag, last_failure_tag, sizeof(last_failure_tag));
    portEXIT_CRITICAL(&memory_stats_lock);
}

bool solar_os_memory_is_external(const void *ptr)
{
    return ptr != NULL && esp_ptr_external_ram(ptr);
}
