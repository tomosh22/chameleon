#include "heap.h"

#if defined(_WIN32)

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "cml/util/fail_fast.h"

namespace cml::agent::heap {

namespace {

// Constant-initialised, so safe to use from operator new before any dynamic initialisation ran.
std::atomic<HANDLE> g_heap{nullptr};
std::atomic<bool> g_forbidden{false};
std::atomic<std::size_t> g_live_blocks{0};

constexpr std::size_t kNaturalAlignment = 16;  // what HeapAlloc guarantees on x64

HANDLE heap_handle() noexcept {
    HANDLE heap = g_heap.load(std::memory_order_acquire);
    if (heap != nullptr) {
        return heap;
    }
    // Growable, serialised (thread-safe) heap with default settings.
    HANDLE created = HeapCreate(0, 0, 0);
    if (created == nullptr) {
        fail_fast("cml_agent: HeapCreate failed; the agent cannot allocate memory");
    }
    HANDLE expected = nullptr;
    if (g_heap.compare_exchange_strong(expected, created, std::memory_order_acq_rel)) {
        return created;
    }
    HeapDestroy(created);  // another thread won the race
    return expected;
}

void check_allowed() noexcept {
    if (g_forbidden.load(std::memory_order_relaxed)) {
        fail_fast("cml_agent: heap use while allocation is forbidden (host threads are suspended)");
    }
}

}  // namespace

void* try_allocate(std::size_t size) noexcept {
    check_allowed();
    void* block = HeapAlloc(heap_handle(), 0, size == 0 ? 1 : size);
    if (block != nullptr) {
        g_live_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}

void* try_allocate_aligned(std::size_t size, std::size_t alignment) noexcept {
    if (alignment <= kNaturalAlignment) {
        return try_allocate(size);
    }
    // Over-allocate, align inside the block and keep the original pointer just in front.
    if (size > static_cast<std::size_t>(-1) - alignment - sizeof(void*)) {
        return nullptr;
    }
    void* raw = try_allocate(size + alignment + sizeof(void*));
    if (raw == nullptr) {
        return nullptr;
    }
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const std::uintptr_t aligned = (start + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void* allocate(std::size_t size) noexcept {
    void* block = try_allocate(size);
    if (block == nullptr) {
        fail_fast("cml_agent: out of memory");
    }
    return block;
}

void* allocate_aligned(std::size_t size, std::size_t alignment) noexcept {
    void* block = try_allocate_aligned(size, alignment);
    if (block == nullptr) {
        fail_fast("cml_agent: out of memory");
    }
    return block;
}

void release(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    check_allowed();
    HeapFree(heap_handle(), 0, block);
    g_live_blocks.fetch_sub(1, std::memory_order_relaxed);
}

void release_aligned(void* block, std::size_t alignment) noexcept {
    if (block == nullptr) {
        return;
    }
    if (alignment <= kNaturalAlignment) {
        release(block);
        return;
    }
    release(reinterpret_cast<void**>(block)[-1]);
}

void set_alloc_forbidden(bool forbidden) noexcept {
    g_forbidden.store(forbidden, std::memory_order_relaxed);
}

bool alloc_forbidden() noexcept {
    return g_forbidden.load(std::memory_order_relaxed);
}

std::size_t live_blocks() noexcept {
    return g_live_blocks.load(std::memory_order_relaxed);
}

}  // namespace cml::agent::heap

#endif  // _WIN32
