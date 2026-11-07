#pragma once

#include <cstddef>
#include <new>

namespace RNS { namespace Utilities {

	// SolarOS replaces the upstream TLSF pool allocators with the system heap.
	class Memory {

	public:

		template <typename T>
		struct ContainerAllocator {
			using value_type = T;

			ContainerAllocator() noexcept {}

			template <class U>
			ContainerAllocator(const ContainerAllocator<U>&) noexcept {}

			T* allocate(std::size_t n) {
				return static_cast<T*>(::operator new(n * sizeof(T)));
			}

			void deallocate(T* p, std::size_t) noexcept {
				::operator delete(p);
			}

			bool operator==(const ContainerAllocator&) const noexcept { return true; }
			bool operator!=(const ContainerAllocator&) const noexcept { return false; }
		};

		static size_t heap_size();
		static size_t heap_available();
		static void dump_heap_stats();

	};

} }
