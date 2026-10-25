#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace cml::pdb::detail {

inline unsigned default_thread_count() noexcept {
    const unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) {
        return 4;
    }
    return hc > 16 ? 16 : hc;
}

// Runs fn(i) for i in [0, count) on up to `threads` threads (the calling thread participates).
// fn must be thread-safe and must not throw.
template <class Fn>
void parallel_for(std::size_t count, unsigned threads, Fn&& fn) {
    if (threads == 0) {
        threads = default_thread_count();
    }
    if (count == 0) {
        return;
    }
    if (threads <= 1 || count < 2) {
        for (std::size_t i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    if (threads > count) {
        threads = static_cast<unsigned>(count);
    }
    std::atomic<std::size_t> next{0};
    auto worker = [&]() {
        for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) {
                return;
            }
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    for (std::thread& t : pool) {
        t.join();
    }
}

}  // namespace cml::pdb::detail
