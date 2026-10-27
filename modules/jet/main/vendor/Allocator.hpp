#pragma once

#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#if !defined(JET_EXTERNAL_LIBRARY) && (defined(ESP_PLATFORM) || defined(JET_SOLAROS_ALLOCATOR))
extern "C" {
#include "solar_os_memory.h"
}
#endif

namespace Renderer {
#if !defined(JET_EXTERNAL_LIBRARY) && (defined(ESP_PLATFORM) || defined(JET_SOLAROS_ALLOCATOR))
// The managed PSRAM pool is distinct from the SDK malloc heap. Geometry and
// scene queues must use that pool even when SDK PSRAM has been exhausted.
template<class T> struct JetAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    JetAllocator() = default;
    template<class U> JetAllocator(const JetAllocator<U>&) {}
    T *allocate(size_t count) {
        void *storage = count <= SIZE_MAX / sizeof(T)
            ? solar_os_memory_alloc(count * sizeof(T),
                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "jet.geometry") : nullptr;
        if (!storage) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        return static_cast<T *>(storage);
    }
    void deallocate(T *storage, size_t) { solar_os_memory_free(storage); }
    template<class U> bool operator==(const JetAllocator<U>&) const { return true; }
    template<class U> bool operator!=(const JetAllocator<U>&) const { return false; }
};
template<class T> using JetVector = std::vector<T, JetAllocator<T>>;
#else
template<class T> using JetVector = std::vector<T>;
#endif
}
