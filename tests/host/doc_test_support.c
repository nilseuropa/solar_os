#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "doc_test_support.h"

typedef union {
    max_align_t alignment;
    size_t bytes;
} memory_header_t;

static size_t memory_used, memory_peak, memory_budget = SIZE_MAX;
static size_t memory_largest = SIZE_MAX;
static int memory_fail_after = -1;

void doc_test_memory_budget(size_t bytes) { memory_budget = bytes; }
void doc_test_memory_largest(size_t bytes) { memory_largest = bytes; }
void doc_test_memory_fail_after(int allocations) { memory_fail_after = allocations; }
size_t doc_test_memory_used(void) { return memory_used; }
size_t doc_test_memory_peak(void) { return memory_peak; }

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "host error";
}

void *solar_os_memory_alloc(size_t size,
                            solar_os_memory_class_t memory_class,
                            const char *tag)
{
    (void)memory_class;
    (void)tag;
    if (memory_fail_after == 0 || size > memory_largest || memory_used > memory_budget ||
        size > memory_budget - memory_used ||
        size > SIZE_MAX - sizeof(memory_header_t)) return NULL;
    memory_header_t *header = malloc(sizeof(*header) + size);
    if (!header) return NULL;
    if (memory_fail_after > 0) memory_fail_after--;
    header->bytes = size;
    memory_used += size;
    if (memory_used > memory_peak) memory_peak = memory_used;
    return header + 1;
}

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    (void)memory_class;
    (void)tag;
    if (size && count > SIZE_MAX / size) return NULL;
    void *pointer = solar_os_memory_alloc(count * size, memory_class, tag);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}

void *solar_os_memory_realloc(void *ptr,
                              size_t size,
                              solar_os_memory_class_t memory_class,
                              const char *tag)
{
    (void)memory_class;
    (void)tag;
    if (!ptr) return solar_os_memory_alloc(size, memory_class, tag);
    if (!size) { solar_os_memory_free(ptr); return NULL; }
    const memory_header_t *header = (const memory_header_t *)ptr - 1;
    const size_t before = header->bytes;
    void *next = solar_os_memory_alloc(size, memory_class, tag);
    if (next) {
        memcpy(next, ptr, before < size ? before : size);
        solar_os_memory_free(ptr);
    }
    return next;
}

void solar_os_memory_free(void *ptr)
{
    if (!ptr) return;
    memory_header_t *header = (memory_header_t *)ptr - 1;
    memory_used -= header->bytes;
    free(header);
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
    return ESP_OK;
}

