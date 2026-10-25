#include "sync_queue.h"

#include <utility>

namespace cml::agent {

bool SyncQueue::post(WorkFn work, void* context) {
    if (work == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    items_.push_back(Item{work, context});
    pending_.store(true, std::memory_order_release);
    return true;
}

std::size_t SyncQueue::run_pending() {
    std::deque<Item> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(items_);
        pending_.store(false, std::memory_order_release);
    }
    // Run outside the lock: the work may post more work or take a long time.
    for (const Item& item : batch) {
        item.work(item.context);
    }
    return batch.size();
}

}  // namespace cml::agent
