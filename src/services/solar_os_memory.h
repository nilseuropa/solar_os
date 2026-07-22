#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_MEMORY_TAG_MAX 24U
#define SOLAR_OS_MEMORY_INTERNAL_RESERVE_BYTES (32U * 1024U)
#define SOLAR_OS_MEMORY_INTERNAL_FALLBACK_MAX_BYTES (4U * 1024U)

typedef enum {
    SOLAR_OS_MEMORY_INTERNAL_CRITICAL = 0,
    SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
    SOLAR_OS_MEMORY_DMA,
    SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
    SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
    SOLAR_OS_MEMORY_TRANSIENT,
    SOLAR_OS_MEMORY_CLASS_COUNT,
} solar_os_memory_class_t;

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
} solar_os_memory_status_t;

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
