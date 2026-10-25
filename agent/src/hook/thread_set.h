#pragma once

// Suspending every other thread of the process (plan section 4.8, "Procedure").
//
// Everything between the first SuspendThread and the last ResumeThread runs while threads that may
// hold ANY lock (the process heap lock, the loader lock, the CRT locks, the agent's own mutexes)
// are stopped. Code in that window must therefore not allocate, log, take a lock or call into the
// loader. ThreadSet is built for that:
//   * init() is the only member that allocates (and resolves ntdll!NtGetNextThread); call it before
//     any thread is suspended. All storage is pre-sized: a process with more threads than the
//     capacity is refused (ThreadSet::Result::TooManyThreads), never grown.
//   * Threads are enumerated with ntdll!NtGetNextThread, which walks the kernel's thread list for the
//     process and hands out handles one at a time. A toolhelp snapshot is not used: it allocates from
//     the process heap in user mode, which a suspended thread may be holding.
//   * The APIs used inside the window are thin system-call wrappers: NtGetNextThread, GetThreadId,
//     SuspendThread, GetThreadContext(CONTEXT_CONTROL), ResumeThread, GetExitCodeThread, CloseHandle,
//     SwitchToThread and GetTickCount64 (reads the shared user data page). None of them takes a
//     user-mode lock or allocates; this is established by how the Windows system DLLs implement them
//     (they forward to the kernel), not by a test, and docs/wp/WP-10.md says so.
//
// Latency: SuspendThread only queues the suspension; the target stops when it next runs, and
// GetThreadContext is what waits for that. With more runnable threads than processors, waiting for
// each thread right after suspending it costs a scheduling delay per thread, one after the other.
// So a pass first suspends every thread it finds (their delays overlap) and only then reads the
// contexts. A thread whose suspension is queued can still run until its context has been read, so
// "no new thread" is only concluded from a pass that follows a completed context read.
//
// Convergence: suspend_all() repeats the enumeration until a complete pass finds no thread it has
// not already suspended. A thread that appears after the last pass can still start running; the
// installer's write is built so that is survivable (see installer.h), and the bounded number of
// passes and the deadline make "threads keep appearing" a clean failure (CML4307) rather than a hang.

#if defined(_WIN32)

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace cml::agent::hook {

class ThreadSet {
public:
    enum class Result : std::uint8_t {
        Ok,
        EnumFailed,        // NtGetNextThread is unavailable or failed
        TooManyThreads,    // more threads than the pre-sized capacity
        NotConverging,     // new threads kept appearing for the allowed number of passes
        SuspendFailed,     // a live thread could not be suspended or its context read
        Timeout,           // the deadline passed
    };

    struct Entry {
        HANDLE handle = nullptr;
        DWORD id = 0;
        std::uint64_t rip = 0;  // valid when suspended and rip_valid
        bool suspended = false;
        bool rip_valid = false;
    };

    ThreadSet() = default;
    ~ThreadSet();
    ThreadSet(const ThreadSet&) = delete;
    ThreadSet& operator=(const ThreadSet&) = delete;

    // NOT window-safe. Allocates room for `capacity` threads and resolves NtGetNextThread.
    bool init(std::size_t capacity);
    bool ready() const noexcept { return entries_ != nullptr && get_next_thread_ != nullptr; }
    std::size_t capacity() const noexcept { return capacity_; }

    // ---- window-safe from here on -------------------------------------------------------------
    // Suspends every thread of the process except the caller, repeating until a pass finds nothing
    // new. `fake_new_passes` (tests only) makes the first n passes report a new thread so that the
    // "never converges" path can be exercised deterministically.
    Result suspend_all(ULONGLONG deadline_tick, unsigned max_passes, unsigned fake_new_passes = 0) noexcept;

    std::size_t size() const noexcept { return count_; }
    const Entry& entry(std::size_t index) const noexcept { return entries_[index]; }
    unsigned passes() const noexcept { return passes_; }

    // Lets one thread run (it stays in the set) / stops it again and refreshes its RIP.
    void resume_one(std::size_t index) noexcept;
    bool suspend_one(std::size_t index) noexcept;

    // Resumes every suspended thread and releases all handles; the set is empty afterwards.
    void resume_all() noexcept;

    // Details of the last failure, for the diagnostic.
    DWORD failed_thread_id() const noexcept { return failed_thread_id_; }
    DWORD failed_error() const noexcept { return failed_error_; }

private:
    using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes,
                                           ULONG flags, PHANDLE new_thread);

    Result enumerate_pass(std::size_t* fresh) noexcept;
    bool known(DWORD id) const noexcept;
    bool suspend_only(std::size_t index) noexcept;       // SuspendThread; the thread may still run a little
    bool capture_context(std::size_t index) noexcept;    // GetThreadContext: returns once it has really stopped
    bool suspend_entry(std::size_t index) noexcept;      // both, one thread
    bool capture_all() noexcept;                         // contexts of every suspended thread that has none yet
    bool exited(HANDLE thread) const noexcept;

    Entry* entries_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t count_ = 0;
    unsigned passes_ = 0;
    DWORD self_id_ = 0;
    DWORD failed_thread_id_ = 0;
    DWORD failed_error_ = 0;
    NtGetNextThreadFn get_next_thread_ = nullptr;
};

}  // namespace cml::agent::hook

#endif  // _WIN32
