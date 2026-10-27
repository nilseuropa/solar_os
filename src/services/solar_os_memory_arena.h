#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Non-moving allocator. The caller serializes all operations. Low-end
 * allocations hold bulk workloads; high-end allocations hold resident data.
 * Metadata lives inside the supplied region, with no secondary allocation. */
typedef struct solar_os_memory_arena_block solar_os_memory_arena_block_t;
typedef struct {
    solar_os_memory_arena_block_t *first, *last;
    solar_os_memory_arena_block_t *free_blocks;
    uintptr_t start, end;
    size_t total, free, minimum_free, allocations, resident;
} solar_os_memory_arena_t;

typedef struct {
    size_t total, free, largest, minimum_free, allocations, resident;
} solar_os_memory_arena_status_t;

bool solar_os_memory_arena_init(solar_os_memory_arena_t *arena, void *buffer, size_t size);
void *solar_os_memory_arena_alloc(solar_os_memory_arena_t *arena, size_t size, bool resident);
bool solar_os_memory_arena_contains(const solar_os_memory_arena_t *arena, const void *pointer);
size_t solar_os_memory_arena_size(const solar_os_memory_arena_t *arena, const void *pointer);
bool solar_os_memory_arena_free(solar_os_memory_arena_t *arena, void *pointer);
void solar_os_memory_arena_status(const solar_os_memory_arena_t *arena,
                                   solar_os_memory_arena_status_t *status);
