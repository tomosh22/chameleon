#pragma once

// Work queued for the application's own thread (plan section 4.9, synchronised mode).
//
// The agent thread prepares a patch and posts the final "write the hooks" step here; the
// application runs it from SyncPoint() on the thread it chooses. SyncPoint() must be cheap when
// nothing is queued, so the common case is one atomic load. In this work package nothing posts
// real work yet; the mechanism and a test-only posting entry point exist so that WP-17 only has
// to enqueue.
//
// Portable (std::mutex only) so it can be unit-tested anywhere.

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>

namespace cml::agent {

class SyncQueue {
public:
    using WorkFn = void (*)(void* context);

    // Any thread. Returns false (and queues nothing) when `work` is null.
    bool post(WorkFn work, void* context);

    // Cheap check for SyncPoint(): is anything waiting?
    bool has_work() const noexcept { return pending_.load(std::memory_order_acquire); }

    // Runs everything queued so far, in posting order, on the calling thread; returns how many
    // items ran. Work posted while this runs waits for the next call. Several threads may call
    // it; each queued item runs exactly once.
    std::size_t run_pending();

private:
    struct Item {
        WorkFn work;
        void* context;
    };

    std::mutex mutex_;
    std::deque<Item> items_;
    std::atomic<bool> pending_{false};
};

}  // namespace cml::agent
