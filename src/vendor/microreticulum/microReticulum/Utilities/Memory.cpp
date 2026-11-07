#include "Memory.h"

#include "../Log.h"

#include <esp_heap_caps.h>

using namespace RNS::Utilities;

/*static*/ size_t Memory::heap_size() {
	return heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

/*static*/ size_t Memory::heap_available() {
	return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

/*static*/ void Memory::dump_heap_stats() {
	INFOF("Free internal heap: %lu of %lu bytes", (unsigned long)heap_available(), (unsigned long)heap_size());
}
