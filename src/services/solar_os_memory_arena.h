#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pinned-by-default allocator with explicit movable objects. The caller serializes all operations. Low-end
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

typedef void (*solar_os_memory_arena_relocated_fn)(void *user, void *old_pointer,
                                                 void *new_pointer);
typedef void (*solar_os_memory_arena_visit_fn)(void *user, const void *pointer,
    size_t size, uint16_t owner, bool movable);

bool solar_os_memory_arena_init(solar_os_memory_arena_t *arena, void *buffer, size_t size);
void *solar_os_memory_arena_alloc(solar_os_memory_arena_t *arena, size_t size, bool resident);
bool solar_os_memory_arena_contains(const solar_os_memory_arena_t *arena, const void *pointer);
size_t solar_os_memory_arena_size(const solar_os_memory_arena_t *arena, const void *pointer);
/* Resize without moving. Failure leaves the allocation and contents intact. */
bool solar_os_memory_arena_resize(solar_os_memory_arena_t *arena, void *pointer, size_t size);
bool solar_os_memory_arena_free(solar_os_memory_arena_t *arena, void *pointer);
void solar_os_memory_arena_status(const solar_os_memory_arena_t *arena,
                                   solar_os_memory_arena_status_t *status);
/* Ordinary allocations are pinned. Only a handle owner can permit movement;
 * the caller holds its arena/handle lock throughout compaction and relocation. */
bool solar_os_memory_arena_describe(solar_os_memory_arena_t *arena, void *pointer,
                                    uint16_t owner, bool movable);
size_t solar_os_memory_arena_compact(solar_os_memory_arena_t *arena,
    solar_os_memory_arena_relocated_fn relocated, void *user);
void solar_os_memory_arena_visit(const solar_os_memory_arena_t *arena,
    solar_os_memory_arena_visit_fn visit, void *user);
