#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "solar_os_paged_array.h"
#include "doc_test_support.h"

int main(void)
{
    solar_os_paged_array_t records;
    assert(!solar_os_paged_array_init(&records, 0, "test"));
    assert(!solar_os_paged_array_init(&records, 8193, "test"));
    assert(solar_os_paged_array_init(&records, sizeof(uint32_t), "test"));
    doc_test_memory_largest(8192);
    assert(solar_os_paged_array_reserve(&records, 8000));
    for (size_t i = 0; i < 8000; ++i) *(uint32_t *)solar_os_paged_array_at(&records, i) = i;
    uint32_t *first = solar_os_paged_array_at(&records, 0);
    size_t capacity = records.capacity, used = doc_test_memory_used();
    assert(solar_os_paged_array_bytes(&records) == used);
    for (int failure = 0; failure < 4; ++failure) {
        doc_test_memory_fail_after(failure);
        assert(!solar_os_paged_array_reserve(&records, 16000));
        assert(records.capacity == capacity && doc_test_memory_used() == used);
        assert(solar_os_paged_array_at(&records, 0) == first);
        for (size_t i = 0; i < 8000; ++i) assert(*(uint32_t *)solar_os_paged_array_at(&records, i) == i);
    }
    doc_test_memory_fail_after(-1);
    assert(!solar_os_paged_array_reserve(&records, SIZE_MAX));
    assert(solar_os_paged_array_reserve(&records, 16000));
    assert(solar_os_paged_array_at(&records, 0) == first);
    assert(!solar_os_paged_array_at(&records, records.capacity));
    solar_os_paged_array_clear(&records);
    assert(solar_os_paged_array_bytes(&records) == 0);
    assert(!doc_test_memory_used());
    assert(solar_os_paged_array_reserve(&records, 1));
    solar_os_paged_array_clear(&records);
    assert(!doc_test_memory_used());
    puts("paged storage: fragmented limits, stable records and atomic OOM rollback passed");
}
