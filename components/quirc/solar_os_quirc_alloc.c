#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"

/* QR buffers must not consume the internal heap on allocation failure. */
void *solar_os_quirc_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void *solar_os_quirc_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = solar_os_quirc_malloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
void solar_os_quirc_free(void *memory)
{
    heap_caps_free(memory);
}
