#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_memory_arena.h"

static unsigned random_state = 1123;
static unsigned random_value(void)
{ random_state = random_state * 1664525U + 1013904223U; return random_state; }

int main(void)
{
    const size_t bytes = 8U * 1024U * 1024U;
    unsigned char *storage = malloc(bytes + 16);
    solar_os_memory_arena_t arena = {0};
    assert(solar_os_memory_arena_init(&arena, storage + 1, bytes));
    solar_os_memory_arena_status_t initial, status;
    solar_os_memory_arena_status(&arena, &initial);
    assert(initial.free == initial.largest && initial.free == arena.free);

    /* The temporary ELF buffer must not leave a hole below its resident code. */
    void *file = solar_os_memory_arena_alloc(&arena, 1137024, false);
    void *code = solar_os_memory_arena_alloc(&arena, 900000, true);
    void *data = solar_os_memory_arena_alloc(&arena, 100000, true);
    assert(file && code && data && (uintptr_t)code > (uintptr_t)file);
    memset(code, 0x71, 900000); memset(data, 0x72, 100000);
    assert(solar_os_memory_arena_free(&arena, file));
    solar_os_memory_arena_status(&arena, &status);
    assert(status.largest == status.free && status.resident == 1000000);
    void *weights = solar_os_memory_arena_alloc(&arena, 3612288, false);
    void *workspace = solar_os_memory_arena_alloc(&arena, 1706016, false);
    assert(weights && workspace);
    assert(((uintptr_t)workspace & 15) == 0 && ((uintptr_t)weights & 15) == 0);
    assert(((unsigned char *)code)[899999] == 0x71);
    assert(((unsigned char *)data)[99999] == 0x72);
    assert(!solar_os_memory_arena_free(&arena, (unsigned char *)weights + 1));
    assert(!solar_os_memory_arena_free(&arena, (unsigned char *)weights + 16));
    assert(!solar_os_memory_arena_free(&arena, storage + 1));
    assert(!solar_os_memory_arena_alloc(&arena, SIZE_MAX, false));
    assert(solar_os_memory_arena_free(&arena, workspace));
    assert(!solar_os_memory_arena_free(&arena, workspace));
    assert(solar_os_memory_arena_free(&arena, weights));
    assert(solar_os_memory_arena_free(&arena, code));
    assert(solar_os_memory_arena_free(&arena, data));
    solar_os_memory_arena_status(&arena, &status);
    assert(status.free == initial.free && status.largest == initial.free);

    /* JSON-sized allocations must not search thousands of live blocks. This
     * also exercises direct-header free in reverse and fragmented orders. */
    enum { OBJECTS = 20000 };
    void **objects = calloc(OBJECTS, sizeof(*objects));
    assert(objects);
    for (unsigned i = 0; i < OBJECTS; i++) {
        objects[i] = solar_os_memory_arena_alloc(&arena, 32, true);
        assert(objects[i]); memset(objects[i], i & 255U, 32);
    }
    solar_os_memory_arena_status(&arena, &status);
    assert(status.allocations == OBJECTS && status.resident == OBJECTS * 32U);
    for (unsigned i = 0; i < OBJECTS; i += 2)
        assert(solar_os_memory_arena_free(&arena, objects[i]));
    for (unsigned i = OBJECTS; i > 0; i -= 2) {
        unsigned slot = i - 1;
        assert(((unsigned char *)objects[slot])[31] == (slot & 255U));
        assert(solar_os_memory_arena_free(&arena, objects[slot]));
    }
    solar_os_memory_arena_status(&arena, &status);
    assert(!status.allocations && !status.resident);
    assert(status.free == initial.free && status.largest == initial.free);
    free(objects);

    /* Mixed lifetime allocation/free orders: retained pointers and contents
     * stay valid, live buffers never overlap, and teardown recovers the region. */
    void *pointers[64] = {0}; size_t sizes[64] = {0};
    for (unsigned iteration = 0; iteration < 20000; iteration++) {
        unsigned slot = random_value() % 64;
        if (pointers[slot]) {
            for (size_t i = 0; i < sizes[slot]; i++)
                assert(((unsigned char *)pointers[slot])[i] == (unsigned char)slot);
            assert(solar_os_memory_arena_free(&arena, pointers[slot]));
            pointers[slot] = NULL;
        } else {
            size_t size = 1U + random_value() % 32768;
            pointers[slot] = solar_os_memory_arena_alloc(&arena, size, random_value() % 2);
            assert(pointers[slot]); sizes[slot] = size;
            memset(pointers[slot], (unsigned char)slot, size);
            for (unsigned j = 0; j < 64; j++) {
                if (j == slot || !pointers[j]) continue;
                assert((uintptr_t)pointers[slot] + size <= (uintptr_t)pointers[j] ||
                       (uintptr_t)pointers[j] + sizes[j] <= (uintptr_t)pointers[slot]);
            }
        }
        solar_os_memory_arena_status(&arena, &status);
        assert(status.free == arena.free && status.largest <= status.free);
    }
    for (unsigned j = 0; j < 64; j++) if (pointers[j])
        assert(solar_os_memory_arena_free(&arena, pointers[j]));
    solar_os_memory_arena_status(&arena, &status);
    assert(status.free == initial.free && status.largest == initial.free);
    free(storage);
    puts("arena lifetimes, ELF staging, large workspace, pointer stability and coalescing passed");
}
