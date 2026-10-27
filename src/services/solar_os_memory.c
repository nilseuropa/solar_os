#include "solar_os_memory.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "solar_os_log.h"

void __real_heap_caps_free(void *ptr);
void *__real_heap_caps_malloc(size_t bytes, uint32_t caps);
void *__real_heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps);
void *__real_heap_caps_aligned_calloc(size_t alignment, size_t count, size_t bytes, uint32_t caps);
void *__real_heap_caps_calloc(size_t count, size_t bytes, uint32_t caps);
void *__real_heap_caps_realloc(void *ptr, size_t bytes, uint32_t caps);
void *__real_heap_caps_malloc_default(size_t bytes);
void *__real_heap_caps_realloc_default(void *ptr, size_t bytes);
size_t __real_heap_caps_get_allocated_size(const void *ptr);
void IRAM_ATTR __wrap_heap_caps_free(void *ptr) { solar_os_memory_free(ptr); }
void IRAM_ATTR __wrap_free(void *ptr) { solar_os_memory_free(ptr); }

static const char *TAG = "memory";
static portMUX_TYPE memory_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_memory_class_stats_t memory_stats[SOLAR_OS_MEMORY_CLASS_COUNT];
static bool last_failure_valid;
static solar_os_memory_class_t last_failure_class;
static size_t last_failure_size;
static char last_failure_tag[SOLAR_OS_MEMORY_TAG_MAX];
static solar_os_memory_arena_t memory_arena;
static StaticSemaphore_t arena_mutex_storage;
static SemaphoreHandle_t arena_mutex;
static size_t (*memory_reclaimer)(void);
static atomic_bool reclaim_running;

void solar_os_memory_init(void)
{
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t reserve = SOLAR_OS_PSRAM_GENERAL_RESERVE_BYTES;
    const size_t largest = heap_caps_get_largest_free_block(caps);
    if (arena_mutex || !reserve || largest <= reserve * 2U) return;
    const size_t size = (largest - reserve) & ~(size_t)15U;
    void *buffer = __real_heap_caps_malloc(size, caps);
    if (!buffer) return;
    if (!solar_os_memory_arena_init(&memory_arena, buffer, size)) {
        heap_caps_free(buffer);
        return;
    }
    arena_mutex = xSemaphoreCreateMutexStatic(&arena_mutex_storage);
}

static void arena_lock(void) { (void)xSemaphoreTake(arena_mutex, portMAX_DELAY); }
static void arena_unlock(void) { (void)xSemaphoreGive(arena_mutex); }

static void *memory_external_alloc(size_t size, bool zero, bool resident)
{
    void *pointer = NULL;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (arena_mutex && (resident || size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES)) {
        arena_lock();
        pointer = solar_os_memory_arena_alloc(&memory_arena, size, resident);
        arena_unlock();
        /* Both regions remain usable; do not reclaim an idle provider while
         * the ordinary PSRAM heap can already satisfy this request. */
        if (!pointer) pointer = __real_heap_caps_aligned_alloc(16U, size, caps);
        if (!pointer && solar_os_memory_reclaim()) {
            arena_lock();
            pointer = solar_os_memory_arena_alloc(&memory_arena, size, resident);
            arena_unlock();
            if (!pointer) pointer = __real_heap_caps_aligned_alloc(16U, size, caps);
        }
    } else {
        pointer = __real_heap_caps_aligned_alloc(16U, size, caps);
        if (!pointer && arena_mutex) {
            arena_lock();
            pointer = solar_os_memory_arena_alloc(&memory_arena, size, true);
            arena_unlock();
        }
    }
    if (pointer && zero) memset(pointer, 0, size);
    return pointer;
}

static bool IRAM_ATTR managed_caps(uint32_t caps)
{
    uint32_t allowed = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#ifdef MALLOC_CAP_SIMD
    allowed |= MALLOC_CAP_SIMD;
#endif
    return arena_mutex && (caps & MALLOC_CAP_SPIRAM) && !(caps & ~allowed) &&
        !xPortInIsrContext();
}

void *IRAM_ATTR __wrap_heap_caps_malloc(size_t size, uint32_t caps)
{
    bool eligible = managed_caps(caps);
    if (size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && eligible)
        return memory_external_alloc(size, false, false);
    void *pointer = __real_heap_caps_malloc(size, caps);
    return !pointer && size && eligible ? memory_external_alloc(size, false, false) : pointer;
}

void *IRAM_ATTR __wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    bool eligible = alignment && alignment <= 16U && !(alignment & (alignment - 1U)) && managed_caps(caps);
    if (size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && eligible)
        return memory_external_alloc(size, false, false);
    void *pointer = __real_heap_caps_aligned_alloc(alignment, size, caps);
    return !pointer && size && eligible ? memory_external_alloc(size, false, false) : pointer;
}

void *IRAM_ATTR __wrap_heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    if (count && size > SIZE_MAX / count) return NULL;
    bool eligible = managed_caps(caps);
    if (count * size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && eligible)
        return memory_external_alloc(count * size, true, false);
    void *pointer = __real_heap_caps_calloc(count, size, caps);
    return !pointer && count && size && eligible ? memory_external_alloc(count * size, true, false) : pointer;
}

void *IRAM_ATTR __wrap_heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, uint32_t caps)
{
    if (count && size > SIZE_MAX / count) return NULL;
    bool eligible = alignment && alignment <= 16U && !(alignment & (alignment - 1U)) && managed_caps(caps);
    if (count * size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && eligible)
        return memory_external_alloc(count * size, true, false);
    void *pointer = __real_heap_caps_aligned_calloc(alignment, count, size, caps);
    return !pointer && count && size && eligible ? memory_external_alloc(count * size, true, false) : pointer;
}

void *IRAM_ATTR __wrap_heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    if (arena_mutex && (uintptr_t)ptr >= memory_arena.start &&
        (uintptr_t)ptr < memory_arena.end) {
        if (!size) { solar_os_memory_free(ptr); return NULL; }
        if (managed_caps(caps)) return solar_os_memory_realloc(ptr, size,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "sdk.realloc");
        arena_lock();
        size_t old = solar_os_memory_arena_size(&memory_arena, ptr);
        arena_unlock();
        void *next = __real_heap_caps_malloc(size, caps);
        if (next) { memcpy(next, ptr, size < old ? size : old); solar_os_memory_free(ptr); }
        return next;
    }
    if (size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && managed_caps(caps)) {
        size_t old = ptr ? heap_caps_get_allocated_size(ptr) : 0U;
        void *next = memory_external_alloc(size, false, false);
        if (next && ptr) { memcpy(next, ptr, size < old ? size : old); __real_heap_caps_free(ptr); }
        return next;
    }
    void *next = __real_heap_caps_realloc(ptr, size, caps);
    if (!next && size && managed_caps(caps)) {
        size_t old = ptr ? __real_heap_caps_get_allocated_size(ptr) : 0U;
        next = memory_external_alloc(size, false, false);
        if (next && ptr) { memcpy(next, ptr, size < old ? size : old); __real_heap_caps_free(ptr); }
    }
    return next;
}

/* Large unconstrained SDK objects share bulk storage. Known resident owners
 * use the resident end; small overflow also stays there. Ordinary small SDK
 * objects retain their capability-heap path. */
void *IRAM_ATTR __wrap_heap_caps_malloc_default(size_t size)
{
    if (size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && managed_caps(MALLOC_CAP_SPIRAM))
        return memory_external_alloc(size, false, false);
    void *pointer = __real_heap_caps_malloc_default(size);
    if (!pointer && size && managed_caps(MALLOC_CAP_SPIRAM))
        pointer = memory_external_alloc(size, false, false);
    return pointer;
}

void *IRAM_ATTR __wrap_heap_caps_realloc_default(void *ptr, size_t size)
{
    if (!ptr) return __wrap_heap_caps_malloc_default(size);
    if (!size) { solar_os_memory_free(ptr); return NULL; }
    if ((arena_mutex && (uintptr_t)ptr >= memory_arena.start &&
        (uintptr_t)ptr < memory_arena.end) ||
        (size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES && managed_caps(MALLOC_CAP_SPIRAM)))
        return __wrap_heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *next = __real_heap_caps_realloc_default(ptr, size);
    if (!next && managed_caps(MALLOC_CAP_SPIRAM)) {
        size_t old = __real_heap_caps_get_allocated_size(ptr);
        next = memory_external_alloc(size, false, false);
        if (next) { memcpy(next, ptr, size < old ? size : old); __real_heap_caps_free(ptr); }
    }
    return next;
}

size_t IRAM_ATTR __wrap_heap_caps_get_allocated_size(const void *ptr)
{
    if (arena_mutex && (uintptr_t)ptr >= memory_arena.start &&
        (uintptr_t)ptr < memory_arena.end) {
        arena_lock();
        size_t bytes = solar_os_memory_arena_size(&memory_arena, ptr);
        arena_unlock();
        return bytes;
    }
    return __real_heap_caps_get_allocated_size(ptr);
}

void *solar_os_memory_elf_alloc(size_t bytes)
{
    return solar_os_memory_alloc(bytes, SOLAR_OS_MEMORY_EXTERNAL_RESIDENT, "native.resident");
}

void *solar_os_memory_dl_alloc(size_t bytes)
{
    return solar_os_memory_alloc(bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.workspace");
}

size_t solar_os_memory_dl_free_bytes(bool external)
{
    solar_os_memory_status_t status;
    solar_os_memory_get_status(&status);
    return external ? status.external.free : status.internal.free;
}

static size_t memory_external_largest(void)
{
    solar_os_memory_status_t status;
    solar_os_memory_get_status(&status);
    return status.external.largest_free;
}

void solar_os_memory_set_reclaimer(size_t (*reclaim)(void))
{
    portENTER_CRITICAL(&memory_stats_lock);
    memory_reclaimer = reclaim;
    portEXIT_CRITICAL(&memory_stats_lock);
}

size_t solar_os_memory_reclaim(void)
{
    if (atomic_exchange(&reclaim_running, true)) return 0;
    portENTER_CRITICAL(&memory_stats_lock);
    size_t (*reclaim)(void) = memory_reclaimer;
    portEXIT_CRITICAL(&memory_stats_lock);
    const size_t released = reclaim ? reclaim() : 0U;
    atomic_store(&reclaim_running, false);
    return released;
}

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
    case SOLAR_OS_MEMORY_EXTERNAL_RESIDENT:
        return "external-resident";
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
            ptr = memory_external_alloc(requested, zero, false);
            fallback = ptr != NULL;
        }
        break;
    case SOLAR_OS_MEMORY_DMA:
        ptr = zero ? heap_caps_calloc(count, size, internal_caps | MALLOC_CAP_DMA) :
                     heap_caps_malloc(requested, internal_caps | MALLOC_CAP_DMA);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_REQUIRED:
        ptr = memory_external_alloc(requested, zero, false);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_RESIDENT:
        ptr = memory_external_alloc(requested, zero, true);
        break;
    case SOLAR_OS_MEMORY_EXTERNAL_PREFERRED:
    case SOLAR_OS_MEMORY_TRANSIENT: {
        const bool external_available = heap_caps_get_total_size(external_caps) > 0;
        if (external_available) {
            ptr = memory_external_alloc(requested, zero, false);
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
                  (unsigned)solar_os_memory_dl_free_bytes(true),
                  (unsigned)memory_external_largest());
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

    if (arena_mutex && (solar_os_memory_arena_contains(&memory_arena, ptr) ||
        size >= SOLAR_OS_PSRAM_BULK_MIN_BYTES ||
        memory_class == SOLAR_OS_MEMORY_EXTERNAL_RESIDENT)) {
        size_t before;
        if (solar_os_memory_arena_contains(&memory_arena, ptr)) {
            arena_lock();
            before = solar_os_memory_arena_size(&memory_arena, ptr);
            arena_unlock();
        } else {
            before = heap_caps_get_allocated_size(ptr);
        }
        void *replacement = solar_os_memory_alloc(size, memory_class, tag);
        if (replacement) {
            memcpy(replacement, ptr, before < size ? before : size);
            solar_os_memory_free(ptr);
        }
        return replacement;
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
    case SOLAR_OS_MEMORY_EXTERNAL_RESIDENT:
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
                  (unsigned)solar_os_memory_dl_free_bytes(true),
                  (unsigned)memory_external_largest());
    return NULL;
}

void IRAM_ATTR solar_os_memory_free(void *ptr)
{
    /* The ordinary-heap path stays cache-safe. Arena bounds are in internal
     * SRAM; inspecting/freeing arena metadata requires PSRAM to be accessible. */
    if (arena_mutex && (uintptr_t)ptr >= memory_arena.start &&
        (uintptr_t)ptr < memory_arena.end) {
        arena_lock();
        (void)solar_os_memory_arena_free(&memory_arena, ptr);
        arena_unlock();
        return;
    }
    __real_heap_caps_free(ptr);
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
    status->external_sdk = status->external;
    if (arena_mutex) {
        arena_lock();
        solar_os_memory_arena_status(&memory_arena, &status->arena);
        arena_unlock();
        status->external.free += status->arena.free;
        status->external.minimum_free += status->arena.minimum_free;
        if (status->arena.largest > status->external.largest_free)
            status->external.largest_free = status->arena.largest;
    }
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
