// SyncPoint plumbing (plan section 4.9): work posted from any thread runs only when the
// application thread calls run_pending(), in order, exactly once.
#include "sync_queue.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "doctest.h"

using cml::agent::SyncQueue;

namespace {

struct Recorder {
    std::vector<int> values;
    std::thread::id ran_on;
};

void record_one(void* context) {
    auto* r = static_cast<Recorder*>(context);
    r->values.push_back(1);
    r->ran_on = std::this_thread::get_id();
}
void record_two(void* context) {
    static_cast<Recorder*>(context)->values.push_back(2);
}
void record_three(void* context) {
    static_cast<Recorder*>(context)->values.push_back(3);
}

struct Reposter {
    SyncQueue* queue;
    int runs = 0;
};
void repost(void* context) {
    auto* r = static_cast<Reposter*>(context);
    ++r->runs;
    if (r->runs == 1) {
        r->queue->post(&repost, context);  // posted while running: waits for the next pass
    }
}

}  // namespace

TEST_CASE("an empty queue reports no work and runs nothing") {
    SyncQueue queue;
    CHECK_FALSE(queue.has_work());
    CHECK(queue.run_pending() == 0);
}

TEST_CASE("null work is refused") {
    SyncQueue queue;
    CHECK_FALSE(queue.post(nullptr, nullptr));
    CHECK_FALSE(queue.has_work());
}

TEST_CASE("posted work waits for the application thread and runs in order") {
    SyncQueue queue;
    Recorder recorder;
    REQUIRE(queue.post(&record_one, &recorder));
    REQUIRE(queue.post(&record_two, &recorder));
    REQUIRE(queue.post(&record_three, &recorder));
    CHECK(queue.has_work());
    CHECK(recorder.values.empty());  // nothing ran yet: only run_pending() executes work

    CHECK(queue.run_pending() == 3);
    CHECK(recorder.values == std::vector<int>{1, 2, 3});
    CHECK(recorder.ran_on == std::this_thread::get_id());
    CHECK_FALSE(queue.has_work());
    CHECK(queue.run_pending() == 0);  // exactly once
}

TEST_CASE("work posted from another thread runs on the thread that calls run_pending") {
    SyncQueue queue;
    Recorder recorder;
    std::thread producer([&] { queue.post(&record_one, &recorder); });
    producer.join();
    CHECK(queue.has_work());
    CHECK(queue.run_pending() == 1);
    CHECK(recorder.ran_on == std::this_thread::get_id());
    CHECK(recorder.ran_on != producer.get_id());
}

TEST_CASE("work posted while running waits for the next pass") {
    SyncQueue queue;
    Reposter reposter{&queue};
    queue.post(&repost, &reposter);
    CHECK(queue.run_pending() == 1);
    CHECK(reposter.runs == 1);
    CHECK(queue.has_work());
    CHECK(queue.run_pending() == 1);
    CHECK(reposter.runs == 2);
    CHECK_FALSE(queue.has_work());
}

TEST_CASE("concurrent producers and one consumer lose and duplicate nothing") {
    SyncQueue queue;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 500;
    std::atomic<int> executed{0};
    struct Count {
        static void bump(void* context) { static_cast<std::atomic<int>*>(context)->fetch_add(1); }
    };

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                queue.post(&Count::bump, &executed);
            }
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (executed.load() < kProducers * kPerProducer && std::chrono::steady_clock::now() < deadline) {
        if (queue.has_work()) {
            queue.run_pending();
        } else {
            std::this_thread::yield();
        }
    }
    for (std::thread& t : producers) {
        t.join();
    }
    queue.run_pending();
    CHECK(executed.load() == kProducers * kPerProducer);
}
