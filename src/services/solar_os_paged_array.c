#include "solar_os_paged_array.h"

#include <stdint.h>
#include <string.h>
#include "solar_os_memory.h"

#define PAGE_BYTES (8U * 1024U)

bool solar_os_paged_array_init(solar_os_paged_array_t *array, size_t element_size,
                               const char *owner)
{
    if (!array || !element_size || element_size > PAGE_BYTES) return false;
    *array = (solar_os_paged_array_t){
        .element_size = element_size,
        .elements_per_page = PAGE_BYTES / element_size,
        .owner = owner,
    };
    return true;
}

bool solar_os_paged_array_reserve(solar_os_paged_array_t *array, size_t count)
{
    if (!array || !array->elements_per_page) return false;
    if (count <= array->capacity) return true;
    const size_t per_page = array->elements_per_page;
    size_t pages = count / per_page + (count % per_page != 0U);
    if (pages > SIZE_MAX / sizeof(void *) || pages > SIZE_MAX / per_page) return false;
    void **directory = solar_os_memory_alloc(pages * sizeof(void *),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "memory.pages");
    if (!directory) return false;
    if (array->page_count) memcpy(directory, array->pages, array->page_count * sizeof(void *));
    size_t built = array->page_count;
    for (; built < pages; ++built) {
        directory[built] = solar_os_memory_calloc(per_page, array->element_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, array->owner);
        if (!directory[built]) break;
    }
    if (built != pages) {
        while (built > array->page_count) solar_os_memory_free(directory[--built]);
        solar_os_memory_free(directory);
        return false;
    }
    solar_os_memory_free(array->pages);
    array->pages = directory;
    array->page_count = pages;
    array->capacity = pages * per_page;
    return true;
}

void *solar_os_paged_array_at(const solar_os_paged_array_t *array, size_t index)
{
    if (!array || !array->elements_per_page || index >= array->capacity) return NULL;
    return (uint8_t *)array->pages[index / array->elements_per_page] +
        (index % array->elements_per_page) * array->element_size;
}

size_t solar_os_paged_array_bytes(const solar_os_paged_array_t *array)
{
    if (!array || !array->page_count) return 0;
    const size_t page_bytes = array->elements_per_page * array->element_size + sizeof(void *);
    return array->page_count > SIZE_MAX / page_bytes ? SIZE_MAX : array->page_count * page_bytes;
}

void solar_os_paged_array_clear(solar_os_paged_array_t *array)
{
    if (!array) return;
    for (size_t i = 0; i < array->page_count; ++i) solar_os_memory_free(array->pages[i]);
    solar_os_memory_free(array->pages);
    array->pages = NULL;
    array->page_count = array->capacity = 0;
}
