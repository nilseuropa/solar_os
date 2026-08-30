#pragma once

#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM UINT32_C(1)
#define MALLOC_CAP_8BIT UINT32_C(2)

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}
