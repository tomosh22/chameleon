// Review follow-ups of WP-10 (Windows): the tests that prove the stopped-world discipline is
// enforced and not just claimed.
//   * An allocation made while the world is stopped is detected (agent heap always, C runtime in
//     Debug cells), and the detector is shown to fire.
//   * The window budget is enforced inside a pass: per enumeration step, per suspension, per context
//     read; an overrun resumes everything and reports CML4309, never CML4305.
//   * The thread table lookup stays linear in the thread count.
//   * The step-limit exit of the enumeration does not leak a handle.
//   * Strategy 1 is used only for an even entry and executable padding.
//   * The offender is let run for a moment (offender_run_us) and checked again; the number of such
//     steps is bounded, and a spent budget during them reports CML4309.
#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "alloc_guard.h"
#include "crash_report.h"
#include "doctest.h"
#include "heap.h"
#include "hook/diag_ids.h"
#include "hook/installer.h"
#include "hook/thread_set.h"
#include "hook_test_support.h"

using namespace cml::agent::hook;
using namespace hooktest;
namespace agent_heap = cml::agent::heap;

namespace {

BatchResult install_one(Installer& installer, const void* target, const void* replacement, const char* name,
                        HookOutcome* outcome = nullptr) {
    std::vector<HookOutcome> out;
    const HookRequest request{target, replacement, name};
    BatchResult r = installer.install(&request, 1, out);
    if (outcome != nullptr) {
        *outcome = out.empty() ? HookOutcome{} : out[0];
    }
    return r;
}

BatchResult revert_one(Installer& installer, const void* target, const char* name) {
    std::vector<HookOutcome> out;
    const HookRequest request{target, nullptr, name};
    return installer.revert(&request, 1, out);
}

Bytes around(const Placed& p) {
    return snapshot(p.entry - 16, 16 + 48);
}

struct Workers {
    std::atomic<bool> stop{false};
    std::atomic<long long> counter{0};
    std::vector<std::thread> threads;
    explicit Workers(int count) {
        for (int i = 0; i < count; ++i) {
            threads.emplace_back([this] {
                while (!stop.load(std::memory_order_relaxed)) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }
    ~Workers() {
        stop.store(true);
        for (std::thread& t : threads) {
            t.join();
        }
    }
    bool running_again() {
        const long long mark = counter.load();
        for (int i = 0; i < 100; ++i) {
            Sleep(5);
            if (counter.load() > mark) {
                return true;
            }
        }
        return false;
    }
};

// ---- allocation inside the window ---------------------------------------------------------------------

char* volatile g_sink = nullptr;
void* volatile g_crt_sink = nullptr;

// Called with the world stopped (test_in_window). Allocates on purpose: through `new`, which in this
// executable is the agent's operator new, and through malloc.
void allocate_in_window(void*) {
    g_sink = new char[24];
    delete[] g_sink;
    g_crt_sink = std::malloc(24);
    std::free(g_crt_sink);
}

struct CountingScope {
    CountingScope() {
        g_alloc_violations.agent_heap = 0;
        g_alloc_violations.crt = 0;
        g_alloc_violations.counting = true;
    }
    ~CountingScope() { g_alloc_violations.counting = false; }
};

}  // namespace

TEST_CASE("this executable's operator new is the agent's heap: a forbidden window sees it") {
    CountingScope counting;
    agent_heap::set_alloc_forbidden(true);
    g_sink = new char[8];
    agent_heap::set_alloc_forbidden(false);
    CHECK(g_alloc_violations.agent_heap.load() == 1);
    delete[] g_sink;
    CHECK(g_alloc_violations.agent_heap.load() == 1);  // released outside the window: not counted
}

TEST_CASE("another thread allocating while the window is open is not blamed on the window") {
    CountingScope counting;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    std::thread other([&] {
        while (!go.load()) {
            YieldProcessor();
        }
        char* p = new char[8];
        delete[] p;
        done = true;
    });
    // Nothing on this thread may allocate between the two flag changes (a thread start does).
    agent_heap::set_alloc_forbidden(true);
    go = true;
    while (!done.load()) {
        YieldProcessor();
    }
    agent_heap::set_alloc_forbidden(false);
    other.join();
    CHECK(g_alloc_violations.agent_heap.load() == 0);
}

TEST_CASE("negative test: an allocation inside the stopped window is detected") {
    Arena arena;
    const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
    const Placed new_fn = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();
    Workers workers(3);

    InstallerOptions options;
    options.test_in_window = &allocate_in_window;
    options.window_budget_ms = 30000;  // a stalled CI runner must not turn this into a timeout (CML4309)
    Installer installer(options);

    CountingScope counting;
    const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "fn");
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    // new + delete[] through the agent heap, both seen.
    CHECK(g_alloc_violations.agent_heap.load() >= 2);
    if (kCrtHookAvailable) {
        // malloc + free through the Debug CRT, both seen.
        CHECK(g_alloc_violations.crt.load() >= 2);
    } else {
        MESSAGE("Release CRT: no allocation hook, only the agent heap path is checked in this cell");
    }
    CHECK_FALSE(agent_heap::alloc_forbidden());
    {
        const BatchResult checked = revert_one(installer, old_fn.entry, "fn");
        REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
    }
}

TEST_CASE("positive control: installing and reverting allocate nothing while the world is stopped") {
    Arena arena;
    const Placed a = arena.place_padded(cl_release(0), 8, 8);
    const Placed b = arena.place_padded(push_first(0), 8, 8);
    const Placed c = arena.place_padded(cl_debug(0), 0, 8);
    const Placed repl = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();
    Workers workers(3);
    InstallerOptions options;
    options.window_budget_ms = 30000;  // as above
    Installer installer(options);
    CountingScope counting;
    std::vector<HookOutcome> out;
    const HookRequest requests[] = {{a.entry, repl.entry, "a"}, {b.entry, repl.entry, "b"}, {c.entry, repl.entry, "c"}};
    {
        const BatchResult checked = installer.install(requests, 3, out);
        REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
    }
    {
        const BatchResult checked = installer.install(requests, 3, out);
        REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
    }  // unchanged
    {
        const BatchResult checked = installer.revert(requests, 3, out);
        REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
    }
    CHECK(g_alloc_violations.agent_heap.load() == 0);
    CHECK(g_alloc_violations.crt.load() == 0);
}

// ---- the budget inside a pass ----------------------------------------------------------------------------

TEST_CASE("ThreadSet: the deadline is checked inside the pass, everything can be resumed afterwards") {
    Workers workers(4);
    ThreadSet set;
    REQUIRE(set.init(64));
    // The n-th deadline check reports "passed"; n sweeps the places where the checks are: pass start,
    // each enumeration step, each suspension, each context read.
    for (unsigned n : {1u, 2u, 3u, 4u, 5u, 7u, 9u, 12u, 16u, 24u, 40u}) {
        INFO("expire after " << n << " checks");
        set.set_test_knobs(n, 0);
        // Nothing between suspend_all and resume_all may allocate or take a lock: the threads that are
        // stopped could be holding the heap or CRT lock. Results are copied to locals first, and
        // checked after the world runs again.
        const ThreadSet::Result r = set.suspend_all(GetTickCount64() + 60000, 16);
        const bool timed_out_while_stopped = set.timed_out();
        set.resume_all();
        if (r == ThreadSet::Result::Timeout) {
            CHECK(timed_out_while_stopped);
        } else {
            CHECK(r == ThreadSet::Result::Ok);  // the budget was larger than the work
            CHECK_FALSE(timed_out_while_stopped);
        }
        CHECK_FALSE(set.timed_out());
        CHECK(set.size() == 0);
        CHECK(workers.running_again());
    }
    set.set_test_knobs(0, 0);
}

TEST_CASE("ThreadSet: a deadline already in the past times out before anything is stopped") {
    Workers workers(2);
    ThreadSet set;
    REQUIRE(set.init(64));
    CHECK(set.suspend_all(GetTickCount64() - 1000, 16) == ThreadSet::Result::Timeout);
    CHECK(set.size() == 0);
    set.resume_all();
}

TEST_CASE("the budget runs out in the middle of a pass: CML4309, not CML4305; bytes untouched; threads resume") {
    Arena arena;
    const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
    const Placed new_fn = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();
    Workers workers(4);
    const Bytes before = around(old_fn);

    int timeouts = 0;
    for (unsigned n : {2u, 3u, 5u, 8u, 13u}) {
        INFO("expire after " << n << " checks");
        InstallerOptions options;
        options.test_expire_after_checks = n;
        Installer installer(options);
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "fn", &outcome);
        REQUIRE_MESSAGE(!r.ok(), "expected the budget to run out: " << r.diag.format());
        CHECK(r.diag.code == ids::kWindowTimeout);
        CHECK(r.diag.code != ids::kThreadInWindow);
        CHECK(r.diag.code != ids::kThreadsNotConverging);
        CHECK(outcome.status == HookStatus::NotAttempted);
        CHECK(outcome.diag.code == ids::kWindowTimeout);
        CHECK(r.stats.attempts == 1);  // a timeout is final, not retried
        CHECK(around(old_fn) == before);
        CHECK(installer.hook_count() == 0);
        CHECK_FALSE(agent_heap::alloc_forbidden());
        CHECK(workers.running_again());
        ++timeouts;
    }
    CHECK(timeouts == 5);
}

TEST_CASE("the budget runs out while the offender is being stepped: CML4309") {
    Arena arena;
    const Placed spin = arena.place_padded(spin_until(), 0, 0);
    const Placed repl = arena.place_padded(ret_only(), 0, 0);
    arena.seal();
    const Bytes before = around(spin);

    volatile char flag = 0;
    std::atomic<bool> started{false};
    std::thread spinner([&] {
        started = true;
        spin.as_void()(&flag);
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    Sleep(20);

    InstallerOptions options;
    options.window_budget_ms = 40;
    options.steps_per_attempt = 1000000;  // only the budget can end the stepping
    options.max_attempts = 50;
    Installer installer(options);
    const auto start = std::chrono::steady_clock::now();
    const BatchResult r = install_one(installer, spin.entry, repl.entry, "spin_until");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK_MESSAGE(r.diag.code == ids::kWindowTimeout, r.diag.format());
    CHECK(r.stats.attempts == 1);
    CHECK(elapsed < std::chrono::seconds(10));
    CHECK(around(spin) == before);
    flag = 1;
    spinner.join();  // resumed, still alive, leaves now
}

// ---- offender stepping -------------------------------------------------------------------------------------

TEST_CASE("an offender that leaves while only it runs is handled inside ONE stop of the world") {
    Arena arena;
    // slow_const(n): `dec ecx / jnz` inside its first 4 bytes n times, then `mov eax, v ; ret`. A
    // thread inside the loop is inside the 5-byte window the whole time, then leaves by itself.
    const Placed slow = arena.place_padded(slow_const(7), 0, 0);
    const Placed repl = arena.place_padded(ret_only(), 0, 0);
    arena.seal();
    const Bytes before = around(slow);

    std::atomic<bool> started{false};
    std::atomic<int> result{-1};
    std::thread runner([&] {
        started = true;
        result = slow.as_int()(600000000);  // a few hundred milliseconds of looping
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    Sleep(10);

    InstallerOptions options;
    options.max_attempts = 1;           // no second stop: only stepping can succeed
    options.steps_per_attempt = 20000;  // bounded, and enough
    options.window_budget_ms = 20000;
    options.offender_run_us = 1000;
    Installer installer(options);
    HookOutcome outcome;
    const BatchResult r = install_one(installer, slow.entry, repl.entry, "slow_const", &outcome);
    runner.join();
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    CHECK(r.stats.attempts == 1);
    CHECK(r.stats.retries >= 1);
    CHECK(outcome.status == HookStatus::Installed);
    CHECK(result.load() == 7);  // the call that was running finished normally
    CHECK(slow.entry[0] == 0xE9);
    {
        const BatchResult checked = revert_one(installer, slow.entry, "slow_const");
        REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
    }
    CHECK(around(slow) == before);
}

TEST_CASE("the number of offender steps per stop is bounded") {
    Arena arena;
    const Placed spin = arena.place_padded(spin_until(), 0, 0);
    const Placed repl = arena.place_padded(ret_only(), 0, 0);
    arena.seal();
    volatile char flag = 0;
    std::atomic<bool> started{false};
    std::thread spinner([&] {
        started = true;
        spin.as_void()(&flag);
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    Sleep(20);
    InstallerOptions options;
    options.max_attempts = 3;
    options.steps_per_attempt = 4;
    options.offender_run_us = 1000;
    Installer installer(options);
    const BatchResult r = install_one(installer, spin.entry, repl.entry, "spin_until");
    CHECK(r.diag.code == ids::kThreadInWindow);
    CHECK(r.stats.attempts == 3);
    CHECK(r.stats.retries == 3 * (4 + 1));  // per stop: the first sighting plus one per step
    flag = 1;
    spinner.join();
}

// ---- thread table ----------------------------------------------------------------------------------------------

TEST_CASE("ThreadSet handles several hundred threads in one stop (set lookup is not quadratic)") {
    constexpr int kThreads = 600;
    std::atomic<bool> release{false};
    std::atomic<int> up{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            ++up;
            while (!release.load(std::memory_order_relaxed)) {
                Sleep(1);
            }
        });
    }
    while (up.load() < kThreads) {
        Sleep(1);
    }

    ThreadSet set;
    REQUIRE(set.init(4096));
    // Everything that is needed while the world is stopped is allocated before it stops.
    std::vector<DWORD> ids(4096, 0);
    const auto start = std::chrono::steady_clock::now();
    const ThreadSet::Result r = set.suspend_all(GetTickCount64() + 60000, 16);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    const std::size_t stopped = set.size() <= ids.size() ? set.size() : ids.size();
    const unsigned passes = set.passes();
    std::size_t not_suspended = 0, no_rip = 0;
    for (std::size_t i = 0; i < stopped; ++i) {
        ids[i] = set.entry(i).id;
        not_suspended += set.entry(i).suspended ? 0 : 1;
        no_rip += set.entry(i).rip_valid ? 0 : 1;
    }
    set.resume_all();
    REQUIRE(r == ThreadSet::Result::Ok);
    MESSAGE("suspend_all of " << stopped << " threads took " << elapsed.count() << " ms in " << passes << " pass(es)");
    CHECK(stopped >= static_cast<std::size_t>(kThreads));
    CHECK(not_suspended == 0);
    CHECK(no_rip == 0);
    std::sort(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(stopped));
    CHECK(std::adjacent_find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(stopped)) ==
          ids.begin() + static_cast<std::ptrdiff_t>(stopped));  // each thread once
    CHECK(elapsed < std::chrono::seconds(30));

    release = true;
    for (std::thread& t : threads) {
        t.join();
    }
}

// ---- handle leak at the step-limit exit -----------------------------------------------------------------------

TEST_CASE("the step-limit exit of the enumeration closes its temporary handle") {
    Workers workers(3);
    ThreadSet set;
    REQUIRE(set.init(64));

    DWORD handles_before = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
    constexpr int kRounds = 60;
    for (int i = 0; i < kRounds; ++i) {
        // Everything is known after the first call, so the second call's first step visits a thread
        // that is already in the set (or this one) and holds a temporary handle when the step budget
        // of 1 is used up.
        const ThreadSet::Result first = set.suspend_all(GetTickCount64() + 60000, 16);
        set.set_test_knobs(0, 1);
        const ThreadSet::Result second = set.suspend_all(GetTickCount64() + 60000, 16);
        set.set_test_knobs(0, 0);
        set.resume_all();  // checks only after the world runs again (see the sweep test)
        REQUIRE(first == ThreadSet::Result::Ok);
        CHECK(second == ThreadSet::Result::NotConverging);
    }
    DWORD handles_after = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    MESSAGE("process handles before=" << handles_before << " after=" << handles_after << " over " << kRounds
                                      << " forced step-limit exits");
    // A leak of one handle per exit would show as +60.
    CHECK(static_cast<long long>(handles_after) - static_cast<long long>(handles_before) < 10);
    CHECK(workers.running_again());
}

// ---- strategy 1: alignment and executable padding -----------------------------------------------------------------

TEST_CASE("an odd entry never gets strategy 1, an even one that is not 16-aligned does") {
    for (std::size_t misalign : {1u, 3u, 5u, 7u, 9u, 15u}) {
        INFO("entry misalignment " << misalign);
        Arena arena;
        const Placed old_fn = arena.place_padded(cl_release(0), 8, 8, 0xCC, 0xCC, misalign);
        const Placed new_fn = arena.place_padded(lea_ret(100), 8, 8);
        arena.seal();
        REQUIRE((reinterpret_cast<std::uintptr_t>(old_fn.entry) & 1) == 1);
        for (int i = 1; i <= 8; ++i) {
            REQUIRE(old_fn.entry[-i] == 0xCC);  // padding is there; only the alignment rules strategy 1 out
        }
        const Bytes before = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "odd", &outcome);
        REQUIRE_MESSAGE(r.ok(), r.diag.format());
        CHECK(outcome.strategy == Strategy::FiveByte);
        CHECK(old_fn.entry[0] == 0xE9);
        CHECK(old_fn.entry[-1] == 0xCC);  // the padding was not used
        CHECK(old_fn.as_int()(1) == 101);
        {
            const BatchResult checked = revert_one(installer, old_fn.entry, "odd");
            REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
        }
        CHECK(around(old_fn) == before);
    }
    for (std::size_t misalign : {2u, 4u, 6u, 14u}) {
        INFO("entry misalignment " << misalign);
        Arena arena;
        const Placed old_fn = arena.place_padded(cl_release(0), 8, 8, 0xCC, 0xCC, misalign);
        const Placed new_fn = arena.place_padded(lea_ret(100), 8, 8);
        arena.seal();
        REQUIRE((reinterpret_cast<std::uintptr_t>(old_fn.entry) & 1) == 0);
        Installer installer;
        HookOutcome outcome;
        {
            const BatchResult checked = install_one(installer, old_fn.entry, new_fn.entry, "even", &outcome);
            REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
        }
        CHECK(outcome.strategy == Strategy::PaddingTwoByte);
        CHECK(old_fn.as_int()(1) == 101);
        {
            const BatchResult checked = revert_one(installer, old_fn.entry, "even");
            REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
        }
    }
}

TEST_CASE("padding in a page that is not executable falls back to strategy 2") {
    Arena arena;
    // The entry is the first byte of the second page; its 8 CC bytes of padding end the first page.
    const Placed old_fn = arena.place_at(0x1000, Bytes(8, 0xCC), cl_release(0), Bytes(8, 0xCC));
    const Placed new_fn = arena.place_padded(lea_ret(100), 8, 8, 0xCC, 0xCC, 0);
    REQUIRE(new_fn.entry > old_fn.entry);
    arena.seal();
    DWORD old_protection = 0;
    REQUIRE(VirtualProtect(arena.base(), 0x1000, PAGE_READONLY, &old_protection));  // readable, not executable
    REQUIRE((protection_of(old_fn.entry - 8) & PAGE_EXECUTE_READ) == 0);
    REQUIRE(protection_of(old_fn.entry) == PAGE_EXECUTE_READ);
    const Bytes before = around(old_fn);

    Installer installer;
    HookOutcome outcome;
    const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "split", &outcome);
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    CHECK(outcome.strategy == Strategy::FiveByte);
    CHECK(old_fn.entry[0] == 0xE9);
    CHECK(old_fn.entry[-1] == 0xCC);  // nothing written into the data page
    CHECK(protection_of(arena.base()) == PAGE_READONLY);
    CHECK(old_fn.as_int()(1) == 101);
    {
        const BatchResult checked = revert_one(installer, old_fn.entry, "split");
        REQUIRE_MESSAGE(checked.ok(), checked.diag.format());
    }
    CHECK(around(old_fn) == before);
    CHECK(protection_of(arena.base()) == PAGE_READONLY);
}

#endif  // _WIN32
