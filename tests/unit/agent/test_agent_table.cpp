// The agent's request table, driven directly (no broker, no agent thread): the limits and the
// eviction rules documented as invariants I1-I6 in agent.h.
#if defined(_WIN32)

#if !defined(CML_AGENT_BUILD)
#define CML_AGENT_BUILD
#endif
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "agent.h"
#include "doctest.h"

using cml::agent::Agent;

namespace {

constexpr std::size_t kAppLimit = 1024;  // kMaxOutstanding
constexpr std::uint32_t kQueueFull = 5217;
constexpr std::uint32_t kCancelled = 5215;

std::wstring module_name(std::size_t i) {
    return L"table_module_" + std::to_wstring(i) + L".dll";
}

bool wait_for_waiters(Agent& agent, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (agent.waiters_for_testing() < count) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

TEST_CASE("exactly 1024 application requests may be open at once") {
    Agent agent(CML_MODE_IMMEDIATE);
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < kAppLimit; ++i) {
        std::uint64_t id = 0;
        REQUIRE(agent.enable_module(module_name(i).c_str(), 0, &id) == CML_STATUS_OK);
        ids.push_back(id);
    }
    std::uint64_t extra = 0;
    CHECK(agent.enable_module(L"one_too_many.dll", 0, &extra) == static_cast<cml_status>(kQueueFull));
    CHECK(agent.disable_module(L"one_too_many.dll", &extra) == static_cast<cml_status>(kQueueFull));

    // Answering one frees exactly one slot; the unconsumed answer does not use any.
    agent.complete_for_testing(ids[0], 0, "ok");
    CHECK(agent.enable_module(L"fits_now.dll", 0, &extra) == CML_STATUS_OK);
    CHECK(agent.enable_module(L"one_too_many.dll", 0, &extra) == static_cast<cml_status>(kQueueFull));

    // Answers piling up (collected or not) never count against the limit.
    for (std::size_t i = 1; i < 700; ++i) {
        agent.complete_for_testing(ids[i], 0, "ok");
    }
    // 1024 + 1 queued - 700 answered = 325 open, so exactly 699 more fit.
    for (std::size_t i = 0; i < 699; ++i) {
        CHECK(agent.enable_module(L"after_flood.dll", 0, &extra) == CML_STATUS_OK);
    }
    CHECK(agent.enable_module(L"over_again.dll", 0, &extra) == static_cast<cml_status>(kQueueFull));
    agent.stop();
}

TEST_CASE("an answer somebody is waiting for survives a flood of newer requests") {
    Agent agent(CML_MODE_IMMEDIATE);
    std::uint64_t watched = 0;
    REQUIRE(agent.enable_module(L"watched.dll", 0, &watched) == CML_STATUS_OK);

    std::atomic<cml_status> wait_result{CML_STATUS_TIMEOUT};
    std::atomic<std::uint32_t> reply_status{0xFFFFFFFFu};
    std::thread waiter([&] {
        cml_reply reply{};
        wait_result = agent.wait_request(watched, 20000, &reply);
        reply_status = reply.status;
    });
    REQUIRE(wait_for_waiters(agent, 1));

    agent.complete_for_testing(watched, 0, "watched answer");
    // Flood: many more answers than the table keeps, none collected. The oldest unwatched ones are
    // evicted; the watched one is older than all of them and still must not be.
    for (int round = 0; round < 3; ++round) {
        std::vector<std::uint64_t> ids;
        for (int i = 0; i < 600; ++i) {
            std::uint64_t id = 0;
            REQUIRE(agent.disable_module(L"flood.dll", &id) == CML_STATUS_OK);
            ids.push_back(id);
        }
        for (const std::uint64_t id : ids) {
            agent.complete_for_testing(id, 0, "flood");
        }
    }
    waiter.join();
    CHECK(wait_result.load() == CML_STATUS_OK);
    CHECK(reply_status.load() == 0);
    agent.stop();
}

TEST_CASE("unwatched answers are evicted oldest first and then forgotten") {
    Agent agent(CML_MODE_IMMEDIATE);
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 1000; ++i) {
        std::uint64_t id = 0;
        REQUIRE(agent.disable_module(L"old.dll", &id) == CML_STATUS_OK);
        ids.push_back(id);
    }
    for (const std::uint64_t id : ids) {
        agent.complete_for_testing(id, 0, "done");
    }
    cml_reply reply{};
    CHECK(agent.wait_request(ids.front(), 0, &reply) == CML_STATUS_UNKNOWN_REQUEST);  // evicted
    CHECK(agent.wait_request(ids.back(), 0, &reply) == CML_STATUS_OK);                // newest kept
    CHECK(agent.wait_request(ids.back(), 0, &reply) == CML_STATUS_UNKNOWN_REQUEST);   // consumed once
    agent.stop();
}

TEST_CASE("stop() cancels 800 open requests for every waiter and keeps the answers collectable") {
    Agent agent(CML_MODE_IMMEDIATE);
    constexpr std::size_t kOpen = 800;
    constexpr std::size_t kWaiters = 64;
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < kOpen; ++i) {
        std::uint64_t id = 0;
        REQUIRE(agent.enable_module(module_name(i).c_str(), 0, &id) == CML_STATUS_OK);
        ids.push_back(id);
    }
    std::atomic<std::size_t> cancelled{0};
    std::atomic<std::size_t> other{0};
    std::vector<std::thread> waiters;
    for (std::size_t i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&, i] {
            cml_reply reply{};
            const cml_status status = agent.wait_request(ids[i], 30000, &reply);
            if (status == CML_STATUS_OK && reply.status == kCancelled) {
                ++cancelled;
            } else {
                ++other;
            }
        });
    }
    REQUIRE(wait_for_waiters(agent, kWaiters));
    agent.stop();
    for (std::thread& t : waiters) {
        t.join();
    }
    CHECK(cancelled.load() == kWaiters);
    CHECK(other.load() == 0);

    // Nothing was evicted by the cancellation: everybody else can still collect theirs.
    std::size_t collected = 0;
    for (std::size_t i = kWaiters; i < kOpen; ++i) {
        cml_reply reply{};
        if (agent.wait_request(ids[i], 0, &reply) == CML_STATUS_OK && reply.status == kCancelled) {
            ++collected;
        }
    }
    CHECK(collected == kOpen - kWaiters);

    std::uint64_t id = 0;
    CHECK(agent.enable_module(L"after_stop.dll", 0, &id) == static_cast<cml_status>(kCancelled));
}

TEST_CASE("an enabled module stays tracked however many answers were dropped") {
    Agent agent(CML_MODE_IMMEDIATE);
    // 600 enables answered "ok" and never collected: the tracked set is not the answer table, so
    // evicting answers cannot lose a module (see the reconnect test for the re-enable itself).
    for (int i = 0; i < 600; ++i) {
        std::uint64_t id = 0;
        REQUIRE(agent.enable_module(module_name(static_cast<std::size_t>(i)).c_str(), 0, &id) == CML_STATUS_OK);
        agent.complete_for_testing(id, 0, "ok");
    }
    CHECK(agent.tracked_modules_for_testing() == 600);
    agent.stop();
}

#endif  // _WIN32
