/* Native service table fixtures. Allocations use each suite's failure-counted
 * memory stub; actual relocation is covered by the production allocator tests. */
#include <stdlib.h>
#include <string.h>
#include "solar_os_memory.h"

struct solar_os_memory_buffer { void *data; unsigned pins; };
solar_os_memory_buffer_t *solar_os_memory_buffer_create(size_t size, const char *owner)
{
    if (!size) return NULL;
    solar_os_memory_buffer_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->data = solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, owner);
    if (!b->data) { free(b); return NULL; }
    return b;
}
void *solar_os_memory_buffer_pin(solar_os_memory_buffer_t *b)
{ if (!b) return NULL; b->pins++; return b->data; }
bool solar_os_memory_buffer_unpin(solar_os_memory_buffer_t *b)
{ if (!b || !b->pins) return false; b->pins--; return true; }
bool solar_os_memory_buffer_destroy(solar_os_memory_buffer_t *b)
{
    if (!b) return true;
    if (b->pins) return false;
    solar_os_memory_free(b->data); free(b); return true;
}
__attribute__((weak)) bool solar_os_memory_is_external(const void *p) { (void)p; return false; }
__attribute__((weak)) void solar_os_memory_get_status(solar_os_memory_status_t *s)
{ memset(s, 0, sizeof(*s)); }
