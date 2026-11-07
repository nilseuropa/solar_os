#pragma once
#include <stdlib.h>
#include <string.h>
#include "solar_os_memory.h"

static int memory_fail_after = -1;
static unsigned memory_allocations;

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind < SOLAR_OS_MEMORY_CLASS_COUNT && tag && strlen(tag) < SOLAR_OS_MEMORY_TAG_MAX);
    if (!memory_fail_after) return NULL;
    void *pointer = malloc(bytes);
    if (pointer) {
        memory_allocations++;
        if (memory_fail_after > 0) memory_fail_after--;
    }
    return pointer;
}
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t kind, const char *tag)
{
    if (bytes && count > SIZE_MAX / bytes) return NULL;
    void *pointer = solar_os_memory_alloc(count * bytes, kind, tag);
    if (pointer) memset(pointer, 0, count * bytes);
    return pointer;
}
void solar_os_memory_free(void *pointer)
{
    if (pointer) { assert(memory_allocations > 0); memory_allocations--; free(pointer); }
}
