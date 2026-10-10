#include "solar_os_memory.h"
#include <stdlib.h>
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls; (void)tag; return malloc(size);
}
