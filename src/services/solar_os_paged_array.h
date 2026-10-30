#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Pinned pages retain ordinary C record addresses for their owner's lifetime.
 * Indexing never requires a single contiguous allocation for the whole array.
 * Callers serialize mutation; pointers survive reserve(), but not clear(). */
typedef struct {
    void **pages;
    size_t element_size, elements_per_page, page_count, capacity;
    const char *owner; /* Must outlive the array, normally a string literal. */
} solar_os_paged_array_t;

bool solar_os_paged_array_init(solar_os_paged_array_t *array, size_t element_size,
                               const char *owner);
bool solar_os_paged_array_reserve(solar_os_paged_array_t *array, size_t count);
void *solar_os_paged_array_at(const solar_os_paged_array_t *array, size_t index);
/* Requested page and directory bytes, excluding allocator overhead. */
size_t solar_os_paged_array_bytes(const solar_os_paged_array_t *array);
void solar_os_paged_array_clear(solar_os_paged_array_t *array);
