#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_memory.h"
#include <esp_heap_caps.h>

typedef struct raw {
    struct raw *next;
    void *pointer;
    size_t size;
    uint32_t caps;
} raw_t;
static raw_t *raws;
static size_t used_external, used_internal;
static void *idle_library;
static unsigned reclaim_calls;
static bool fail_raw;
int test_isr;
#define EXTERNAL_BYTES (8U * 1024U * 1024U)
#define INTERNAL_BYTES (256U * 1024U)

size_t heap_caps_get_total_size(uint32_t caps)
{ return caps & MALLOC_CAP_SPIRAM ? EXTERNAL_BYTES : INTERNAL_BYTES; }
size_t heap_caps_get_free_size(uint32_t caps)
{ return heap_caps_get_total_size(caps) - (caps & MALLOC_CAP_SPIRAM ? used_external : used_internal); }
size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps); }
void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps)
{
    if (fail_raw || size > heap_caps_get_free_size(caps)) return NULL;
    size_t padded = (size + align - 1U) & ~(align - 1U);
    raw_t *r = malloc(sizeof(*r));
    assert(r);
    r->pointer = aligned_alloc(align, padded); assert(r->pointer);
    r->size = size; r->caps = caps; r->next = raws; raws = r;
    if (caps & MALLOC_CAP_SPIRAM) used_external += size;
    else used_internal += size;
    return r->pointer;
}
void *heap_caps_malloc(size_t size, uint32_t caps) { return heap_caps_aligned_alloc(16, size, caps); }
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{ void *p = heap_caps_malloc(count * size, caps); if (p) memset(p, 0, count * size); return p; }
size_t heap_caps_get_allocated_size(const void *p)
{ for (raw_t *r = raws; r; r = r->next) if (r->pointer == p) return r->size; abort(); }
void __real_heap_caps_free(void *p)
{
    if (!p) return;
    raw_t **link = &raws;
    while (*link && (*link)->pointer != p) link = &(*link)->next;
    assert(*link); /* A pooled pointer must never reach the SDK allocator. */
    raw_t *r = *link; *link = r->next;
    if (r->caps & MALLOC_CAP_SPIRAM) used_external -= r->size;
    else used_internal -= r->size;
    free(r->pointer); free(r);
}
void heap_caps_free(void *p) { solar_os_memory_free(p); }
void *heap_caps_realloc(void *p, size_t size, uint32_t caps)
{
    size_t old = heap_caps_get_allocated_size(p);
    void *next = heap_caps_malloc(size, caps);
    if (next) { memcpy(next, p, old < size ? old : size); __real_heap_caps_free(p); }
    return next;
}
void *__real_heap_caps_malloc(size_t n, uint32_t c) { return heap_caps_malloc(n, c); }
void *__real_heap_caps_calloc(size_t n, size_t s, uint32_t c) { return heap_caps_calloc(n, s, c); }
void *__real_heap_caps_aligned_alloc(size_t a, size_t n, uint32_t c) { return heap_caps_aligned_alloc(a, n, c); }
void *__real_heap_caps_aligned_calloc(size_t a, size_t n, size_t s, uint32_t c)
{ void *p = heap_caps_aligned_alloc(a, n * s, c); if (p) memset(p, 0, n * s); return p; }
void *__real_heap_caps_realloc(void *p, size_t n, uint32_t c) { return heap_caps_realloc(p, n, c); }
void *__real_heap_caps_malloc_default(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *__real_heap_caps_realloc_default(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
size_t __real_heap_caps_get_allocated_size(const void *p) { return heap_caps_get_allocated_size(p); }
void *__wrap_heap_caps_malloc_default(size_t);
void *__wrap_heap_caps_realloc_default(void *, size_t);
size_t __wrap_heap_caps_get_allocated_size(const void *);
void *__wrap_heap_caps_malloc(size_t, uint32_t);
void *__wrap_heap_caps_calloc(size_t, size_t, uint32_t);
void *__wrap_heap_caps_aligned_alloc(size_t, size_t, uint32_t);
void *__wrap_heap_caps_aligned_calloc(size_t, size_t, size_t, uint32_t);
void *__wrap_heap_caps_realloc(void *, size_t, uint32_t);

bool esp_ptr_external_ram(const void *p)
{
    for (raw_t *r = raws; r; r = r->next)
        if ((r->caps & MALLOC_CAP_SPIRAM) && (uintptr_t)p >= (uintptr_t)r->pointer &&
            (uintptr_t)p < (uintptr_t)r->pointer + r->size) return true;
    return false;
}
size_t strlcpy(char *d, const char *s, size_t n)
{ size_t len = strlen(s); if (n) { size_t count = len < n - 1 ? len : n - 1; memcpy(d, s, count); d[count] = 0; } return len; }
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void __wrap_free(void *);
void __wrap_heap_caps_free(void *);

static size_t reclaim(void)
{
    reclaim_calls++;
    assert(solar_os_memory_reclaim() == 0); /* Nested pressure cannot recurse. */
    if (!idle_library) return 0;
    solar_os_memory_free(idle_library); idle_library = NULL;
    return 1;
}

int main(void)
{
    solar_os_memory_status_t before, after;
    solar_os_memory_init(); solar_os_memory_init();
    solar_os_memory_get_status(&before);
    assert(before.arena.total > 6U * 1024U * 1024U);
    assert(before.external.free > 7U * 1024U * 1024U);
    void *small = solar_os_memory_alloc(100, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "small");
    void *bulk = solar_os_memory_calloc(1, 100000, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "bulk");
    void *resident = solar_os_memory_elf_alloc(20000);
    assert(small && bulk && resident && solar_os_memory_is_external(bulk));
    solar_os_memory_get_status(&after);
    assert(after.arena.allocations == 2 && after.arena.resident == 20000);
    assert(after.external_sdk.free == before.external_sdk.free - 100);
    for (unsigned i = 0; i < 100000; i++) assert(((unsigned char *)bulk)[i] == 0);
    memset(small, 0x12, 100); memset(bulk, 0x31, 100000);
    void *grown = solar_os_memory_realloc(small, 200000, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "grow");
    assert(grown);
    for (unsigned i = 0; i < 100; i++) assert(((unsigned char *)grown)[i] == 0x12);
    assert(!solar_os_memory_realloc(bulk, EXTERNAL_BYTES * 2U, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "failure"));
    for (unsigned i = 0; i < 100000; i++) assert(((unsigned char *)bulk)[i] == 0x31);
    void *shrunk = solar_os_memory_realloc(grown, 50, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "shrink");
    assert(shrunk && ((unsigned char *)shrunk)[49] == 0x12);
    __wrap_free(bulk); __wrap_heap_caps_free(resident); solar_os_memory_free(shrunk);
    solar_os_memory_get_status(&after);
    assert(after.arena.free == before.arena.free && after.arena.largest == before.arena.largest);

    /* SDK users share the pool, but capability/alignment/ISR contracts survive. */
    uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    bulk = __wrap_heap_caps_calloc(2, 50000, external_caps);
    assert(bulk && ((unsigned char *)bulk)[99999] == 0);
    memset(bulk, 0x62, 100000);
    grown = __wrap_heap_caps_realloc(bulk, 150000, external_caps);
    assert(grown && ((unsigned char *)grown)[99999] == 0x62);
    small = __wrap_heap_caps_realloc(grown, 100, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(small && !solar_os_memory_is_external(small) && ((unsigned char *)small)[99] == 0x62);
    __wrap_heap_caps_free(small);
    small = heap_caps_malloc(32, external_caps);
    memset(small, 0x74, 32);
    bulk = __wrap_heap_caps_realloc(small, 100000, external_caps);
    assert(bulk && ((unsigned char *)bulk)[31] == 0x74);
    __wrap_free(bulk);
    bulk = __wrap_heap_caps_aligned_calloc(16, 2, 50000, external_caps);
    assert(bulk && ((uintptr_t)bulk & 15U) == 0 && ((unsigned char *)bulk)[99999] == 0);
    __wrap_free(bulk);
    assert(!__wrap_heap_caps_calloc(SIZE_MAX, 2, external_caps));
    uint32_t special_caps[] = {MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA,
        external_caps, external_caps | MALLOC_CAP_DMA};
    for (unsigned i = 0; i < 3; i++) {
        void *special = i == 1 ? __wrap_heap_caps_aligned_alloc(64, 100000, special_caps[i]) :
            __wrap_heap_caps_malloc(100000, special_caps[i]);
        solar_os_memory_get_status(&after);
        assert(special && after.arena.allocations == 0);
        __wrap_free(special);
    }
    test_isr = 1;
    void *interrupt = __wrap_heap_caps_malloc(100000, external_caps);
    test_isr = 0;
    assert(interrupt); __wrap_free(interrupt);
    solar_os_memory_get_status(&after);
    assert(after.arena.free == before.arena.free);

    /* SDK capacity remains usable when a request cannot fit the managed gap. */
    solar_os_memory_get_status(&after);
    const size_t sdk_bytes = SOLAR_OS_PSRAM_GENERAL_RESERVE_BYTES / 2U;
    void *fill = solar_os_memory_alloc(after.arena.largest - 32768,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "fill");
    void *sdk_bulk = solar_os_memory_alloc(sdk_bytes,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "sdk-bulk");
    assert(fill && sdk_bulk && __wrap_heap_caps_get_allocated_size(sdk_bulk) == sdk_bytes);
    memset(sdk_bulk, 0x35, sdk_bytes);
    assert(((unsigned char *)sdk_bulk)[sdk_bytes - 1] == 0x35);
    solar_os_memory_free(sdk_bulk); solar_os_memory_free(fill);
    solar_os_memory_get_status(&after);
    assert(after.arena.free == before.arena.free && after.external.free == before.external.free);

    bulk = __wrap_heap_caps_malloc_default(100000);
    assert(bulk && __wrap_heap_caps_get_allocated_size(bulk) == 100000);
    memset(bulk, 0x2e, 100000);
    grown = __wrap_heap_caps_realloc_default(bulk, 110000);
    assert(grown && ((unsigned char *)grown)[99999] == 0x2e);
    assert(!__wrap_heap_caps_realloc_default(grown, EXTERNAL_BYTES * 2U));
    assert(((unsigned char *)grown)[99999] == 0x2e);
    assert(!__wrap_heap_caps_realloc_default(grown, 0));
    small = heap_caps_malloc(64, external_caps);
    assert(small); memset(small, 0x47, 64);
    fail_raw = true;
    grown = __wrap_heap_caps_realloc(small, 128, external_caps);
    assert(grown && ((unsigned char *)grown)[63] == 0x47);
    __wrap_free(grown);
    void *sdk_small[] = {
        __wrap_heap_caps_malloc(128, external_caps),
        __wrap_heap_caps_calloc(16, 8, external_caps),
        __wrap_heap_caps_aligned_alloc(16, 128, external_caps),
        __wrap_heap_caps_aligned_calloc(16, 16, 8, external_caps),
    };
    for (unsigned i = 0; i < 4; i++) {
        assert(sdk_small[i] && ((uintptr_t)sdk_small[i] & 15U) == 0);
        if (i == 1 || i == 3) assert(((unsigned char *)sdk_small[i])[127] == 0);
        __wrap_free(sdk_small[i]);
    }
    assert(!__wrap_heap_caps_aligned_alloc(64, 128, external_caps));
    small = __wrap_heap_caps_malloc_default(100);
    assert(small && __wrap_heap_caps_get_allocated_size(small) == 100);
    small = __wrap_heap_caps_realloc_default(small, 200);
    assert(small); __wrap_free(small);
    fail_raw = false;

    /* Large pressure releases an idle resident image and retries once. */
    idle_library = solar_os_memory_elf_alloc(1024U * 1024U);
    solar_os_memory_set_reclaimer(reclaim);
    solar_os_memory_get_status(&after);
    void *large = solar_os_memory_alloc(after.arena.largest + 100000,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "pressure");
    assert(large && !idle_library && reclaim_calls == 1);
    solar_os_memory_free(large);
    fail_raw = true;
    small = solar_os_memory_alloc(32, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "small-fallback");
    assert(small && ((uintptr_t)small & 15U) == 0);
    solar_os_memory_free(small);
    solar_os_memory_get_status(&after);
    assert(after.arena.free == before.arena.free && after.arena.largest == before.arena.largest);
    assert(after.external.free == before.external.free);
    /* The boot reservation intentionally remains allocated until reset. */
    assert(raws && !raws->next);
    __real_heap_caps_free(raws->pointer);
    puts("policy routing, alignment, calloc, realloc ownership, free hooks and pressure retry passed");
}
