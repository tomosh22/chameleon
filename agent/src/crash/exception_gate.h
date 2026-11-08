#pragma once

// Crash -> patch -> continue, agent side (plan section 4.11, WP-20).
//
// A vectored exception handler, registered first, that holds a thread which faulted in the application's own code until a
// person (through the broker) decides what happens to it:
//   Continue  resume at the faulting instruction (after a patch fixed the state the instruction reads)
//   Leave     unwind the faulting function one frame and return 0 from it
//   Ignore    pass the exception to the next handler (what would have happened without Chameleon)
//   Reexecute (WP-23) run the replacement of a function again from its entry, with the register arguments reloaded from the
//             locations the PDB records for the parameters (the broker resolves them; see compute_reexecute)
//
// The handler acts only when ALL of this holds, and otherwise returns EXCEPTION_CONTINUE_SEARCH at once:
//   * the gate is enabled (the broker says a control client listens for `exception` events and `crash_handler` is on);
//   * the code is an access violation, an integer division by zero or an illegal instruction;
//   * the faulting instruction lies in an enabled module or in memory the patch loader owns (a patch image);
//   * the thread is not one of the agent's own threads;
//   * the hook installer does not have the other threads stopped (heap::alloc_forbidden());
//   * no debugger is attached, unless break_with_debugger is set.
//
// THE HANDLER RUNS ON THE FAULTING THREAD, which may hold the process heap, the loader lock or any other lock. It therefore
// allocates nothing, logs nothing, takes no lock and calls into no IPC: it fills a preallocated slot (thread id, a copy of the
// CONTEXT, code, address), signals the agent thread through an event, and waits on the slot's own event. Everything else -
// reporting to the broker, unwinding for Leave, the decision - happens on the agent thread (take_new / resolve). The number of
// slots is bounded (kMaxSlots); a fault that finds none is passed on (Ignore) and counted.
//
// The handler is registered once per process and stays registered: the agent DLL is never unloaded. All state is static.

#if defined(_WIN32)

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cml/util/result.h"

namespace cml::agent::crash {

enum class Action : std::int32_t { None = 0, Continue = 1, Leave = 2, Ignore = 3, Reexecute = 4 };

inline constexpr std::size_t kMaxSlots = 8;
inline constexpr std::size_t kMaxRanges = 256;
inline constexpr std::size_t kMaxIgnoredThreads = 8;
inline constexpr std::size_t kMaxFrames = 32;  // frames reported per held thread (WP-23)

// One frame of a held thread's stack, innermost first (the agent-side twin of ipc::ExceptionFrame).
struct Frame {
    std::uint64_t rip = 0;
    std::uint64_t entry_rsp = 0;       // RSP at the function's entry (points at the return address); 0 when unknown
    std::uint64_t rsp = 0;
    std::uint64_t rbp = 0;
    std::uint64_t r13 = 0;
    std::uint64_t function_start = 0;  // per the unwind data; 0 when the frame has none
};

// The broker's recipe for Reexecute (the agent-side twin of ipc::Reexecute).
struct ReexecuteParam {
    std::uint32_t position = 0;       // 0..3
    bool is_float = false;
    std::uint32_t size = 0;           // 1, 2, 4, 8
    std::uint32_t base_register = 0;  // CV_AMD64: 334 RBP, 335 RSP, 341 R13
    std::int32_t offset = 0;
};
struct ReexecutePlan {
    std::uint32_t frame_index = 0;
    std::uint64_t entry_rsp = 0;
    std::uint64_t target = 0;
    std::uint32_t param_count = 0;
    ReexecuteParam params[4];
};

// An address range in which a fault is the application's business.
struct Range {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    bool patch = false;  // a patch image (memory owned by the patch loader), not an enabled module
};

// What the agent thread learns about a held thread.
struct Held {
    std::uint32_t id = 0;
    std::uint32_t thread_id = 0;
    std::uint32_t code = 0;
    std::uint32_t access = 0xFFFFFFFFu;  // access violation: 0 read, 1 write, 8 execute; else 0xFFFFFFFF
    std::uint64_t address = 0;           // the faulting instruction
    std::uint64_t data_address = 0;      // access violation: the accessed address
    std::uint64_t stack_pointer = 0;
    std::uint64_t module_base = 0;       // base of the range holding `address`
    bool in_patch = false;
    std::uint32_t frame_count = 0;       // WP-23: the unwound stack, innermost first
    Frame frames[kMaxFrames];
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded because CONTEXT is 16-byte aligned: intended
#endif

class ExceptionGate {
public:
    // The process-wide gate. Created on first use, never destroyed.
    static ExceptionGate& shared();

    // Creates the slot events and registers the handler first in the chain. NOT usable from the handler's context. Idempotent;
    // CML4404 / CML4405 when it cannot (crash handling then stays off).
    Result<void> install();
    bool installed() const noexcept { return installed_.load(std::memory_order_acquire); }

    // Turns holding on or off. Turning it off answers every held thread with Ignore. Agent thread.
    void configure(bool enabled, bool break_with_debugger);
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Replaces the ranges in which faults are acted on (agent thread; extra ranges beyond kMaxRanges are dropped).
    // Protocol (the handler must stay lock-free): two tables, one active. The handler counts itself in as a reader of the table it
    // read from `active_table_` and re-checks that it is still the active one before scanning (else it counts out and retries); the
    // writer fills the INACTIVE table only after its reader count is zero, then flips. So a handler never scans a table while it is
    // being rewritten, however long it was preempted.
    void set_ranges(const Range* ranges, std::size_t count);
    std::size_t range_count() const noexcept;
    // The handler's lookup, public so a test can hammer it against set_ranges() without faulting.
    bool in_ranges_for_testing(std::uint64_t pc) const noexcept {
        std::uint64_t base = 0;
        bool patch = false;
        return classify(pc, &base, &patch);
    }

    // Threads that never get held (the agent's own). Agent thread / worker threads at their start and end.
    void add_ignored_thread(std::uint32_t thread_id) noexcept;
    void remove_ignored_thread(std::uint32_t thread_id) noexcept;

    // The agent thread's wake-up event: signalled when a thread was held. The caller keeps it open until it called
    // set_notify_event(nullptr).
    void set_notify_event(HANDLE event) noexcept { notify_.store(event, std::memory_order_release); }

    // Agent thread. The next held thread that was not reported yet.
    bool take_new(Held* out);
    // Agent thread. Releases the thread with `id`. CML4400 when no thread with that id is waiting for an answer; for Leave
    // CML4401 / CML4402 when the frame cannot be unwound (the thread stays held; choose another action).
    Result<void> resolve(std::uint32_t id, Action action);
    // WP-23. Agent thread. Releases the thread with `id` at the entry of `plan.target`, as described by `plan`
    // (compute_reexecute). CML4400 when no thread with that id is waiting; CML4600..4603 when the plan cannot be applied (the thread
    // stays held; choose another action).
    Result<void> resolve_reexecute(std::uint32_t id, const ReexecutePlan& plan);
    // Every held thread gets Ignore.
    void release_all() noexcept;
    // Threads waiting for an answer.
    std::size_t held_count() const noexcept;
    // Faults that found no free slot since the last call (they were passed on).
    std::uint32_t take_overflow_count() noexcept { return overflow_.exchange(0, std::memory_order_acq_rel); }

    // The context a Leave resumes with: `in` unwound one frame (RtlVirtualUnwind; a function without unwind information is a
    // leaf, whose return address is at RSP), RAX = 0, XMM0 = 0. Public so a test can check it without a fault.
    static Result<void> compute_leave(const CONTEXT& in, CONTEXT* out);

    // WP-23. The stack of `in`, innermost first: for every frame the registers the frame has at its `rip` (RtlVirtualUnwind restores the
    // callee-saved ones, so they are right for the outer frames too) and the RSP the function was entered with. Stops at a frame that
    // cannot be unwound, at RIP 0 or after `max` frames. Returns the number of frames written. Public for the unit test.
    static std::size_t unwind_frames(const CONTEXT& in, Frame* out, std::size_t max);
    // The context a Reexecute resumes with. Unwinds `in` to the frame of `plan.frame_index`, checks that the function was entered with
    // `plan.entry_rsp`, reads the parameters from [base register of that frame + offset], and builds the entry state of the function:
    // RSP = entry_rsp, the callee-saved registers its caller had, RIP = target, RCX/RDX/R8/R9 and XMM0..3 loaded. Public so a test can
    // check it without a fault.
    static Result<void> compute_reexecute(const CONTEXT& in, const ReexecutePlan& plan, CONTEXT* out);

    // The handler (exposed for the unit test that registers its own).
    static LONG CALLBACK handler(PEXCEPTION_POINTERS info);

private:
    ExceptionGate() = default;
    ExceptionGate(const ExceptionGate&) = delete;
    ExceptionGate& operator=(const ExceptionGate&) = delete;

    enum SlotState : int { kFree = 0, kFilling = 1, kPending = 2, kReported = 3 };

    struct Slot {
        std::atomic<int> state{kFree};
        std::atomic<std::int32_t> action{0};
        std::uint32_t id = 0;
        std::uint32_t thread_id = 0;
        std::uint32_t code = 0;
        std::uint32_t access = 0xFFFFFFFFu;
        std::uint64_t address = 0;
        std::uint64_t data_address = 0;
        std::uint64_t stack_pointer = 0;
        std::uint64_t module_base = 0;
        bool in_patch = false;
        HANDLE event = nullptr;  // auto-reset: signalled when `action` was set
        CONTEXT context;         // at the fault
        CONTEXT out_context;     // Leave: what to resume with
    };

    struct RangeEntry {
        std::atomic<std::uint64_t> base{0};
        std::atomic<std::uint64_t> end{0};
        std::atomic<bool> patch{false};
    };
    struct RangeTable {
        std::atomic<std::uint32_t> readers{0};  // handlers currently inside classify() on this table
        std::atomic<std::uint32_t> count{0};
        RangeEntry entries[kMaxRanges];
    };

    LONG on_exception(PEXCEPTION_POINTERS info);
    bool classify(std::uint64_t pc, std::uint64_t* base, bool* patch) const noexcept;
    bool is_ignored_thread(DWORD thread_id) const noexcept;
    Slot* claim_slot() noexcept;
    Slot* find_open(std::uint32_t id) noexcept;

    std::atomic<bool> installed_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> break_with_debugger_{false};
    std::atomic<HANDLE> notify_{nullptr};
    std::atomic<std::uint32_t> next_id_{1};
    std::atomic<std::uint32_t> overflow_{0};
    std::atomic<int> active_table_{0};
    mutable RangeTable tables_[2];  // readers is bumped by const lookups
    std::atomic<DWORD> ignored_threads_[kMaxIgnoredThreads] = {};
    Slot slots_[kMaxSlots];
    void* handler_cookie_ = nullptr;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace cml::agent::crash

#endif  // _WIN32
