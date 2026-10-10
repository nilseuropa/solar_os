#pragma once
#include <stddef.h>
/* Enter/leave occur only in the serialized SolarOS worker. Allocations belong
 * to that request; exhaustion/cancellation unwinds to its native error guard. */
void *solar_os_imlib_malloc(size_t size);
void *solar_os_imlib_calloc(size_t count, size_t size);
void solar_os_imlib_free(void *ptr);
void solar_os_imlib_poll(void);
#define m_malloc solar_os_imlib_malloc
#define m_malloc0(size) solar_os_imlib_calloc(1, (size))
#define m_free solar_os_imlib_free
