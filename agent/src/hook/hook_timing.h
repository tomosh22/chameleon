#pragma once

// WP-50 (R-47): where the time of one InstallHooks goes, and the evidence line that says so.
//
// The installer (installer.cpp) fills a BatchTiming for every batch; the dispatcher (dispatch.cpp) adds the phases around it
// (the wait in the queue, collecting and running the application's hooks, building the answer) and logs one line,
//
//   WP50-HOOKS side=agent request=<id> batch=<n> op=<install|revert|quiet> hooks=<n> installed=<n> repointed=<n> unchanged=<n>
//              reverted=<n> pages=<n> threads=<n> attempts=<n> status=<CML id or 0> total_us=... <phase>_us=...
//
// which goes to the agent's log and to the broker (forwarded whatever the log level), whose log keeps it. cmltest prints the
// broker's evidence lines after every reload, so CI shows it next to RELOAD-TIMING. `batch` counts the batches of this process
// (1 = the first InstallHooks the installer ever applied).
//
// Header-only and portable (no Windows calls): the line is unit-tested on every host.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace cml::agent::hook {

// Where the time of one batch went, in microseconds (QueryPerformanceCounter). Phases in the order they run; the window phases
// are summed over every attempt. Measured with one counter read at each phase boundary (and around each thread system call,
// ThreadSet::Timing); nothing is allocated or logged for it while threads are suspended.
struct BatchTiming {
    std::uint64_t lock_us = 0;        // waiting for another batch (the installer's mutex)
    std::uint64_t plan_us = 0;        // 1 + 1b: reading the bytes, choosing strategies, the batch checks
    std::uint64_t probe_us = 0;       //   of plan_us: ReadProcessMemory / VirtualQuery on the targets and the replacements
    std::uint64_t protect_us = 0;     // 2: VirtualProtect to writable
    std::uint64_t enumerate_us = 0;   // 3: NtGetNextThread (ThreadSet::Timing::enumerate)
    std::uint64_t suspend_us = 0;     // 3: SuspendThread (also when an offender is stopped again, which is inside step_us)
    std::uint64_t context_us = 0;     // 3: GetThreadContext (waits until each thread has stopped; offenders as for suspend_us)
    std::uint64_t stop_us = 0;        // 3: all of ThreadSet::suspend_all (the three above and the bookkeeping between them)
    std::uint64_t check_us = 0;       // 3: RIP against the windows, and the verification of the bytes
    std::uint64_t write_us = 0;       // 4: the stores and FlushInstructionCache, every hook
    std::uint64_t write_max_us = 0;   //   of write_us: the slowest single hook (a first write to a page copies it)
    std::uint64_t step_us = 0;        // 3: letting only the offenders run (RIP in a window), every step
    std::uint64_t resume_us = 0;      // 3/4: ResumeThread and closing the handles (letting an offender run is also inside step_us)
    std::uint64_t retry_wait_us = 0;  // between attempts: SwitchToThread / Sleep(1)
    std::uint64_t unprotect_us = 0;   // 5: VirtualProtect back
    std::uint64_t commit_us = 0;      // 5: the registry
    std::uint64_t total_us = 0;       // the whole call, lock included
    std::uint32_t installs = 0;       // jobs by kind (of a batch that passed planning; a refused batch counts what was planned)
    std::uint32_t repoints = 0;
    std::uint32_t unchanged = 0;
    std::uint32_t reverts = 0;
    std::uint32_t pages = 0;          // pages made writable (a job spans one or two)
};

// The phases of one request around the installer (dispatch.cpp), in microseconds.
struct DispatchTiming {
    std::uint64_t wait_us = 0;       // from the request's arrival to the start of the work (the SyncPoint queue in synchronised mode)
    std::uint64_t prefetch_us = 0;   // WP-50: bringing the targets' pages into memory (prefetch_code_pages), threads running
    std::uint64_t collect_us = 0;    // user_hooks::Session: the hook records of the enabled modules and the patch images
    std::uint64_t prepatch_us = 0;   // the application's pre-patch hooks
    std::uint64_t build_us = 0;      // the installer's requests from the message
    std::uint64_t install_us = 0;    // Installer::install / revert (BatchTiming::total_us)
    std::uint64_t rollback_us = 0;   // WP-28 injection only: taking the hooks out again
    std::uint64_t postpatch_us = 0;  // commit of the patches and the application's post-patch hooks
    std::uint64_t reply_us = 0;      // the answer
    std::uint64_t total_us = 0;      // arrival to the answer
};

struct HooksTimingLine {
    std::uint64_t request_id = 0;
    std::uint64_t batch = 0;
    const char* op = "install";  // "install", "revert" or "quiet" (a re-pointing of a revert, WP-22)
    std::size_t hooks = 0;
    std::uint32_t status = 0;
    unsigned threads = 0;
    unsigned attempts = 0;
    const char* prefetch = "none";      // WP-50: "api", "touch" or "none"
    std::uint32_t prefetch_pages = 0;
    BatchTiming batch_timing;
    DispatchTiming dispatch;
};

namespace timing_detail {

inline void append(std::string& out, const char* format, ...) {
    char buffer[160];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) {
        out.append(buffer, static_cast<std::size_t>(n) < sizeof(buffer) ? static_cast<std::size_t>(n) : sizeof(buffer) - 1);
    }
}

inline unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

}  // namespace timing_detail

// The line described at the top. Keys are stable: CI and the documentation (docs/wp/WP-50.md, TROUBLESHOOTING) read them.
inline std::string format_hooks_timing(const HooksTimingLine& l) {
    using timing_detail::append;
    using timing_detail::ull;
    const BatchTiming& b = l.batch_timing;
    const DispatchTiming& d = l.dispatch;
    std::string out;
    out.reserve(720);
    append(out, "WP50-HOOKS side=agent request=%llu batch=%llu op=%s hooks=%llu", ull(l.request_id), ull(l.batch), l.op, ull(l.hooks));
    append(out, " installed=%u repointed=%u unchanged=%u reverted=%u pages=%u threads=%u attempts=%u", b.installs, b.repoints, b.unchanged,
           b.reverts, b.pages, l.threads, l.attempts);
    if (l.status == 0) {
        out += " status=0";
    } else {
        append(out, " status=CML%u", l.status);
    }
    append(out, " total_us=%llu wait_us=%llu prefetch_us=%llu prefetch=%s prefetch_pages=%u", ull(d.total_us), ull(d.wait_us), ull(d.prefetch_us),
           l.prefetch, l.prefetch_pages);
    append(out, " collect_us=%llu prepatch_us=%llu build_us=%llu", ull(d.collect_us), ull(d.prepatch_us), ull(d.build_us));
    append(out, " install_us=%llu lock_us=%llu plan_us=%llu probe_us=%llu protect_us=%llu", ull(d.install_us), ull(b.lock_us), ull(b.plan_us),
           ull(b.probe_us), ull(b.protect_us));
    append(out, " stop_us=%llu enumerate_us=%llu suspend_us=%llu context_us=%llu check_us=%llu", ull(b.stop_us), ull(b.enumerate_us),
           ull(b.suspend_us), ull(b.context_us), ull(b.check_us));
    append(out, " write_us=%llu write_max_us=%llu step_us=%llu resume_us=%llu retry_wait_us=%llu", ull(b.write_us), ull(b.write_max_us),
           ull(b.step_us), ull(b.resume_us), ull(b.retry_wait_us));
    append(out, " unprotect_us=%llu commit_us=%llu rollback_us=%llu postpatch_us=%llu reply_us=%llu", ull(b.unprotect_us), ull(b.commit_us),
           ull(d.rollback_us), ull(d.postpatch_us), ull(d.reply_us));
    return out;
}

}  // namespace cml::agent::hook
