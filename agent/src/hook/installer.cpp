#include "installer.h"

#if defined(_WIN32)

#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include "cml/util/log.h"
#include "diag_ids.h"
#include "fault/fault_injection.h"
#include "heap.h"

namespace cml::agent::hook {

namespace {

constexpr std::uintptr_t kPageSize = 0x1000;  // x64
constexpr unsigned kYieldOnlyRetries = 4;     // retries that only yield; later ones sleep 1 ms

// WP-50: the performance counter (window-safe: no lock, no allocation) and its conversion, done after the window.
inline long long ticks() noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

std::uint64_t to_us(long long elapsed) noexcept {
    static const long long frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    if (elapsed <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(elapsed / frequency * 1000000 + (elapsed % frequency) * 1000000 / frequency);
}

// One contiguous store into code.
struct Write {
    std::uint8_t* address = nullptr;
    std::uint8_t length = 0;
    std::uint8_t expect[8] = {};  // bytes that must be there right before the store
    std::uint8_t data[8] = {};    // bytes to store
};

enum class Op : std::uint8_t { Install, Repoint, Unchanged, Revert };

enum class BadArgument : std::uint8_t { Null, Duplicate, SameFunction, NotCode };

// Why a job failed, as plain data: the Diag text is built after the world is running again.
struct Failure {
    std::uint32_t code = 0;
    std::uint32_t offset = 0;  // 4300 / 4301 / 4304: offset of the instruction from the entry
    std::uint8_t bytes[4] = {};
    std::uint8_t byte_count = 0;
    std::uint64_t a = 0;  // 4302: replacement address; 4305: RIP
    std::uint64_t b = 0;  // 4302: jump site
    std::uint32_t thread_id = 0;
    std::uint32_t win_error = 0;
    std::uint32_t conflicts = 0;
    BadArgument reason = BadArgument::Null;  // 4310 only
};

std::string format(const char* text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    std::vsnprintf(buffer, sizeof(buffer), text, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
}

std::string hex_bytes(const std::uint8_t* bytes, std::size_t count) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            s += ' ';
        }
        s += kDigits[bytes[i] >> 4];
        s += kDigits[bytes[i] & 15];
    }
    return s;
}

const char* display_name(const HookRequest& request) {
    return (request.name != nullptr && request.name[0] != '\0') ? request.name : "<unnamed function>";
}

Diag make_diag(const Failure& f, const HookRequest& request) {
    const char* name = display_name(request);
    const auto target = reinterpret_cast<std::uintptr_t>(request.target);
    switch (f.code) {
        case ids::kDecodeUnknown:
            return Diag::make(f.code,
                              format("cannot hook %s: the instruction at %s+%u (bytes %s) is not one the hook installer "
                                     "understands, so it cannot tell where the instruction ends.",
                                     name, name, f.offset, hex_bytes(f.bytes, f.byte_count).c_str()),
                              "Report the function's first bytes as a Chameleon bug, or exclude this function from "
                              "hot reload; compiling without /arch:AVX or similar options usually avoids it.");
        case ids::kFunctionTooShort:
            return Diag::make(f.code,
                              format("cannot hook %s: the function is shorter than the 5-byte jump and neither "
                                     "alignment padding before it nor after it can be used.",
                                     name),
                              "Link with /FUNCTIONPADMIN (padding before every function) or give the function a body "
                              "of at least 5 bytes.");
        case ids::kOutOfRange:
            return Diag::make(f.code,
                              format("cannot hook %s: the new code at 0x%llx is more than 2 GB from the jump at "
                                     "0x%llx.",
                                     name, static_cast<unsigned long long>(f.a), static_cast<unsigned long long>(f.b)),
                              "Report this as a Chameleon bug: patch code must be loaded within 2 GB of the module "
                              "it patches.");
        case ids::kCodeUnreadable:
            return Diag::make(f.code,
                              format("cannot hook %s: the code at 0x%llx cannot be read (not mapped, not executable, "
                                     "or it ends inside an instruction).",
                                     name, static_cast<unsigned long long>(target)),
                              "Check that the address is the entry of a function in a loaded module.");
        case ids::kCallInWindow:
            return Diag::make(f.code,
                              format("cannot hook %s: a call instruction ends at %s+%u, inside the bytes the hook "
                                     "overwrites, so a thread returning from that call would run overwritten code.",
                                     name, name, f.offset),
                              "Exclude this function from hot reload or add padding before it (/FUNCTIONPADMIN).");
        case ids::kThreadInWindow:
            return Diag::make(f.code,
                              format("cannot hook %s: thread %u kept executing inside its first bytes (RIP=%s+%llu) "
                                     "through %u attempts.",
                                     name, f.thread_id, name,
                                     static_cast<unsigned long long>(f.a - target), f.conflicts),
                              "Wait for the application to leave the function (a long-running call in it blocks the "
                              "patch) and reload again.");
        case ids::kBadArgument:
        {
            const char* why = "an address in the request is null.";
            switch (f.reason) {
                case BadArgument::Null: break;
                case BadArgument::Duplicate: why = "it is listed twice in the request."; break;
                case BadArgument::SameFunction: why = "the replacement is the function itself."; break;
                case BadArgument::NotCode: why = "the replacement is not executable code."; break;
            }
            return Diag::make(f.code, format("cannot hook %s: %s", name, why),
                              "Report this as a Chameleon bug (the broker sent a bad InstallHooks request).");
        }
        case ids::kNotInstalled:
            return Diag::make(f.code, format("cannot revert %s: it has no hook installed.", name),
                              "Nothing to do; the function already runs its original code.");
        case ids::kCodeChanged:
            return Diag::make(f.code,
                              format("cannot change the hook of %s: its bytes are not the ones recorded (something "
                                     "else modified the code).",
                                     name),
                              "Remove other tools that patch this function (hooks, instrumentation) and try again.");
        case ids::kWriteFailed:
            return Diag::make(f.code,
                              format("cannot hook %s: VirtualProtect on its code failed (Win32 error %u).", name,
                                     f.win_error),
                              "Check that the module's code pages can be made writable (no hardened code-integrity "
                              "policy such as ACG).");
        case ids::kOverlap:
            return Diag::make(f.code,
                              format("cannot hook %s: the bytes the hook overwrites overlap another hook.", name),
                              "Report this as a Chameleon bug unless two tiny functions are placed back to back.");
        default:
            return Diag::make(f.code, format("cannot hook %s (internal code %u).", name, f.code), "Report this as a bug.");
    }
}

// Whether memory with these attributes holds code we may read (and hook).
bool is_code_region(const MEMORY_BASIC_INFORMATION& info) noexcept {
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// WP-50 (R-47): the answers of VirtualQuery during the planning of one batch. Every page of the range VirtualQuery reports
// ([BaseAddress, BaseAddress + RegionSize)) has the same attributes, so an address inside a range already reported gets the
// same answer without another call. The first query into the large code region of a module that was never changed is
// expensive (it measures the whole region); a batch of new hooks asks three times per target. Lives in one planning pass
// (under the installer's mutex, before any VirtualProtect or write of the batch); a failed query is not remembered.
class RegionCache {
public:
    explicit RegionCache(bool enabled) : enabled_(enabled) {}
    bool is_code(const void* address, std::uint32_t* queries) {
        const auto a = reinterpret_cast<std::uintptr_t>(address);
        if (enabled_) {
            for (const Region& r : regions_) {
                if (a >= r.begin && a < r.end) {
                    return r.code;
                }
            }
        }
        MEMORY_BASIC_INFORMATION info;
        ++*queries;
        if (VirtualQuery(address, &info, sizeof(info)) == 0) {
            return false;
        }
        const bool code = is_code_region(info);
        if (enabled_) {
            const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            regions_.push_back(Region{begin, begin + info.RegionSize, code});
        }
        return code;
    }
    void clear() { regions_.clear(); }

private:
    struct Region {
        std::uintptr_t begin;
        std::uintptr_t end;
        bool code;
    };
    bool enabled_;
    std::vector<Region> regions_;
};

// ReadProcessMemory on the own process: fails with an error (and a count of what it did copy)
// instead of faulting when the range is not fully readable.
bool read_code(const void* address, void* buffer, std::size_t length, std::size_t* got) noexcept {
    SIZE_T copied = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), address, buffer, length, &copied);
    *got = static_cast<std::size_t>(copied);
    return ok != 0;
}

// One store, as atomic as the hardware allows. The world is stopped when this runs, so atomicity
// only matters for a thread the convergence loop missed; it keeps such a thread from ever seeing a
// half-written jump in the common cases:
//   2 bytes  -> 16-bit interlocked exchange (the entry of strategy 1);
//   <= 8 bytes inside one aligned 8-byte block (the 5-byte jump of strategy 2 when the entry is at
//               offset 0..3 of the block, and the 4-byte displacement of a re-point when it does not
//               straddle) -> 64-bit compare-exchange that merges the new bytes into the block;
//   otherwise plain byte stores.
void apply_write(const Write& w) noexcept {
    if (w.length == 2) {
        const auto value = static_cast<short>(static_cast<unsigned short>(w.data[0] | (w.data[1] << 8)));
#if defined(__GNUC__) && !defined(__clang__)
        // MinGW (used for local smoke builds only) has no _InterlockedExchange16.
        __atomic_exchange_n(reinterpret_cast<volatile short*>(w.address), value, __ATOMIC_SEQ_CST);
#else
        _InterlockedExchange16(reinterpret_cast<volatile short*>(w.address), value);
#endif
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(w.address);
    const std::uintptr_t block = address & ~static_cast<std::uintptr_t>(7);
    if (address + w.length <= block + 8) {
        auto* word = reinterpret_cast<volatile long long*>(block);
        long long seen = *word;
        for (;;) {
            long long wanted = seen;
            auto* bytes = reinterpret_cast<unsigned char*>(&wanted);
            for (std::size_t i = 0; i < w.length; ++i) {
                bytes[address - block + i] = w.data[i];
            }
            const long long before = _InterlockedCompareExchange64(word, wanted, seen);
            if (before == seen) {
                return;
            }
            seen = before;
        }
    }
    auto* destination = reinterpret_cast<volatile std::uint8_t*>(w.address);
    for (std::size_t i = 0; i < w.length; ++i) {
        destination[i] = w.data[i];
    }
}

bool bytes_equal(const std::uint8_t* address, const std::uint8_t* expected, std::size_t length) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        if (address[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

// Lets the offender run for about `microseconds` (it is the only thread that does). Yields so that
// it can be scheduled even on one processor; reads the performance counter, which is a plain
// register / shared-page read.
void let_run(unsigned microseconds) noexcept {
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const long long wanted = frequency.QuadPart * static_cast<long long>(microseconds) / 1000000;
    do {
        SwitchToThread();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < wanted);
}

std::uint32_t diag_for(ThreadSet::Result result) noexcept {
    switch (result) {
        case ThreadSet::Result::Ok: return 0;
        case ThreadSet::Result::EnumFailed: return ids::kThreadEnumFailed;
        case ThreadSet::Result::TooManyThreads: return ids::kThreadEnumFailed;
        case ThreadSet::Result::NotConverging: return ids::kThreadsNotConverging;
        case ThreadSet::Result::SuspendFailed: return ids::kSuspendFailed;
        case ThreadSet::Result::Timeout: return ids::kWindowTimeout;
    }
    return ids::kThreadEnumFailed;
}

Diag batch_diag(std::uint32_t code, ThreadSet::Result result, std::uint32_t thread_id, std::uint32_t error,
                const InstallerOptions& options) {
    switch (code) {
        case ids::kThreadEnumFailed:
            if (result == ThreadSet::Result::TooManyThreads) {
                return Diag::make(code,
                                  format("cannot stop the application's threads: it has more than %llu threads.",
                                         static_cast<unsigned long long>(options.max_threads)),
                                  "Reduce the thread count or report this limit as a Chameleon bug.");
            }
            return Diag::make(code,
                              format("cannot enumerate the application's threads (error 0x%x).", error),
                              "Report this as a Chameleon bug; ntdll!NtGetNextThread is required.");
        case ids::kThreadsNotConverging:
            return Diag::make(code,
                              format("the application keeps creating threads: %u enumeration passes still found new "
                                     "ones.",
                                     options.max_passes),
                              "Pause thread creation (or use synchronised mode and call SyncPoint where no threads "
                              "are being created) and reload again.");
        case ids::kSuspendFailed:
            return Diag::make(code,
                              format("thread %u could not be suspended or inspected (Win32 error %u).", thread_id,
                                     error),
                              "Report this as a Chameleon bug; a protected or exiting thread blocked the hook.");
        case ids::kWindowTimeout:
            return Diag::make(code,
                              format("the application's threads were stopped for more than %u ms; the hook was "
                                     "abandoned and they run again.",
                                     options.window_budget_ms),
                              "Reduce the number of functions per reload or report this as a bug.");
        case ipc::fault::ids::kAgentHookInstall:  // WP-28: built after the threads run again
            return Diag::make(code, "injected failure at the stage 'hook-install' (inside the window with the application's threads suspended); nothing was written",
                              "This failure was requested with the control command inject-failure. Reload again: without it the hooks install.");
        case ipc::fault::ids::kAgentRevertRepoint:  // WP-43
            return Diag::make(code, "injected failure at the stage 'revert-repoint' (re-pointing hooks to an older patch, inside the window with the application's threads suspended); nothing was written",
                              "This failure was requested with the control command inject-failure. Revert again: without it the hooks are re-pointed.");
        case ipc::fault::ids::kAgentRevertRestore:  // WP-43
            return Diag::make(code, "injected failure at the stage 'revert-restore' (restoring original bytes, inside the window with the application's threads suspended); nothing was written",
                              "This failure was requested with the control command inject-failure. Revert again: without it the original code comes back.");
        default:
            return Diag::make(code, format("hook installation failed (internal code %u).", code), "Report this as a bug.");
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------

struct Installer::Job {
    const HookRequest* request = nullptr;
    Op op = Op::Install;
    Record staged;  // Install: the record to add; Repoint: the record as it will be; Revert: the old one
    Write writes[2];
    int write_count = 0;
    std::uintptr_t window_lo = 0;
    std::uintptr_t window_hi = 0;
    std::uintptr_t protect_first_page = 0;
    int protect_pages = 0;
    DWORD old_protect[2] = {0, 0};
    bool protected_ok = false;
    Failure failure;
};

struct Installer::Report {
    bool done = false;
    bool retry = false;
    std::uint32_t diag = 0;  // batch-level failure
    ThreadSet::Result thread_result = ThreadSet::Result::Ok;
    std::uint32_t job = 0;
    std::uint32_t thread_id = 0;
    std::uint32_t error = 0;
    std::uint64_t rip = 0;
    unsigned threads = 0;
    unsigned passes = 0;
    unsigned window_ms = 0;
    // WP-50: counter ticks of this attempt's phases (BatchTiming says what each is).
    long long stop = 0;
    long long check = 0;
    long long write = 0;
    long long write_max = 0;
    long long step = 0;
    ThreadSet::Timing threads_timing;
};

Installer::Installer(const InstallerOptions& options) : options_(options) {
    ready_ = threads_.init(options_.max_threads);
}

Installer::~Installer() = default;

void Installer::set_test_options(const InstallerOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t threads = options_.max_threads;
    options_ = options;
    options_.max_threads = threads;
}

std::size_t Installer::hook_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

std::size_t Installer::forget_range(std::uint64_t base, std::uint64_t size) {
    if (size == 0) {
        return 0;
    }
    // A Record owns nothing beyond itself: the hook is a jump written into the module's own bytes (its padding or its entry) straight to
    // the replacement, with no trampoline, no near-allocated slot and no other bookkeeping (the patch images and their reservation are
    // the loader's, which keeps them for the session). So dropping the records is all there is to release; repeated FreeLibrary
    // cycles leak nothing here.
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t end = base + size;
    const auto first = std::remove_if(records_.begin(), records_.end(), [&](const Record& r) {
        const std::uint64_t target = static_cast<std::uint64_t>(r.target);
        return target >= base && target < end;
    });
    const std::size_t dropped = static_cast<std::size_t>(records_.end() - first);
    records_.erase(first, records_.end());  // stays sorted: remove_if keeps the order of what it keeps
    return dropped;
}

Installer::Record* Installer::find_record(std::uintptr_t target) {
    auto it = std::lower_bound(records_.begin(), records_.end(), target,
                               [](const Record& r, std::uintptr_t t) { return r.target < t; });
    return (it != records_.end() && it->target == target) ? &*it : nullptr;
}

const Installer::Record* Installer::find_record(std::uintptr_t target) const {
    auto it = std::lower_bound(records_.begin(), records_.end(), target,
                               [](const Record& r, std::uintptr_t t) { return r.target < t; });
    return (it != records_.end() && it->target == target) ? &*it : nullptr;
}

bool Installer::find(const void* target, HookInfo* info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Record* record = find_record(reinterpret_cast<std::uintptr_t>(target));
    if (record == nullptr) {
        return false;
    }
    if (info != nullptr) {
        info->strategy = record->strategy;
        info->replacement = reinterpret_cast<const void*>(record->replacement);
        std::memcpy(info->original_entry, record->original_entry, sizeof(info->original_entry));
        std::memcpy(info->original_padding, record->original_padding, sizeof(info->original_padding));
        info->entry_length = record->entry_length;
    }
    return true;
}

BatchResult Installer::install(const HookRequest* requests, std::size_t count, std::vector<HookOutcome>& outcomes, bool injectable) {
    return apply(false, requests, count, outcomes, injectable ? ipc::fault::Stage::HookInstall : ipc::fault::Stage::Count);
}

BatchResult Installer::install(const HookRequest* requests, std::size_t count, std::vector<HookOutcome>& outcomes, ipc::fault::Stage inject) {
    return apply(false, requests, count, outcomes, inject);
}

BatchResult Installer::revert(const HookRequest* requests, std::size_t count, std::vector<HookOutcome>& outcomes, ipc::fault::Stage inject) {
    return apply(true, requests, count, outcomes, inject);
}

// ---- the stopped-world part ---------------------------------------------------------------------

bool Installer::find_conflict(const Job* jobs, std::size_t count, std::size_t* thread_index,
                              std::size_t* job_index) const noexcept {
    for (std::size_t t = 0; t < threads_.size(); ++t) {
        const ThreadSet::Entry& thread = threads_.entry(t);
        if (!thread.suspended) {
            continue;
        }
        for (std::size_t j = 0; j < count; ++j) {
            if (jobs[j].op != Op::Unchanged && thread.rip >= jobs[j].window_lo && thread.rip < jobs[j].window_hi) {
                *thread_index = t;
                *job_index = j;
                return true;
            }
        }
    }
    return false;
}

// Runs with the application's threads stopped. No allocation, no logging, no locks, no CRT: see
// thread_set.h for the system calls used and why each is safe. The code that touches memory is
// bracketed by heap::set_alloc_forbidden so that a stray allocation aborts in tests.
void Installer::attempt(Job* jobs, std::size_t count, unsigned* conflicts_seen, Report* report, ipc::fault::Stage inject) noexcept {
    if (options_.test_before_stop != nullptr) {
        options_.test_before_stop(options_.test_context);
    }
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline = started + options_.window_budget_ms;

    threads_.set_test_knobs(options_.test_expire_after_checks, options_.test_step_limit);
    threads_.reset_timing();
    long long phase_started = ticks();
    ThreadSet::Result stopped = threads_.suspend_all(deadline, options_.max_passes, options_.test_fake_new_thread_passes);
    report->stop += ticks() - phase_started;
    report->passes = std::max(report->passes, threads_.passes());
    unsigned steps = 0;
    while (stopped == ThreadSet::Result::Ok) {
        report->threads = std::max<unsigned>(report->threads, static_cast<unsigned>(threads_.size()));

        heap::set_alloc_forbidden(true);
        std::size_t thread_index = 0;
        std::size_t job_index = 0;
        phase_started = ticks();
        const bool conflict = find_conflict(jobs, count, &thread_index, &job_index);
        report->check += ticks() - phase_started;
        if (!conflict) {
            // WP-28: failure injection (stage hook-install; WP-43: revert-repoint and revert-restore for the requests of a revert). One
            // lock-free atomic decrement and a constant table read: nothing here allocates or takes a lock. Nothing was written; the threads
            // are resumed below and the batch fails with the stage's diagnostic.
            if (inject != ipc::fault::Stage::Count && fault::consume(inject)) {
                report->diag = ipc::fault::info(inject).diag_id;
                break;
            }
            // Nobody is inside any window. Verify, then write.
            phase_started = ticks();
            bool intact = true;
            for (std::size_t j = 0; j < count && intact; ++j) {
                if (jobs[j].op == Op::Unchanged) {
                    continue;
                }
                for (int w = 0; w < jobs[j].write_count && intact; ++w) {
                    const Write& write = jobs[j].writes[w];
                    if (!bytes_equal(write.address, write.expect, write.length)) {
                        intact = false;
                        report->diag = ids::kCodeChanged;
                        report->job = static_cast<std::uint32_t>(j);
                    }
                }
            }
            report->check += ticks() - phase_started;
            if (intact) {
                if (options_.test_in_window != nullptr) {
                    options_.test_in_window(options_.test_context);
                }
                const HANDLE process = GetCurrentProcess();
                for (std::size_t j = 0; j < count; ++j) {
                    if (jobs[j].op == Op::Unchanged) {
                        continue;
                    }
                    const long long w0 = ticks();
                    for (int w = 0; w < jobs[j].write_count; ++w) {
                        apply_write(jobs[j].writes[w]);
                        FlushInstructionCache(process, jobs[j].writes[w].address, jobs[j].writes[w].length);
                    }
                    const long long spent = ticks() - w0;
                    report->write += spent;
                    report->write_max = std::max(report->write_max, spent);
                }
                report->done = true;
            }
            break;
        }

        // A thread is inside a window: remember who, and decide whether to try again.
        heap::set_alloc_forbidden(false);
        ++*conflicts_seen;
        report->job = static_cast<std::uint32_t>(job_index);
        report->thread_id = threads_.entry(thread_index).id;
        report->rip = threads_.entry(thread_index).rip;
        if (threads_.expired()) {
            // The budget is spent: not "a thread would not leave" (CML4305) but "took too long" (CML4309).
            stopped = ThreadSet::Result::Timeout;
            break;
        }
        if (steps >= options_.steps_per_attempt) {
            report->retry = true;
            break;
        }
        ++steps;

        // Let only the offenders run (everything else stays stopped), yield once so that they can
        // get out of the way, stop them again and re-check. Application threads that run now may use
        // the agent heap, hence the flag was cleared above.
        const long long step_started = ticks();
        std::size_t offenders[64];
        std::size_t offender_count = 0;
        for (std::size_t t = 0; t < threads_.size() && offender_count < 64; ++t) {
            const ThreadSet::Entry& thread = threads_.entry(t);
            if (!thread.suspended) {
                continue;
            }
            for (std::size_t j = 0; j < count; ++j) {
                if (jobs[j].op != Op::Unchanged && thread.rip >= jobs[j].window_lo && thread.rip < jobs[j].window_hi) {
                    offenders[offender_count++] = t;
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < offender_count; ++i) {
            threads_.resume_one(offenders[i]);
        }
        let_run(options_.offender_run_us);
        for (std::size_t i = 0; i < offender_count; ++i) {
            if (!threads_.suspend_one(offenders[i])) {
                if (threads_.timed_out()) {
                    stopped = ThreadSet::Result::Timeout;
                } else {
                    report->diag = ids::kSuspendFailed;
                }
                report->thread_id = threads_.entry(offenders[i]).id;
                report->error = threads_.failed_error();
                break;
            }
        }
        report->step += ticks() - step_started;
        if (report->diag != 0 || stopped != ThreadSet::Result::Ok) {
            break;
        }
        // The offenders may have started threads of their own.
        phase_started = ticks();
        stopped = threads_.suspend_all(deadline, options_.max_passes, options_.test_fake_new_thread_passes);
        report->stop += ticks() - phase_started;
        report->passes = std::max(report->passes, threads_.passes());
    }
    heap::set_alloc_forbidden(false);

    if (stopped != ThreadSet::Result::Ok) {
        report->diag = diag_for(stopped);
        report->thread_result = stopped;
        report->thread_id = threads_.failed_thread_id();
        report->error = threads_.failed_error();
    }
    threads_.resume_all();
    report->threads_timing = threads_.timing();
    const ULONGLONG elapsed = GetTickCount64() - started;
    report->window_ms = std::max<unsigned>(report->window_ms, static_cast<unsigned>(elapsed));
    if (options_.test_after_resume != nullptr) {
        options_.test_after_resume(options_.test_context);
    }
}

// ---- planning and orchestration ------------------------------------------------------------------

namespace {

void copy_name(char* destination, std::size_t capacity, const char* source) {
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    std::size_t i = 0;
    for (; source[i] != '\0' && i + 1 < capacity; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

struct Interval {
    std::uintptr_t lo;
    std::uintptr_t hi;
    std::int64_t job;  // index of the job, or -1 for an installed hook outside this batch
};

}  // namespace

BatchResult Installer::apply(bool reverting, const HookRequest* requests, std::size_t count,
                             std::vector<HookOutcome>& outcomes, ipc::fault::Stage inject) {
    const long long called = ticks();
    BatchResult result = apply_locked(reverting, requests, count, outcomes, inject, called);
    result.timing.total_us = to_us(ticks() - called);  // WP-50: every way out of the batch, the lock included
    return result;
}

BatchResult Installer::apply_locked(bool reverting, const HookRequest* requests, std::size_t count,
                                    std::vector<HookOutcome>& outcomes, ipc::fault::Stage inject, long long called) {
    std::lock_guard<std::mutex> lock(mutex_);
    BatchResult result;
    // WP-50: the phase boundaries. `since` is the start of the current phase (apply() fills total_us).
    long long since = ticks();
    result.timing.lock_us = to_us(since - called);
    long long probing = 0;  // ticks inside ReadProcessMemory / VirtualQuery while planning
    long long querying = 0;  // of them: inside VirtualQuery (is_code)
    std::uint32_t queries = 0;  // VirtualQuery calls made (the region cache answers the rest)
    RegionCache regions(!options_.test_no_region_cache);
    const auto timed_is_code = [&querying, &queries, &regions](const void* address) {
        const long long q0 = ticks();
        const bool code = regions.is_code(address, &queries);
        querying += ticks() - q0;
        return code;
    };
    const auto lap = [&since]() {
        const long long now = ticks();
        const long long spent = now - since;
        since = now;
        return to_us(spent);
    };
    outcomes.assign(count, HookOutcome{});
    auto fail_all = [&](Diag diag) {
        for (HookOutcome& outcome : outcomes) {
            outcome.status = HookStatus::NotAttempted;
            outcome.diag = diag;
        }
        result.diag = std::move(diag);
        return result;
    };
    if (count == 0) {
        return result;
    }
    if (!ready_) {
        return fail_all(batch_diag(ids::kThreadEnumFailed, ThreadSet::Result::EnumFailed, 0, 0, options_));
    }
    if (count > options_.max_hooks_per_request) {
        return fail_all(Diag::make(ids::kTooManyHooks,
                                   format("a request with %llu hooks is above the limit of %llu.",
                                          static_cast<unsigned long long>(count),
                                          static_cast<unsigned long long>(options_.max_hooks_per_request)),
                                   "Split the request."));
    }

    // ---- 1. plan (everything may allocate here) ----
    std::vector<Job> jobs(count);
    records_.reserve(records_.size() + count);  // the commit below must not allocate while jobs are half applied
    bool any_failed = false;
    std::uint32_t first_failed = 0;

    // WP-50 (R-47): the region cache is filled in ascending address order first, so that one VirtualQuery of the lowest
    // address of a region answers every later question inside it (VirtualQuery reports the range from the queried page to
    // the region's end). Only the addresses planning may ask about; the answers are the ones planning would get.
    if (!options_.test_no_region_cache && !reverting) {
        std::vector<std::uintptr_t> asked;
        asked.reserve(count * 4);
        for (std::size_t i = 0; i < count; ++i) {
            const auto t = reinterpret_cast<std::uintptr_t>(requests[i].target);
            const auto r = reinterpret_cast<std::uintptr_t>(requests[i].replacement);
            if (r != 0) {
                asked.push_back(r);
            }
            if (t > kMinPadding) {
                asked.push_back(t - kMinPadding);
                asked.push_back(t - 1);
                asked.push_back(t);
            }
        }
        std::sort(asked.begin(), asked.end());
        asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
        const long long warm_started = ticks();
        for (const std::uintptr_t a : asked) {
            (void)timed_is_code(reinterpret_cast<const void*>(a));
        }
        probing += ticks() - warm_started;
    }

    for (std::size_t i = 0; i < count; ++i) {
        Job& job = jobs[i];
        const HookRequest& request = requests[i];
        job.request = &request;
        Failure& failure = job.failure;
        const auto target = reinterpret_cast<std::uintptr_t>(request.target);
        const auto replacement = reinterpret_cast<std::uintptr_t>(request.replacement);

        auto reject = [&](std::uint32_t code) {
            failure.code = code;
            any_failed = true;
        };

        if (target == 0 || (!reverting && replacement == 0)) {
            failure.reason = BadArgument::Null;
            reject(ids::kBadArgument);
            continue;
        }
        if (!reverting && target == replacement) {
            failure.reason = BadArgument::SameFunction;
            reject(ids::kBadArgument);
            continue;
        }
        const Record* existing = find_record(target);

        if (reverting) {
            if (existing == nullptr) {
                reject(ids::kNotInstalled);
                continue;
            }
            job.op = Op::Revert;
            job.staged = *existing;
            const Record& e = *existing;
            auto* entry = reinterpret_cast<std::uint8_t*>(e.target);
            Write& first = job.writes[0];
            first.address = entry;
            first.length = e.entry_length;
            std::memcpy(first.expect, e.current_entry, e.entry_length);
            std::memcpy(first.data, e.original_entry, e.entry_length);
            job.write_count = 1;
            if (e.padding_length != 0) {
                Write& second = job.writes[1];
                second.address = entry - e.padding_length;
                second.length = e.padding_length;
                std::memcpy(second.expect, e.current_padding, e.padding_length);
                std::memcpy(second.data, e.original_padding, e.padding_length);
                job.write_count = 2;
            }
            job.window_lo = e.target - e.window_before;
            job.window_hi = e.target + e.window_after;
            continue;
        }

        long long probe_started = ticks();
        const bool replacement_is_code = timed_is_code(request.replacement);
        probing += ticks() - probe_started;
        if (!replacement_is_code) {
            failure.reason = BadArgument::NotCode;
            reject(ids::kBadArgument);
            continue;
        }

        if (existing != nullptr) {
            job.staged = *existing;
            Record& e = job.staged;
            job.window_lo = e.target - e.window_before;
            job.window_hi = e.target + e.window_after;
            if (existing->replacement == replacement) {
                job.op = Op::Unchanged;
                continue;
            }
            job.op = Op::Repoint;
            std::uint8_t jump[5];
            const std::uintptr_t site = e.strategy == Strategy::PaddingTwoByte ? e.target - kJmpRel32Size : e.target;
            if (!encode_jmp_rel32(jump, site, replacement)) {
                failure.a = replacement;
                failure.b = site;
                reject(ids::kOutOfRange);
                continue;
            }
            Write& w = job.writes[0];
            job.write_count = 1;
            w.length = 4;
            if (e.strategy == Strategy::PaddingTwoByte) {
                w.address = reinterpret_cast<std::uint8_t*>(e.target) - 4;
                std::memcpy(w.expect, e.current_padding + 1, 4);
                std::memcpy(e.current_padding, jump, 5);
            } else {
                w.address = reinterpret_cast<std::uint8_t*>(e.target) + 1;
                std::memcpy(w.expect, e.current_entry + 1, 4);
                std::memcpy(e.current_entry, jump, 5);
            }
            std::memcpy(w.data, jump + 1, 4);
            e.replacement = replacement;
            continue;
        }

        // A new hook: read the bytes around the entry and choose a strategy.
        probe_started = ticks();
        if (!timed_is_code(request.target)) {
            probing += ticks() - probe_started;
            reject(ids::kCodeUnreadable);
            continue;
        }
        std::uint8_t before[kLookBehind] = {};
        std::uint8_t at[kLookAhead] = {};
        std::size_t before_len = 0;
        for (std::size_t k = kLookBehind; k > 0; --k) {
            std::size_t got = 0;
            if (read_code(reinterpret_cast<const std::uint8_t*>(target) - k, before, k, &got) && got == k) {
                before_len = k;
                break;
            }
        }
        std::size_t at_len = 0;
        read_code(request.target, at, kLookAhead, &at_len);  // a partial read still tells how much is readable
        PlanEnvironment environment;
        environment.entry_two_byte_aligned = (target & 1) == 0;
        // Only worth a VirtualQuery when the bytes say the padding is there at all.
        environment.padding_executable = before_len < kMinPadding || (timed_is_code(reinterpret_cast<const void*>(target - kMinPadding)) &&
                                                                      timed_is_code(reinterpret_cast<const void*>(target - 1)));
        probing += ticks() - probe_started;
        const HookPlan plan = plan_hook(before, before_len, at, at_len, environment);
        if (plan.strategy == Strategy::None) {
            failure.code = plan.diag;
            failure.offset = plan.fail_offset;
            failure.byte_count = plan.fail_byte_count;
            std::memcpy(failure.bytes, plan.fail_bytes, sizeof(failure.bytes));
            any_failed = true;
            continue;
        }

        job.op = Op::Install;
        Record& r = job.staged;
        r.target = target;
        r.replacement = replacement;
        r.strategy = plan.strategy;
        r.entry_length = static_cast<std::uint8_t>(plan.entry_write_len);
        r.padding_length = static_cast<std::uint8_t>(plan.padding_write_len);
        r.window_before = plan.window_before;
        r.window_after = plan.window_after;
        copy_name(r.name, sizeof(r.name), request.name);
        auto* entry = reinterpret_cast<std::uint8_t*>(target);

        std::uint8_t jump[5];
        const std::uintptr_t site = plan.strategy == Strategy::PaddingTwoByte ? target - kJmpRel32Size : target;
        if (!encode_jmp_rel32(jump, site, replacement)) {
            failure.a = replacement;
            failure.b = site;
            reject(ids::kOutOfRange);
            continue;
        }
        std::memcpy(r.original_entry, at, r.entry_length);
        if (plan.strategy == Strategy::PaddingTwoByte) {
            Write& pad = job.writes[0];
            pad.address = entry - kJmpRel32Size;
            pad.length = 5;
            std::memcpy(pad.expect, before + before_len - 5, 5);
            std::memcpy(pad.data, jump, 5);
            Write& head = job.writes[1];
            head.address = entry;
            head.length = 2;
            std::memcpy(head.expect, at, 2);
            head.data[0] = 0xEB;  // jmp rel8
            head.data[1] = 0xF9;  // -7: from entry+2 back to entry-5, where the padding jump is
            job.write_count = 2;
            std::memcpy(r.original_padding, pad.expect, 5);
            std::memcpy(r.current_padding, jump, 5);
            r.current_entry[0] = 0xEB;
            r.current_entry[1] = 0xF9;
        } else {
            Write& head = job.writes[0];
            head.address = entry;
            head.length = 5;
            std::memcpy(head.expect, at, 5);
            std::memcpy(head.data, jump, 5);
            job.write_count = 1;
            std::memcpy(r.current_entry, jump, 5);
        }
        job.window_lo = target - plan.window_before;
        job.window_hi = target + plan.window_after;
    }

    // ---- 1b. batch-level checks: duplicates, overlapping regions ----
    {
        std::vector<std::pair<std::uintptr_t, std::size_t>> order;
        order.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            order.emplace_back(reinterpret_cast<std::uintptr_t>(requests[i].target), i);
        }
        std::sort(order.begin(), order.end());
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (order[i].first == order[i - 1].first && order[i].first != 0) {
                Job& later = jobs[order[i].second];
                if (later.failure.code == 0) {
                    later.failure.code = ids::kBadArgument;
                    later.failure.reason = BadArgument::Duplicate;
                }
                any_failed = true;
            }
        }
    }
    if (!any_failed) {
        std::vector<Interval> intervals;
        intervals.reserve(count + records_.size());
        std::vector<std::uintptr_t> replaced;  // installed hooks this batch re-points or reverts
        for (std::size_t i = 0; i < count; ++i) {
            if (jobs[i].op == Op::Repoint || jobs[i].op == Op::Revert) {
                replaced.push_back(jobs[i].staged.target);
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (jobs[i].op != Op::Unchanged) {
                std::uintptr_t lo = jobs[i].window_lo;
                std::uintptr_t hi = jobs[i].window_hi;
                for (int w = 0; w < jobs[i].write_count; ++w) {
                    lo = std::min(lo, reinterpret_cast<std::uintptr_t>(jobs[i].writes[w].address));
                    hi = std::max(hi, reinterpret_cast<std::uintptr_t>(jobs[i].writes[w].address) + jobs[i].writes[w].length);
                }
                intervals.push_back({lo, hi, static_cast<std::int64_t>(i)});
            }
        }
        for (const Record& r : records_) {
            if (std::find(replaced.begin(), replaced.end(), r.target) == replaced.end()) {
                intervals.push_back({r.target - r.window_before, r.target + r.window_after, -1});
            }
        }
        std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
        for (std::size_t i = 1; i < intervals.size(); ++i) {
            if (intervals[i].lo < intervals[i - 1].hi) {
                const std::int64_t culprit = intervals[i].job >= 0 ? intervals[i].job : intervals[i - 1].job;
                if (culprit >= 0 && jobs[static_cast<std::size_t>(culprit)].failure.code == 0) {
                    jobs[static_cast<std::size_t>(culprit)].failure.code = ids::kOverlap;
                    any_failed = true;
                }
            }
        }
    }

    regions.clear();  // planning is over: the batch's own VirtualProtect and writes come next
    result.timing.plan_us = lap();
    result.timing.probe_us = to_us(probing);
    result.timing.query_us = to_us(querying);
    result.timing.queries = queries;
    for (const Job& job : jobs) {
        if (job.failure.code != 0) {
            continue;
        }
        switch (job.op) {
            case Op::Install: ++result.timing.installs; break;
            case Op::Repoint: ++result.timing.repoints; break;
            case Op::Unchanged: ++result.timing.unchanged; break;
            case Op::Revert: ++result.timing.reverts; break;
        }
    }
    if (any_failed) {
        // Nothing has been touched. Name the failures; everything else is "not attempted".
        bool have_first = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (jobs[i].failure.code != 0) {
                outcomes[i].status = HookStatus::Failed;
                outcomes[i].diag = make_diag(jobs[i].failure, requests[i]);
                if (!have_first) {
                    first_failed = static_cast<std::uint32_t>(i);
                    have_first = true;
                }
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (jobs[i].failure.code == 0) {
                outcomes[i].status = HookStatus::NotAttempted;
                outcomes[i].diag = Diag::make(ids::kBatchAborted,
                                              format("%s was not changed: another hook of the same request failed (%s).",
                                                     display_name(requests[i]), display_name(requests[first_failed])),
                                              "Fix the failing hook and send the request again.");
            }
        }
        result.diag = outcomes[first_failed].diag;
        CML_LOG(Warn, "hooks: %s of %llu hook(s) refused: %s", reverting ? "revert" : "install",
                static_cast<unsigned long long>(count), result.diag.format().c_str());
        return result;
    }

    bool any_write = false;
    for (const Job& job : jobs) {
        any_write = any_write || job.op != Op::Unchanged;
    }

    // ---- 2. make the pages writable (still with all threads running) ----
    std::size_t protected_jobs = 0;
    if (any_write) {
        for (std::size_t i = 0; i < count; ++i) {
            Job& job = jobs[i];
            if (job.op == Op::Unchanged) {
                continue;
            }
            std::uintptr_t lo = ~static_cast<std::uintptr_t>(0);
            std::uintptr_t hi = 0;
            for (int w = 0; w < job.write_count; ++w) {
                const auto a = reinterpret_cast<std::uintptr_t>(job.writes[w].address);
                lo = std::min(lo, a);
                hi = std::max(hi, a + job.writes[w].length);
            }
            job.protect_first_page = lo & ~(kPageSize - 1);
            job.protect_pages = static_cast<int>(((hi - 1) & ~(kPageSize - 1)) - job.protect_first_page) / static_cast<int>(kPageSize) + 1;
            bool ok = job.protect_pages <= 2;
            int done = 0;
            DWORD error = ok ? 0 : ERROR_INVALID_PARAMETER;
            for (; ok && done < job.protect_pages; ++done) {
                void* page = reinterpret_cast<void*>(job.protect_first_page + static_cast<std::uintptr_t>(done) * kPageSize);
                if (VirtualProtect(page, kPageSize, PAGE_EXECUTE_READWRITE, &job.old_protect[done]) == 0) {
                    ok = false;
                    error = GetLastError();
                    break;
                }
            }
            if (!ok) {
                for (int p = done - 1; p >= 0; --p) {  // undo the pages of this job already changed
                    DWORD ignored = 0;
                    VirtualProtect(reinterpret_cast<void*>(job.protect_first_page + static_cast<std::uintptr_t>(p) * kPageSize),
                                   kPageSize, job.old_protect[p], &ignored);
                }
                job.failure.code = ids::kWriteFailed;
                job.failure.win_error = error;
                break;  // the loop below restores the jobs before it
            }
            job.protected_ok = true;
            protected_jobs = i + 1;
            result.timing.pages += static_cast<std::uint32_t>(job.protect_pages);
        }
    }
    result.timing.protect_us = lap();
    auto restore_protection = [&]() {
        for (std::size_t i = protected_jobs; i-- > 0;) {
            Job& job = jobs[i];
            if (!job.protected_ok) {
                continue;
            }
            for (int p = job.protect_pages - 1; p >= 0; --p) {
                DWORD ignored = 0;
                VirtualProtect(reinterpret_cast<void*>(job.protect_first_page + static_cast<std::uintptr_t>(p) * kPageSize),
                               kPageSize, job.old_protect[p], &ignored);
            }
            job.protected_ok = false;
        }
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (jobs[i].failure.code == ids::kWriteFailed) {
            restore_protection();
            result.timing.unprotect_us = lap();
            for (std::size_t k = 0; k < count; ++k) {
                outcomes[k].status = k == i ? HookStatus::Failed : HookStatus::NotAttempted;
                outcomes[k].diag = k == i ? make_diag(jobs[i].failure, requests[i])
                                          : Diag::make(ids::kBatchAborted,
                                                       format("%s was not changed: another hook of the same request failed.",
                                                              display_name(requests[k])),
                                                       "Fix the failing hook and send the request again.");
            }
            result.diag = outcomes[i].diag;
            CML_LOG(Warn, "hooks: %s", result.diag.format().c_str());
            return result;
        }
    }

    // ---- 3. stop the world, check, write, resume (retrying on RIP-in-window) ----
    Report report;
    unsigned conflicts = 0;
    if (any_write) {
        for (;;) {
            report = Report{};
            attempt(jobs.data(), count, &conflicts, &report, inject);
            result.timing.stop_us += to_us(report.stop);
            result.timing.check_us += to_us(report.check);
            result.timing.write_us += to_us(report.write);
            result.timing.write_max_us = std::max(result.timing.write_max_us, to_us(report.write_max));
            result.timing.step_us += to_us(report.step);
            result.timing.enumerate_us += to_us(report.threads_timing.enumerate);
            result.timing.suspend_us += to_us(report.threads_timing.suspend);
            result.timing.context_us += to_us(report.threads_timing.context);
            result.timing.resume_us += to_us(report.threads_timing.resume);
            ++result.stats.attempts;
            result.stats.threads = std::max(result.stats.threads, report.threads);
            result.stats.passes = std::max(result.stats.passes, report.passes);
            result.stats.window_ms = std::max(result.stats.window_ms, report.window_ms);
            if (report.done || report.diag != 0 || !report.retry) {
                break;
            }
            // The offender is still in the way. The plan allows max_attempts looks; then give up.
            if (result.stats.attempts >= options_.max_attempts) {
                report.diag = ids::kThreadInWindow;
                break;
            }
            // Let the whole application run, then look again.
            const long long wait_started = ticks();
            if (result.stats.attempts <= kYieldOnlyRetries) {
                SwitchToThread();
            } else {
                Sleep(1);
            }
            result.timing.retry_wait_us += to_us(ticks() - wait_started);
        }
        result.stats.retries = conflicts;
    } else {
        report.done = true;
    }

    since = ticks();  // the window phases were summed per attempt above
    // ---- 4. protection back, registry, outcomes ----
    restore_protection();
    result.timing.unprotect_us = lap();

    if (!report.done) {
        Diag diag;
        std::size_t culprit = count;  // job named by the failure, if any
        if (report.diag == ids::kThreadInWindow || report.diag == ids::kCodeChanged) {
            culprit = report.job;
            Failure f;
            f.code = report.diag;
            f.thread_id = report.thread_id;
            f.a = report.rip;
            f.conflicts = result.stats.attempts;
            diag = make_diag(f, requests[culprit]);
        } else {
            diag = batch_diag(report.diag, report.thread_result, report.thread_id, report.error, options_);
        }
        for (std::size_t k = 0; k < count; ++k) {
            if (k == culprit) {
                outcomes[k].status = HookStatus::Failed;
                outcomes[k].diag = diag;
            } else {
                outcomes[k].status = HookStatus::NotAttempted;
                outcomes[k].diag = culprit < count
                                       ? Diag::make(ids::kBatchAborted,
                                                    format("%s was not changed: another hook of the same request "
                                                           "failed (%s).",
                                                           display_name(requests[k]), display_name(requests[culprit])),
                                                    "Fix the failing hook and send the request again.")
                                       : diag;
            }
        }
        result.diag = diag;
        CML_LOG(Warn, "hooks: %s of %llu hook(s) failed after %u attempt(s), %u retries: %s",
                reverting ? "revert" : "install", static_cast<unsigned long long>(count), result.stats.attempts,
                result.stats.retries, diag.format().c_str());
        return result;
    }

    for (std::size_t i = 0; i < count; ++i) {
        Job& job = jobs[i];
        HookOutcome& outcome = outcomes[i];
        outcome.strategy = job.staged.strategy;
        switch (job.op) {
            case Op::Install: {
                auto position = std::lower_bound(records_.begin(), records_.end(), job.staged.target,
                                                 [](const Record& r, std::uintptr_t t) { return r.target < t; });
                records_.insert(position, job.staged);  // capacity was reserved: no reallocation
                outcome.status = HookStatus::Installed;
                break;
            }
            case Op::Repoint:
                *find_record(job.staged.target) = job.staged;
                outcome.status = HookStatus::Repointed;
                break;
            case Op::Unchanged:
                outcome.status = HookStatus::Unchanged;
                break;
            case Op::Revert:
                records_.erase(records_.begin() + (find_record(job.staged.target) - records_.data()));
                outcome.status = HookStatus::Reverted;
                break;
        }
    }
    result.timing.commit_us = lap();
    CML_LOG(Info, "hooks: %s %llu hook(s): attempts=%u retries=%u threads=%u window=%ums",
            reverting ? "reverted" : "applied", static_cast<unsigned long long>(count), result.stats.attempts,
            result.stats.retries, result.stats.threads, result.stats.window_ms);
    return result;
}

Installer& process_installer() {
    static Installer* instance = new Installer();  // never destroyed: hooks outlive every static destructor
    return *instance;
}

}  // namespace cml::agent::hook

#endif  // _WIN32
