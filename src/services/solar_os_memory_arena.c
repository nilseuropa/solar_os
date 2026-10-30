#include "solar_os_memory_arena.h"

#include <string.h>

#define ARENA_ALIGNMENT 16U
struct solar_os_memory_arena_block {
    solar_os_memory_arena_block_t *previous, *next;
    solar_os_memory_arena_block_t *free_previous, *free_next;
    size_t span, requested;
    uintptr_t cookie;
    bool resident;
    bool movable;
    uint16_t owner;
};
#define ARENA_HEADER ((sizeof(solar_os_memory_arena_block_t) + 15U) & ~(size_t)15U)

static uintptr_t block_cookie(const solar_os_memory_arena_t *a,
                              const solar_os_memory_arena_block_t *b)
{ return (uintptr_t)b ^ a->start ^ (uintptr_t)0xa57e19b3U; }

static void free_remove(solar_os_memory_arena_t *a, solar_os_memory_arena_block_t *b)
{
    if (b->free_previous) b->free_previous->free_next = b->free_next;
    else a->free_blocks = b->free_next;
    if (b->free_next) b->free_next->free_previous = b->free_previous;
    b->free_previous = b->free_next = NULL;
}

static void free_insert(solar_os_memory_arena_t *a, solar_os_memory_arena_block_t *b)
{
    b->free_previous = NULL;
    b->free_next = a->free_blocks;
    if (b->free_next) b->free_next->free_previous = b;
    a->free_blocks = b;
}

bool solar_os_memory_arena_init(solar_os_memory_arena_t *a, void *buffer, size_t size)
{
    if (!a || !buffer || size < ARENA_HEADER + ARENA_ALIGNMENT ||
        (uintptr_t)buffer > UINTPTR_MAX - size) return false;
    uintptr_t start = ((uintptr_t)buffer + 15U) & ~(uintptr_t)15U;
    uintptr_t end = ((uintptr_t)buffer + size) & ~(uintptr_t)15U;
    if (start >= end || end - start < ARENA_HEADER + ARENA_ALIGNMENT) return false;
    *a = (solar_os_memory_arena_t) {
        .start = start, .end = end, .total = end - start,
        .free = end - start - ARENA_HEADER,
        .minimum_free = end - start - ARENA_HEADER,
    };
    a->first = a->last = (solar_os_memory_arena_block_t *)start;
    *a->first = (solar_os_memory_arena_block_t){.span = end - start};
    free_insert(a, a->first);
    return true;
}

void solar_os_memory_arena_status(const solar_os_memory_arena_t *a,
                                   solar_os_memory_arena_status_t *s)
{
    *s = (solar_os_memory_arena_status_t){.total = a->total, .free = a->free,
        .minimum_free = a->minimum_free, .allocations = a->allocations,
        .resident = a->resident};
    for (const solar_os_memory_arena_block_t *b = a->free_blocks; b; b = b->free_next) {
        size_t available = b->span - ARENA_HEADER;
        if (available > s->largest) s->largest = available;
    }
}

void *solar_os_memory_arena_alloc(solar_os_memory_arena_t *a, size_t size, bool resident)
{
    if (!a || !size || size > SIZE_MAX - ARENA_HEADER - 15U) return NULL;
    const size_t needed = ARENA_HEADER + ((size + 15U) & ~(size_t)15U);
    /* Search only holes, choosing the appropriate physical end. Live JSON
     * objects and retained app buffers never participate in this search. */
    solar_os_memory_arena_block_t *b = NULL;
    for (solar_os_memory_arena_block_t *hole = a->free_blocks; hole; hole = hole->free_next) {
        if (hole->span >= needed && (!b || (resident ?
            (uintptr_t)hole > (uintptr_t)b : (uintptr_t)hole < (uintptr_t)b))) b = hole;
    }
    if (b) {
        free_remove(a, b);
        const size_t remainder = b->span - needed;
        if (remainder >= ARENA_HEADER + ARENA_ALIGNMENT) {
            a->free -= needed;
            solar_os_memory_arena_block_t *n = (void *)((uintptr_t)b +
                (resident ? remainder : needed));
            *n = (solar_os_memory_arena_block_t) {
                .previous = b, .next = b->next,
                .span = resident ? needed : remainder,
            };
            if (n->next) n->next->previous = n;
            else a->last = n;
            b->next = n;
            b->span = resident ? remainder : needed;
            free_insert(a, resident ? b : n);
            if (resident) b = n;
        } else {
            a->free -= b->span - ARENA_HEADER;
        }
        b->requested = size;
        b->resident = resident;
        b->movable = false;
        b->owner = 0;
        b->cookie = block_cookie(a, b);
        a->allocations++;
        if (resident) a->resident += size;
        if (a->free < a->minimum_free) a->minimum_free = a->free;
        return (void *)((uintptr_t)b + ARENA_HEADER);
    }
    return NULL;
}

bool solar_os_memory_arena_contains(const solar_os_memory_arena_t *a, const void *p)
{
    return p && (uintptr_t)p >= a->start && (uintptr_t)p < a->end;
}

static solar_os_memory_arena_block_t *find(const solar_os_memory_arena_t *a, const void *p)
{
    uintptr_t address = (uintptr_t)p;
    if (!solar_os_memory_arena_contains(a, p) || (address & 15U) ||
        address < a->start + ARENA_HEADER) return NULL;
    solar_os_memory_arena_block_t *b = (void *)(address - ARENA_HEADER);
    if (b->cookie != block_cookie(a, b) || !b->requested ||
        b->span < ARENA_HEADER || b->span > a->end - (uintptr_t)b ||
        b->requested > b->span - ARENA_HEADER) return NULL;
    return b;
}

size_t solar_os_memory_arena_size(const solar_os_memory_arena_t *a, const void *p)
{
    solar_os_memory_arena_block_t *b = find(a, p);
    return b ? b->requested : 0;
}

static void merge(solar_os_memory_arena_t *a, solar_os_memory_arena_block_t *b)
{
    solar_os_memory_arena_block_t *n = b->next;
    b->span += n->span;
    a->free += ARENA_HEADER;
    b->next = n->next;
    if (b->next) b->next->previous = b;
    else a->last = b;
    n->cookie = 0;
}

bool solar_os_memory_arena_resize(solar_os_memory_arena_t *a, void *p, size_t size)
{
    if (!a || !size || size > SIZE_MAX - ARENA_HEADER - 15U) return false;
    solar_os_memory_arena_block_t *b = find(a, p);
    if (!b) return false;
    const size_t needed = ARENA_HEADER + ((size + 15U) & ~(size_t)15U);
    if (needed > b->span) {
        solar_os_memory_arena_block_t *next = b->next;
        if (!next || next->requested || next->span < needed - b->span)
            return false;
        const size_t span = next->span;
        free_remove(a, next);
        /* merge() also works with a live left block; its cookie stays valid. */
        merge(a, b);
        a->free -= span;
    }
    const size_t remainder = b->span - needed;
    if (remainder >= ARENA_HEADER + ARENA_ALIGNMENT) {
        solar_os_memory_arena_block_t *tail = (void *)((uintptr_t)b + needed);
        *tail = (solar_os_memory_arena_block_t){
            .previous = b, .next = b->next, .span = remainder,
        };
        if (tail->next) tail->next->previous = tail;
        else a->last = tail;
        b->next = tail;
        b->span = needed;
        a->free += remainder - ARENA_HEADER;
        if (tail->next && !tail->next->requested) {
            free_remove(a, tail->next);
            merge(a, tail);
        }
        free_insert(a, tail);
    }
    if (b->resident) a->resident = a->resident - b->requested + size;
    b->requested = size;
    if (a->free < a->minimum_free) a->minimum_free = a->free;
    return true;
}

bool solar_os_memory_arena_free(solar_os_memory_arena_t *a, void *p)
{
    solar_os_memory_arena_block_t *b = find(a, p);
    if (!b) return false;
    a->free += b->span - ARENA_HEADER;
    a->allocations--;
    if (b->resident) a->resident -= b->requested;
    b->requested = 0;
    b->cookie = 0;
    b->resident = false;
    b->movable = false;
    b->owner = 0;
    if (b->next && !b->next->requested) {
        free_remove(a, b->next); merge(a, b);
    }
    if (b->previous && !b->previous->requested) {
        solar_os_memory_arena_block_t *previous = b->previous;
        free_remove(a, previous); merge(a, previous); b = previous;
    }
    free_insert(a, b);
    return true;
}

bool solar_os_memory_arena_describe(solar_os_memory_arena_t *a, void *p,
                                    uint16_t owner, bool movable)
{
    solar_os_memory_arena_block_t *b = find(a, p);
    if (!b) return false;
    b->owner = owner;
    b->movable = movable;
    return true;
}

size_t solar_os_memory_arena_compact(solar_os_memory_arena_t *a,
    solar_os_memory_arena_relocated_fn relocated, void *user)
{
    if (!a || !a->first || !relocated) return 0;
    size_t moved = 0;
    /* Slide only explicitly movable blocks into the preceding hole. memmove
     * supports a hole smaller than the object, without a second full buffer.
     * Rebuild the free list because overlapping moves invalidate its links. */
    solar_os_memory_arena_block_t *b = a->first;
    while (b && b->next) {
        solar_os_memory_arena_block_t *next = b->next;
        if (!b->requested && next->requested && next->movable) {
            const size_t hole_span = b->span, live_span = next->span;
            const size_t requested = next->requested;
            solar_os_memory_arena_block_t *previous = b->previous;
            solar_os_memory_arena_block_t *after = next->next;
            void *old_pointer = (void *)((uintptr_t)next + ARENA_HEADER);
            memmove(b, next, live_span);
            solar_os_memory_arena_block_t *hole = (void *)((uintptr_t)b + live_span);
            *hole = (solar_os_memory_arena_block_t){
                .previous = b, .next = after, .span = hole_span,
            };
            b->previous = previous;
            b->next = hole;
            b->cookie = block_cookie(a, b);
            if (previous) previous->next = b;
            if (after) after->previous = hole;
            else a->last = hole;
            relocated(user, old_pointer, (void *)((uintptr_t)b + ARENA_HEADER));
            moved += requested;
            b = hole;
        } else if (!b->requested && !next->requested) {
            merge(a, b);
        } else {
            b = next;
        }
    }
    a->free_blocks = NULL;
    for (b = a->first; b; b = b->next) {
        b->free_previous = b->free_next = NULL;
        if (!b->requested) free_insert(a, b);
    }
    return moved;
}

void solar_os_memory_arena_visit(const solar_os_memory_arena_t *a,
    solar_os_memory_arena_visit_fn visit, void *user)
{
    if (!a || !visit) return;
    for (const solar_os_memory_arena_block_t *b = a->first; b; b = b->next) {
        if (b->requested) visit(user, (const void *)((uintptr_t)b + ARENA_HEADER),
                               b->requested, b->owner, b->movable);
    }
}
