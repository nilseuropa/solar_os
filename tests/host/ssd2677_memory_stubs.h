#pragma once
#include <stdlib.h>
#include "solar_os_memory.h"

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; return malloc(bytes); }
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; return calloc(count, bytes); }
void solar_os_memory_free(void *ptr) { free(ptr); }
