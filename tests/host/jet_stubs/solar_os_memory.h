#pragma once
#include <stddef.h>
typedef enum {
    SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
    SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
    SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
} solar_os_memory_class_t;
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t memory_class, const char *tag);
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t memory_class, const char *tag);
void solar_os_memory_free(void *storage);
