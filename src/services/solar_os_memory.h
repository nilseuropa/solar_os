#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os_resource_limits.h"
#include "solar_os_memory_arena.h"

#define SOLAR_OS_MEMORY_TAG_MAX 24U
#define SOLAR_OS_MEMORY_INTERNAL_RESERVE_BYTES SOLAR_OS_INTERNAL_RESERVE_BYTES
#define SOLAR_OS_MEMORY_INTERNAL_FALLBACK_MAX_BYTES (4U * 1024U)

typedef enum {
    SOLAR_OS_MEMORY_INTERNAL_CRITICAL = 0,
    SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
    SOLAR_OS_MEMORY_DMA,
    SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
    SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
    SOLAR_OS_MEMORY_TRANSIENT,
    SOLAR_OS_MEMORY_EXTERNAL_RESIDENT,
    SOLAR_OS_MEMORY_CLASS_COUNT,
} solar_os_memory_class_t;

typedef struct {
    size_t size;
    size_t alignment;
    uint32_t capabilities;
    solar_os_memory_class_t memory_class;
    const char *tag;
} solar_os_memory_pressure_request_t;

/* Descriptors must outlive registration. Callbacks run outside allocator locks,
 * must try-lock rather than wait for another allocating worker, and release
 * only data their owner can rebuild. They must not retain the request pointer.
 * Descriptor fields are immutable while registered. Lower priorities run first;
 * a satisfied allocation skips remaining callbacks. NULL requests reclaim all.
 * Callers must serialize buffer handle destruction against pin/unpin calls. */
typedef struct solar_os_memory_reclaimer {
    const char *owner;
    size_t (*reclaim)(const solar_os_memory_pressure_request_t *request, void *user);
    void *user;
    uint8_t priority;
    /* Private; initialize the descriptor to zero before first registration. */
    unsigned references;
} solar_os_memory_reclaimer_t;

typedef struct {
    uint32_t requests, callbacks, busy, compactions, low_stack;
    size_t released_bytes, moved_bytes;
} solar_os_memory_pressure_stats_t;

typedef struct solar_os_memory_buffer solar_os_memory_buffer_t;

/* A buffer is a contiguous lease while pinned, and movable while unpinned.
 * Only PSRAM byte buffers are movable. DMA, stacks, code and ordinary pointers
 * retain the pinned contract. No pointer may outlive its matching pin. */
solar_os_memory_buffer_t *solar_os_memory_buffer_create(size_t size, const char *owner);
void *solar_os_memory_buffer_pin(solar_os_memory_buffer_t *buffer);
bool solar_os_memory_buffer_unpin(solar_os_memory_buffer_t *buffer);
bool solar_os_memory_buffer_destroy(solar_os_memory_buffer_t *buffer);
size_t solar_os_memory_compact(void);
bool solar_os_memory_register_reclaimer(solar_os_memory_reclaimer_t *reclaimer);
/* False means a pressure pass still holds the descriptor; retain it and retry. */
bool solar_os_memory_unregister_reclaimer(solar_os_memory_reclaimer_t *reclaimer);
void solar_os_memory_pressure(const solar_os_memory_pressure_request_t *request);
void solar_os_memory_get_pressure_stats(solar_os_memory_pressure_stats_t *stats);

typedef struct {
    char owner[SOLAR_OS_MEMORY_TAG_MAX];
    size_t bytes, allocations, movable_bytes, largest;
} solar_os_memory_owner_status_t;
/* Managed-region owners; raw SDK allocations are grouped as sdk.pinned. */
size_t solar_os_memory_get_owners(solar_os_memory_owner_status_t *owners, size_t capacity);

typedef struct {
    size_t total;
    size_t free;
    size_t minimum_free;
    size_t largest_free;
} solar_os_memory_region_status_t;

typedef struct {
    uint32_t requests;
    uint32_t successes;
    uint32_t failures;
    uint32_t fallbacks;
    uint64_t requested_bytes;
} solar_os_memory_class_stats_t;

typedef struct {
    solar_os_memory_region_status_t internal;
    solar_os_memory_region_status_t external;
    solar_os_memory_region_status_t dma;
    solar_os_memory_class_stats_t classes[SOLAR_OS_MEMORY_CLASS_COUNT];
    size_t internal_reserve;
    size_t internal_fallback_max;
    bool last_failure_valid;
    solar_os_memory_class_t last_failure_class;
    size_t last_failure_size;
    char last_failure_tag[SOLAR_OS_MEMORY_TAG_MAX];
    solar_os_memory_arena_status_t arena;
    solar_os_memory_region_status_t external_sdk;
} solar_os_memory_status_t;

/* Reserve one region before peripheral/service allocations. Zero disables the
 * pool; the ordinary capability heaps remain available on every target. */
void solar_os_memory_init(void);
void solar_os_memory_set_reclaimer(size_t (*reclaim)(void));
size_t solar_os_memory_reclaim(void);
void *solar_os_memory_elf_alloc(size_t bytes);
void *solar_os_memory_dl_alloc(size_t bytes);
size_t solar_os_memory_dl_free_bytes(bool external);

void *solar_os_memory_alloc(size_t size,
                            solar_os_memory_class_t memory_class,
                            const char *tag);
void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag);
void *solar_os_memory_realloc(void *ptr,
                              size_t size,
                              solar_os_memory_class_t memory_class,
                              const char *tag);
void solar_os_memory_free(void *ptr);

void solar_os_memory_get_status(solar_os_memory_status_t *status);
const char *solar_os_memory_class_name(solar_os_memory_class_t memory_class);
bool solar_os_memory_is_external(const void *ptr);
