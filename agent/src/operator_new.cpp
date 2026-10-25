// Replaces the global operator new / delete of cml_agent.dll with the private agent heap
// (heap.h). Compiled into the DLL only: the DLL has its own statically linked C++ runtime, so
// these replacements affect exactly the code inside cml_agent.dll and nothing in the host.
//
// Allocation failure aborts the process (the agent is built without exceptions, so there is
// nothing to throw); the nothrow forms return nullptr instead.
#if defined(_WIN32)

#include <cstddef>
#include <new>

#include "heap.h"

namespace heap = cml::agent::heap;

void* operator new(std::size_t size) {
    return heap::allocate(size);
}
void* operator new[](std::size_t size) {
    return heap::allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return heap::try_allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return heap::try_allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return heap::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return heap::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return heap::try_allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return heap::try_allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* block) noexcept {
    heap::release(block);
}
void operator delete[](void* block) noexcept {
    heap::release(block);
}
void operator delete(void* block, std::size_t) noexcept {
    heap::release(block);
}
void operator delete[](void* block, std::size_t) noexcept {
    heap::release(block);
}
void operator delete(void* block, const std::nothrow_t&) noexcept {
    heap::release(block);
}
void operator delete[](void* block, const std::nothrow_t&) noexcept {
    heap::release(block);
}
void operator delete(void* block, std::align_val_t alignment) noexcept {
    heap::release_aligned(block, static_cast<std::size_t>(alignment));
}
void operator delete[](void* block, std::align_val_t alignment) noexcept {
    heap::release_aligned(block, static_cast<std::size_t>(alignment));
}
void operator delete(void* block, std::size_t, std::align_val_t alignment) noexcept {
    heap::release_aligned(block, static_cast<std::size_t>(alignment));
}
void operator delete[](void* block, std::size_t, std::align_val_t alignment) noexcept {
    heap::release_aligned(block, static_cast<std::size_t>(alignment));
}
void operator delete(void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    heap::release_aligned(block, static_cast<std::size_t>(alignment));
}
void operator delete[](void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    heap::release_aligned(block, static_cast<std::size_t>(alignment));
}

#endif  // _WIN32
