#include "detail/counting.h"

#include "cml/pdb/types.h"

namespace cml::pdb::detail {
namespace {

std::atomic<std::size_t> g_tracked{0};
std::atomic<std::size_t> g_tracked_peak{0};
std::atomic<std::size_t> g_mapped{0};
std::atomic<std::size_t> g_mapped_peak{0};

void raise_peak(std::atomic<std::size_t>& peak, std::size_t value) noexcept {
    std::size_t seen = peak.load(std::memory_order_relaxed);
    while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

void note_alloc(std::size_t bytes) noexcept {
    const std::size_t now = g_tracked.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    raise_peak(g_tracked_peak, now);
}

void note_free(std::size_t bytes) noexcept {
    g_tracked.fetch_sub(bytes, std::memory_order_relaxed);
}

void note_map(std::size_t bytes) noexcept {
    const std::size_t now = g_mapped.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    raise_peak(g_mapped_peak, now);
}

void note_unmap(std::size_t bytes) noexcept {
    g_mapped.fetch_sub(bytes, std::memory_order_relaxed);
}

CountedBuffer::CountedBuffer(std::size_t size) {
    if (size == 0) {
        return;
    }
    data_ = static_cast<std::uint8_t*>(::operator new(size));
    size_ = size;
    note_alloc(size);
}

void CountedBuffer::reset() noexcept {
    if (data_ != nullptr) {
        ::operator delete(static_cast<void*>(data_));
        note_free(size_);
    }
    data_ = nullptr;
    size_ = 0;
}

MemoryStats current_memory_stats() noexcept {
    MemoryStats s;
    s.tracked_bytes = g_tracked.load(std::memory_order_relaxed);
    s.tracked_peak_bytes = g_tracked_peak.load(std::memory_order_relaxed);
    s.mapped_bytes = g_mapped.load(std::memory_order_relaxed);
    s.mapped_peak_bytes = g_mapped_peak.load(std::memory_order_relaxed);
    return s;
}

void reset_peaks() noexcept {
    g_tracked_peak.store(g_tracked.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_mapped_peak.store(g_mapped.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

}  // namespace cml::pdb::detail
