// The agent's private heap (heap.h). The operator new/delete replacement lives in the DLL only
// (operator_new.cpp), so these tests call the heap functions directly.
#include "heap.h"

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "doctest.h"

namespace heap = cml::agent::heap;

TEST_CASE("heap allocate and release balance the live-block count") {
    const std::size_t before = heap::live_blocks();
    void* a = heap::allocate(100);
    void* b = heap::allocate(1);
    void* zero = heap::allocate(0);  // zero-sized requests still yield a unique block
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(zero != nullptr);
    CHECK(a != b);
    CHECK(heap::live_blocks() == before + 3);
    std::memset(a, 0xAB, 100);  // the whole block is writable
    heap::release(a);
    heap::release(b);
    heap::release(zero);
    heap::release(nullptr);  // allowed
    CHECK(heap::live_blocks() == before);
}

TEST_CASE("heap blocks are 16-byte aligned") {
    for (std::size_t size : {1u, 7u, 16u, 100u, 4096u}) {
        void* block = heap::allocate(size);
        CHECK(reinterpret_cast<std::uintptr_t>(block) % 16 == 0);
        heap::release(block);
    }
}

TEST_CASE("over-aligned allocations honour the alignment and release cleanly") {
    const std::size_t before = heap::live_blocks();
    for (std::size_t alignment : {32u, 64u, 256u, 4096u}) {
        void* block = heap::allocate_aligned(100, alignment);
        REQUIRE(block != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(block) % alignment == 0);
        std::memset(block, 0xCD, 100);
        heap::release_aligned(block, alignment);
    }
    // Alignments up to 16 take the plain path.
    void* plain = heap::allocate_aligned(10, 16);
    CHECK(reinterpret_cast<std::uintptr_t>(plain) % 16 == 0);
    heap::release_aligned(plain, 16);
    CHECK(heap::live_blocks() == before);
}

TEST_CASE("absurd sizes fail without aborting through try_allocate") {
    CHECK(heap::try_allocate(static_cast<std::size_t>(-1)) == nullptr);
    CHECK(heap::try_allocate_aligned(static_cast<std::size_t>(-1), 64) == nullptr);
}

TEST_CASE("the allocation-forbidden switch (WP-10 hook) can be set and cleared") {
    CHECK_FALSE(heap::alloc_forbidden());
    heap::set_alloc_forbidden(true);
    CHECK(heap::alloc_forbidden());
    heap::set_alloc_forbidden(false);
    CHECK_FALSE(heap::alloc_forbidden());
    // Allocation works again after clearing it.
    void* block = heap::allocate(8);
    heap::release(block);
}

TEST_CASE("the heap is usable from many threads at once") {
    const std::size_t before = heap::live_blocks();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([] {
            std::vector<void*> blocks;
            for (int i = 0; i < 2000; ++i) {
                blocks.push_back(heap::allocate(static_cast<std::size_t>(16 + i % 200)));
            }
            for (void* block : blocks) {
                heap::release(block);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(heap::live_blocks() == before);
}
