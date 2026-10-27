/* Share the validated, private C++ allocation/exception/TLS runtime. All symbols
 * are localized within this library; the ESP-DL module is not a dependency. */
#include "../espdl/port.c"
void *heap_caps_malloc(size_t bytes, uint32_t caps)
{ return heap_caps_aligned_alloc(16, bytes, caps); }
