#pragma once

// The hook installer (plan section 4.8, WP-10): redirects the entry of a running function to new
// code, re-points an existing redirection, and puts the original bytes back.
//
// Every batch (one InstallHooks request) is all or nothing. It is applied like this:
//
//   1. Plan, with all threads running: read the bytes around each entry (ReadProcessMemory, which
//      fails with an error instead of faulting), choose a strategy (hook_plan.h), encode the jump,
//      check it reaches (+-2 GB), reject duplicates and overlapping regions. Any failure ends the
//      batch here, before a thread has been touched. This is also where everything that allocates
//      happens (outcome strings, the job table, the registry's spare capacity).
//   2. VirtualProtect the pages that will be written to PAGE_EXECUTE_READWRITE (kept out of the
//      window on purpose: it is one more system call that does not have to run with the world stopped).
//   3. Stop the world: ThreadSet::suspend_all (convergence loop), then compare every thread's RIP
//      with the window of every hook. Window = from the first byte the hook writes to the END of the
//      last instruction it touches (hook_plan.h). A thread inside a window means the install cannot
//      be proven safe now: the offender alone is let run for a moment and checked again (up to
//      steps_per_attempt times while everything else stays stopped), then everything is resumed, the
//      installer yields and tries again, up to max_attempts times in total (plan: 50), then the
//      batch fails with CML4305 naming the thread and the function.
//   4. With no thread in any window: check the bytes are still the ones planned for, write, flush the
//      instruction cache, resume everything. Strategy 1 writes the padding jump first and then the
//      2 bytes at the entry in one 16-bit interlocked exchange; re-pointing writes only the 4-byte
//      displacement (one interlocked 64-bit compare-exchange when it lies in an aligned 8-byte
//      block).
//   5. Restore page protection and update the registry (original bytes of the FIRST hook are kept
//      for revert; re-pointing never overwrites them).
//
// What runs while threads are stopped (steps 3 and 4) allocates nothing, logs nothing and takes no
// lock: see thread_set.h for the system calls it makes, and heap::set_alloc_forbidden(true) is set
// around the part that reads and writes code so that any allocation by this code aborts the process
// in tests instead of deadlocking in the field.

#if defined(_WIN32)

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "cml/util/diag.h"
#include "hook_plan.h"
#include "thread_set.h"

namespace cml::agent::hook {

struct HookRequest {
    const void* target = nullptr;       // old entry (the function being replaced)
    const void* replacement = nullptr;  // new entry (ignored for revert)
    const char* name = nullptr;         // for diagnostics; may be null
};

enum class HookStatus : std::uint8_t {
    Installed = 0,     // a new hook was written
    Repointed = 1,     // the existing hook now jumps to the new replacement
    Reverted = 2,      // the original bytes are back
    Failed = 3,        // this hook is the reason the batch failed
    NotAttempted = 4,  // the batch failed before / because of another hook
    Unchanged = 5,     // already hooked to this very replacement; nothing was written
};

struct HookOutcome {
    Strategy strategy = Strategy::None;
    HookStatus status = HookStatus::NotAttempted;
    Diag diag;  // code 0 when there is none
};

struct BatchStats {
    unsigned attempts = 0;         // times the world was stopped
    unsigned retries = 0;          // times a thread was found inside a window (resolved by stepping or by a retry)
    unsigned threads = 0;          // threads stopped in the largest window
    unsigned passes = 0;           // enumeration passes of the largest window
    unsigned window_ms = 0;        // longest time the world was stopped
};

struct BatchResult {
    Diag diag;  // code 0 = every hook of the batch was applied
    BatchStats stats;
    bool ok() const noexcept { return diag.code == 0; }
};

struct InstallerOptions {
    unsigned max_attempts = 50;        // times the world may be stopped for one batch (plan: retry max 50)
    unsigned steps_per_attempt = 3;    // times per stop that only the offender runs before everything is resumed
    unsigned offender_run_us = 1000;   // how long the offender runs alone in each such step
    unsigned window_budget_ms = 2000;  // longest the world may stay stopped in one attempt
    unsigned max_passes = 16;          // enumeration passes before "threads keep appearing"
    std::size_t max_threads = 4096;    // capacity of the pre-sized thread table
    std::size_t max_hooks_per_request = 16384;

    // Test hooks. `test_fake_new_thread_passes`: the first n enumeration passes of every attempt
    // report a new thread. `test_in_window`: called with the world stopped, immediately before the
    // bytes are written; it must not allocate or take locks.
    unsigned test_fake_new_thread_passes = 0;
    // Tests only: the deadline is reported as passed at the n+1-th check inside the passes (0 = off);
    // and the enumeration step budget (0 = default). See ThreadSet::set_test_knobs.
    unsigned test_expire_after_checks = 0;
    std::size_t test_step_limit = 0;
    void (*test_in_window)(void* context) = nullptr;
    void* test_context = nullptr;
};

struct HookInfo {
    Strategy strategy = Strategy::None;
    const void* replacement = nullptr;
    std::uint8_t original_entry[5] = {};  // what the first hook replaced at the entry
    std::uint8_t original_padding[5] = {};
    std::uint32_t entry_length = 0;       // valid bytes in original_entry (2 or 5)
};

class Installer {
public:
    explicit Installer(const InstallerOptions& options = {});
    ~Installer();
    Installer(const Installer&) = delete;
    Installer& operator=(const Installer&) = delete;

    // Hooks every request (or re-points the ones that are already hooked). Safe from any thread;
    // concurrent batches are serialised. `outcomes` is resized to `count`.
    BatchResult install(const HookRequest* requests, std::size_t count, std::vector<HookOutcome>& outcomes);
    // Puts the original bytes back (requests[i].replacement is ignored).
    BatchResult revert(const HookRequest* requests, std::size_t count, std::vector<HookOutcome>& outcomes);

    bool ready() const noexcept { return ready_; }
    std::size_t hook_count() const;
    bool find(const void* target, HookInfo* info) const;

    // Test hook: replaces the options (thread capacity unchanged).
    void set_test_options(const InstallerOptions& options);

private:
    // One installed hook. Sorted by `target` in records_.
    struct Record {
        std::uintptr_t target = 0;
        std::uintptr_t replacement = 0;
        Strategy strategy = Strategy::None;
        std::uint8_t entry_length = 0;    // bytes written at the entry: 2 or 5
        std::uint8_t padding_length = 0;  // bytes written before the entry: 5 or 0
        std::uint8_t original_padding[5] = {};
        std::uint8_t current_padding[5] = {};
        std::uint8_t original_entry[5] = {};
        std::uint8_t current_entry[5] = {};
        std::uint32_t window_before = 0;
        std::uint32_t window_after = 0;
        char name[48] = {};
    };
    struct Job;
    struct Report;

    BatchResult apply(bool revert, const HookRequest* requests, std::size_t count, std::vector<HookOutcome>& outcomes);
    void attempt(Job* jobs, std::size_t count, unsigned* conflicts_seen, Report* report) noexcept;
    bool find_conflict(const Job* jobs, std::size_t count, std::size_t* thread_index, std::size_t* job_index) const noexcept;
    Record* find_record(std::uintptr_t target);
    const Record* find_record(std::uintptr_t target) const;

    InstallerOptions options_;
    bool ready_ = false;
    mutable std::mutex mutex_;  // serialises batches and guards records_
    ThreadSet threads_;
    std::vector<Record> records_;  // sorted by target
};

// The process-wide installer used by the agent: hooks are process state, so every agent in the
// process shares one registry. Created on first use, never destroyed.
Installer& process_installer();

}  // namespace cml::agent::hook

#endif  // _WIN32
