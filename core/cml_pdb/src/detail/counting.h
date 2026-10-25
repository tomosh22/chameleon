#pragma once

// Allocation accounting for cml_pdb. Every long-lived container in the index uses detail::Vec so
// that MemoryStats::tracked_* reflects what the index holds (the memory budget is 1.5x the PDB
// size, counting the mapped file; see docs/wp/WP-04.md).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

namespace cml::pdb::detail {

void note_alloc(std::size_t bytes) noexcept;
void note_free(std::size_t bytes) noexcept;
void note_map(std::size_t bytes) noexcept;
void note_unmap(std::size_t bytes) noexcept;
}  // namespace cml::pdb::detail

namespace cml::pdb {
struct MemoryStats;
}

namespace cml::pdb::detail {
cml::pdb::MemoryStats current_memory_stats() noexcept;
void reset_peaks() noexcept;

template <class T>
struct CountingAllocator {
    using value_type = T;
    CountingAllocator() noexcept = default;
    template <class U>
    CountingAllocator(const CountingAllocator<U>&) noexcept {}  // NOLINT: allocator rebind

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        T* p = static_cast<T*>(::operator new(bytes));
        note_alloc(bytes);
        return p;
    }
    void deallocate(T* p, std::size_t n) noexcept {
        ::operator delete(static_cast<void*>(p));
        note_free(n * sizeof(T));
    }
    template <class U>
    bool operator==(const CountingAllocator<U>&) const noexcept {
        return true;
    }
    template <class U>
    bool operator!=(const CountingAllocator<U>&) const noexcept {
        return false;
    }
};

template <class T>
using Vec = std::vector<T, CountingAllocator<T>>;

// A heap buffer that is counted but, unlike Vec<uint8_t>, is not zero-initialised.
class CountedBuffer {
public:
    CountedBuffer() noexcept = default;
    explicit CountedBuffer(std::size_t size);
    CountedBuffer(CountedBuffer&& o) noexcept : data_(o.data_), size_(o.size_) {
        o.data_ = nullptr;
        o.size_ = 0;
    }
    CountedBuffer& operator=(CountedBuffer&& o) noexcept {
        if (this != &o) {
            reset();
            data_ = o.data_;
            size_ = o.size_;
            o.data_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }
    CountedBuffer(const CountedBuffer&) = delete;
    CountedBuffer& operator=(const CountedBuffer&) = delete;
    ~CountedBuffer() { reset(); }

    std::uint8_t* data() noexcept { return data_; }
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    void reset() noexcept;

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace cml::pdb::detail
