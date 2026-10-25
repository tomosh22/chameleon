// WP-10 acceptance: unit tests hooking functions in the test host under heavy multithreaded load.
//
// 16 threads spin through nine hooked functions (two compiled by this cell's compiler, seven
// hand-assembled with different entry shapes: padding or none, first instruction one byte or more,
// shorter than 5 bytes, one that loops inside its own first bytes so that threads are often caught
// inside a window) while the main thread repeatedly installs, re-points and reverts all nine hooks in
// one batch each, for 60 seconds. A further thread churns the process heap, the loader lock and
// thread creation, so that a stopped thread is regularly holding exactly the locks the installer
// must not need. Every result a worker sees must be the original, the first or the second
// implementation (anything else is a corrupted call), the main thread checks all nine functions
// after every batch, and the process must not take a single hardware exception.
//
// Output (also written to <build>/test_work/agent_hook/evidence.txt for the CI log):
//   WP10-STRESS seconds=60 threads=16 installs=N reverts=N retries=N crashes=0
//   WP10-STRESS-DETAIL ...
#if defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "alloc_guard.h"
#include "crash_report.h"
#include "doctest.h"
#include "hook/diag_ids.h"
#include "hook/installer.h"
#include "hook_test_support.h"

using namespace cml::agent::hook;
using namespace hooktest;

namespace {

constexpr int kWorkerThreads = 16;

volatile int g_zero = 0;
HOOKTEST_NOINLINE int n0_orig(int x) { return x + 1000 + g_zero; }
HOOKTEST_NOINLINE int n0_a(int x) { return x + 2000 + g_zero; }
HOOKTEST_NOINLINE int n0_b(int x) { return x + 3000 + g_zero; }
HOOKTEST_NOINLINE int n1_orig(int x) { return x + 1100 + g_zero; }
HOOKTEST_NOINLINE int n1_a(int x) { return x + 2100 + g_zero; }
HOOKTEST_NOINLINE int n1_b(int x) { return x + 3100 + g_zero; }

template <class Function>
const void* address_of(Function function) {
    return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(function));
}

struct Target {
    const char* name = "";
    const void* entry = nullptr;
    const void* replacement[2] = {nullptr, nullptr};
    // expected result for implementation 0 (original), 1 (A), 2 (B): arg + offset, or offset only
    bool ignores_argument = false;
    int offset[3] = {0, 0, 0};
    int argument_override = -1;  // >= 0: always call with this argument
    Bytes snapshot_before;
};

int expected(const Target& t, int which, int argument) {
    return t.ignores_argument ? t.offset[which] : argument + t.offset[which];
}

// Which implementation produced `result` (0 original, 1 A, 2 B), or -1 when none can have.
int classify(const Target& t, int argument, int result) {
    for (int which = 0; which < 3; ++which) {
        if (expected(t, which, argument) == result) {
            return which;
        }
    }
    return -1;
}

struct WorkerStats {
    std::uint64_t calls[3] = {0, 0, 0};
    std::uint64_t bad = 0;
    int last_bad_target = -1;
    int last_bad_result = 0;
    char padding[64] = {};  // keeps neighbouring workers off one cache line
};

int call_target(const void* entry, int argument) {
    return reinterpret_cast<IntFn>(reinterpret_cast<std::uintptr_t>(entry))(argument);
}

std::atomic<long> g_crashes{0};

LONG CALLBACK count_crashes(PEXCEPTION_POINTERS info) {
    switch (info->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_BREAKPOINT:  // an int3 padding byte was executed
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_GUARD_PAGE:
            ++g_crashes;
            std::fprintf(stderr, "WP10-STRESS crash: exception 0x%08lx at %p\n",
                         static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                         info->ExceptionRecord->ExceptionAddress);
            std::fflush(stderr);
            break;
        default:
            break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int configured_seconds() {
    char text[16];
    const DWORD n = GetEnvironmentVariableA("CML_HOOK_STRESS_SECONDS", text, sizeof(text));
    if (n > 0 && n < sizeof(text)) {
        const int value = std::atoi(text);
        if (value > 0) {
            return value;
        }
    }
    return 60;
}

void start_evidence_file() {
#if defined(CML_TEST_WORK_DIR)
    DeleteFileA(CML_TEST_WORK_DIR "\\evidence.txt");
#endif
}

void write_evidence(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
#if defined(CML_TEST_WORK_DIR)
    CreateDirectoryA(CML_TEST_WORK_DIR, nullptr);
    HANDLE file = CreateFileA(CML_TEST_WORK_DIR "\\evidence.txt", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const std::string text = line + "\n";
        DWORD written = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(file);
    }
#endif
}

}  // namespace

TEST_CASE("WP10 stress: hooks installed, re-pointed and reverted under heavy multithreaded load") {
    const int seconds = configured_seconds();
    start_evidence_file();
    Arena arena(16);
    REQUIRE(arena.ok());

    std::vector<Target> targets;
    auto add_blob = [&](const char* name, const Bytes& code, std::size_t pad_before, std::size_t pad_after,
                        std::int32_t a_offset, std::int32_t b_offset) {
        Target t;
        t.name = name;
        const Placed old_fn = arena.place_padded(code, pad_before, pad_after);
        const Placed a = arena.place_padded(lea_ret(a_offset), 8, 8);
        const Placed b = arena.place_padded(lea_ret(b_offset), 8, 8);
        t.entry = old_fn.entry;
        t.replacement[0] = a.entry;
        t.replacement[1] = b.entry;
        t.offset[0] = 0;
        t.offset[1] = a_offset;
        t.offset[2] = b_offset;
        targets.push_back(t);
    };
    {
        Target n0;
        n0.name = "natural_0";
        n0.entry = address_of(&n0_orig);
        n0.replacement[0] = address_of(&n0_a);
        n0.replacement[1] = address_of(&n0_b);
        n0.offset[0] = 1000;
        n0.offset[1] = 2000;
        n0.offset[2] = 3000;
        targets.push_back(n0);
        Target n1;
        n1.name = "natural_1";
        n1.entry = address_of(&n1_orig);
        n1.replacement[0] = address_of(&n1_a);
        n1.replacement[1] = address_of(&n1_b);
        n1.offset[0] = 1100;
        n1.offset[1] = 2100;
        n1.offset[2] = 3100;
        targets.push_back(n1);
    }
    add_blob("cl_release_padded", cl_release(0), 8, 8, 10, 20);       // strategy 1, first instruction 4 bytes
    add_blob("cl_debug_padded", cl_debug(0), 7, 8, 11, 21);           // strategy 1, first instruction 2 bytes (REX push)
    add_blob("lea_ret_no_padding", lea_ret(0), 0, 0, 12, 22);         // strategy 2, no padding anywhere
    add_blob("push_first_padded", push_first(0), 16, 16, 13, 23);     // padding, but first instruction 1 byte: strategy 2
    add_blob("short_padded", short_lea(0), 8, 8, 14, 24);             // 4 bytes, padding both sides: strategy 1
    add_blob("short_trailing_pad", short_lea(0), 0, 12, 15, 25);      // 4 bytes, padding only after: strategy 2, short function
    {
        // Loops inside its first four bytes for `argument` iterations, then returns a constant.
        Target slow;
        slow.name = "slow_loop";
        const Placed old_fn = arena.place_padded(slow_const(7), 0, 0);
        const Placed a = arena.place_padded(const_ret(70001), 8, 8);
        const Placed b = arena.place_padded(const_ret(70002), 8, 8);
        slow.entry = old_fn.entry;
        slow.replacement[0] = a.entry;
        slow.replacement[1] = b.entry;
        slow.ignores_argument = true;
        slow.offset[0] = 7;
        slow.offset[1] = 70001;
        slow.offset[2] = 70002;
        slow.argument_override = 150;
        targets.push_back(slow);
    }
    arena.seal();
    for (Target& t : targets) {
        t.snapshot_before = snapshot(static_cast<const std::uint8_t*>(t.entry) - 16, 16 + 48);
    }

    // The functions behave as the originals before anything is hooked.
    for (const Target& t : targets) {
        const int argument = t.argument_override >= 0 ? t.argument_override : 5;
        REQUIRE_MESSAGE(call_target(t.entry, argument) == expected(t, 0, argument), t.name);
    }

    void* const veh = AddVectoredExceptionHandler(1, &count_crashes);
    g_crashes = 0;

    std::atomic<bool> stop{false};
    std::atomic<int> running{0};
    std::vector<WorkerStats> stats(kWorkerThreads + 1);  // the last one belongs to the pinned thread
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkerThreads; ++w) {
        workers.emplace_back([&, w] {
            WorkerStats& mine = stats[static_cast<std::size_t>(w)];
            int x = w * 1000;
            ++running;
            while (!stop.load(std::memory_order_relaxed)) {
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    const Target& t = targets[i];
                    const int argument = t.argument_override >= 0 ? t.argument_override : (x & 0xFFFF);
                    const int result = call_target(t.entry, argument);
                    const int which = classify(t, argument, result);
                    if (which < 0) {
                        ++mine.bad;
                        mine.last_bad_target = static_cast<int>(i);
                        mine.last_bad_result = result;
                    } else {
                        ++mine.calls[which];
                    }
                }
                ++x;
                // Let the other runnable threads in now and then: with 16 spinners on a few processors a
                // thread that is never descheduled for long stops the world slowly (the suspension of a
                // thread takes effect when it next runs).
                if ((x & 0x3F) == 0) {
                    SwitchToThread();
                }
            }
        });
    }

    // The pinned thread. It calls the slow function with an argument that keeps it inside the first bytes of
    // the function (a dec/jnz loop of about a millisecond) while the function is not hooked, with a 1 ms gap between calls. An install that follows a revert therefore finds this thread inside
    // a window in a good share of the cycles, so the RIP-in-window path is exercised in every cycle, whatever the other 16
    // threads (which only pass through the slow function for ~150 iterations between other calls) happen to
    // be doing on this machine.
    std::atomic<long long> pinned_calls{0};
    workers.emplace_back([&] {
        WorkerStats& mine = stats[kWorkerThreads];
        const Target& t = targets.back();
        ++running;
        while (!stop.load(std::memory_order_relaxed)) {
            const int result = call_target(t.entry, 3000000);
            const int which = classify(t, 3000000, result);
            if (which < 0) {
                ++mine.bad;
                mine.last_bad_target = static_cast<int>(targets.size()) - 1;
                mine.last_bad_result = result;
            } else {
                ++mine.calls[which];
            }
            ++pinned_calls;
            Sleep(1);  // a gap outside the function, so that the installer can win: a function that is never
                       // left is a legitimate CML4305, and the stress run must still make progress
        }
    });

    // Heap, loader lock and thread creation churn: threads that get stopped holding those locks.
    std::atomic<long long> churn_rounds{0};
    std::thread churn([&] {
        std::uint32_t seed = 12345;
        while (!stop.load(std::memory_order_relaxed)) {
            seed = seed * 1664525u + 1013904223u;
            void* block = std::malloc(32 + (seed >> 20) % 4096);
            std::free(block);
            std::string text(100 + (seed >> 16) % 900, 'x');
            if (text.size() == 3) {
                break;
            }
            void* heap_block = HeapAlloc(GetProcessHeap(), 0, 64 + (seed >> 12) % 512);
            HeapFree(GetProcessHeap(), 0, heap_block);
            if ((churn_rounds.load() & 7) == 0) {
                if (HMODULE module = LoadLibraryW(L"version.dll")) {
                    FreeLibrary(module);
                }
            }
#if !defined(CML_HOOK_NO_THREAD_CHURN)  // local Wine smoke runs only: Wine hangs on thread creation under suspension
            if ((churn_rounds.load() & 31) == 0) {
                HANDLE h = CreateThread(
                    nullptr, 0, [](LPVOID) -> DWORD { return 0; }, nullptr, 0, nullptr);
                if (h != nullptr) {
                    WaitForSingleObject(h, INFINITE);  // one short-lived thread at a time, not a backlog
                    CloseHandle(h);
                }
            }
#endif
            ++churn_rounds;
        }
    });

    while (running.load() < kWorkerThreads) {
        std::this_thread::yield();
    }
    while (pinned_calls.load() < 20) {
        std::this_thread::yield();
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // ---- the installer's loop --------------------------------------------------------------------
    Installer installer;
    REQUIRE(installer.ready());
    std::vector<HookRequest> install_a, install_b, revert_all;
    for (const Target& t : targets) {
        install_a.push_back({t.entry, t.replacement[0], t.name});
        install_b.push_back({t.entry, t.replacement[1], t.name});
        revert_all.push_back({t.entry, nullptr, t.name});
    }
    std::vector<HookOutcome> outcomes;
    unsigned long long installs = 0, reverts = 0, retries = 0, attempts = 0, failures = 0, cycles = 0;
    unsigned max_window_ms = 0, max_threads = 0;
    unsigned long long wrong_state = 0;
    std::vector<std::string> failure_texts;
    std::uint32_t unexpected_failure_code = 0;
    std::string unexpected_failure_text;

    auto account = [&](const BatchResult& r) {
        retries += r.stats.retries;
        attempts += r.stats.attempts;
        max_window_ms = std::max(max_window_ms, r.stats.window_ms);
        max_threads = std::max(max_threads, r.stats.threads);
    };
    // After a batch the main thread calls every function itself: the state must be exactly what the
    // batch says, whatever the 16 other threads are doing.
    auto verify_state = [&](int which) {
        for (const Target& t : targets) {
            const int argument = t.argument_override >= 0 ? t.argument_override : 3;
            if (call_target(t.entry, argument) != expected(t, which, argument)) {
                ++wrong_state;
            }
        }
    };
    auto note_failure = [&](const BatchResult& r) {
        ++failures;
        const std::uint32_t code = r.diag.code;
        // Clean refusals under load: a thread would not leave a window in time, or the world could
        // not be stopped in time. Anything else is a bug.
        if (code != ids::kThreadInWindow && code != ids::kWindowTimeout && code != ids::kThreadsNotConverging &&
            unexpected_failure_code == 0) {
            unexpected_failure_code = code;
            unexpected_failure_text = r.diag.format();
        }
        if (failure_texts.size() < 3) {
            failure_texts.push_back(r.diag.format());
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        ++cycles;
        for (int phase = 0; phase < 3; ++phase) {
            if (phase < 2) {
                const std::vector<HookRequest>& request = phase == 0 ? install_a : install_b;
                const BatchResult r = installer.install(request.data(), request.size(), outcomes);
                account(r);
                if (r.ok()) {
                    ++installs;
                    verify_state(phase + 1);
                } else {
                    note_failure(r);
                }
            } else if (installer.hook_count() > 0) {
                const BatchResult r = installer.revert(revert_all.data(), revert_all.size(), outcomes);
                account(r);
                if (r.ok()) {
                    ++reverts;
                    verify_state(0);
                } else {
                    note_failure(r);
                }
            }
            Sleep(1);
        }
    }
    // Whatever is still hooked goes back.
    if (installer.hook_count() > 0) {
        for (int tries = 0; tries < 20 && installer.hook_count() > 0; ++tries) {
            const BatchResult r = installer.revert(revert_all.data(), revert_all.size(), outcomes);
            account(r);
            if (r.ok()) {
                ++reverts;
            }
        }
    }

    stop = true;
    for (std::thread& w : workers) {
        w.join();
    }
    churn.join();
    RemoveVectoredExceptionHandler(veh);

    unsigned long long calls[3] = {0, 0, 0}, bad = 0;
    int bad_target = -1, bad_result = 0;
    for (const WorkerStats& s : stats) {
        for (int i = 0; i < 3; ++i) {
            calls[i] += s.calls[i];
        }
        bad += s.bad;
        if (s.bad != 0) {
            bad_target = s.last_bad_target;
            bad_result = s.last_bad_result;
        }
    }

    write_evidence("WP10-STRESS seconds=" + std::to_string(seconds) + " threads=" + std::to_string(kWorkerThreads) +
                   " installs=" + std::to_string(installs) + " reverts=" + std::to_string(reverts) +
                   " retries=" + std::to_string(retries) + " crashes=" + std::to_string(g_crashes.load()));
    write_evidence("WP10-STRESS-DETAIL cycles=" + std::to_string(cycles) + " hooks_per_batch=" +
                   std::to_string(targets.size()) + " stops=" + std::to_string(attempts) +
                   " refused=" + std::to_string(failures) + " calls_original=" + std::to_string(calls[0]) +
                   " calls_new_a=" + std::to_string(calls[1]) + " calls_new_b=" + std::to_string(calls[2]) +
                   " bad_results=" + std::to_string(bad) + " wrong_state=" + std::to_string(wrong_state) +
                   " max_window_ms=" + std::to_string(max_window_ms) + " max_threads=" + std::to_string(max_threads) +
                   " churn_rounds=" + std::to_string(churn_rounds.load()));
    for (const std::string& text : failure_texts) {
        write_evidence("WP10-STRESS-REFUSED " + text);
    }

    CHECK(g_crashes.load() == 0);
    CHECK_MESSAGE(bad == 0, "target " << (bad_target >= 0 ? targets[static_cast<std::size_t>(bad_target)].name : "?")
                                      << " returned " << bad_result);
    CHECK(wrong_state == 0);
    CHECK_MESSAGE(unexpected_failure_code == 0, unexpected_failure_text);
    CHECK(installs > 0);
    CHECK(reverts > 0);
    CHECK_MESSAGE(retries >= 1, "the RIP-in-window path was never taken");
    CHECK(calls[0] > 0);
    CHECK(calls[1] > 0);
    CHECK(calls[2] > 0);
    // Refusals are allowed, a majority of them is not.
    CHECK(failures * 4 <= cycles * 3 + 3);
    CHECK(installer.hook_count() == 0);
    for (const Target& t : targets) {
        const Bytes now = snapshot(static_cast<const std::uint8_t*>(t.entry) - 16, 16 + 48);
        CHECK_MESSAGE(now == t.snapshot_before, t.name << ": bytes differ from the original after the last revert");
    }
}

#endif  // _WIN32
