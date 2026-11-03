#include "crash/exception_gate.h"

#if defined(_WIN32)

#include <cstring>
#include <string>
#include <new>

#include "crash/diag_ids.h"
#include "cml/util/format.h"
#include "heap.h"

namespace cml::agent::crash {

namespace {

// Set once by install(); the handler reads it (a function-local static would take a lock in the handler's context).
std::atomic<ExceptionGate*> g_instance{nullptr};

constexpr DWORD kWaitSliceMs = 500;  // how often a held thread looks at the gate while it waits (belt and braces)

bool is_handled_code(DWORD code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION;
}

bool executable_code(std::uint64_t address) noexcept {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) != sizeof info) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

}  // namespace

ExceptionGate& ExceptionGate::shared() {
    // Static storage, constructed once, never destroyed: threads may still be inside the handler during process exit.
    alignas(ExceptionGate) static unsigned char storage[sizeof(ExceptionGate)];
    static ExceptionGate* instance = new (storage) ExceptionGate();
    return *instance;
}

Result<void> ExceptionGate::install() {
    if (installed_.load(std::memory_order_acquire)) {
        return Result<void>();
    }
    for (Slot& slot : slots_) {
        if (slot.event == nullptr) {
            slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (slot.event == nullptr) {
                return Diag::make(ids::kNoSlotEvent, "cannot create the events the crash handler waits on (error " + std::to_string(GetLastError()) + ")",
                                  "Close other programs to free system resources; crash handling stays off.");
            }
        }
    }
    g_instance.store(this, std::memory_order_release);
    handler_cookie_ = AddVectoredExceptionHandler(1, &ExceptionGate::handler);
    if (handler_cookie_ == nullptr) {
        g_instance.store(nullptr, std::memory_order_release);
        return Diag::make(ids::kHandlerInstallFailed, "AddVectoredExceptionHandler failed (error " + std::to_string(GetLastError()) + ")",
                          "Crash handling stays off; the application's faults are unaffected.");
    }
    installed_.store(true, std::memory_order_release);
    return Result<void>();
}

void ExceptionGate::configure(bool enabled, bool break_with_debugger) {
    break_with_debugger_.store(break_with_debugger, std::memory_order_release);
    enabled_.store(enabled && installed(), std::memory_order_release);
    if (!enabled) {
        release_all();
    }
}

void ExceptionGate::set_ranges(const Range* ranges, std::size_t count) {
    // Write the table that is not in use, then flip: the handler reads one table from start to end without a lock.
    const int next = 1 - active_table_.load(std::memory_order_acquire);
    RangeTable& table = tables_[next];
    // A handler that read the old value of active_table_ may still be counted in on this table: wait for it to leave (this is the
    // agent thread, which may sleep; the handler never waits for anything).
    while (table.readers.load() != 0) {
        Sleep(0);
    }
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < count && n < kMaxRanges; ++i) {
        if (ranges[i].size == 0) {
            continue;
        }
        table.entries[n].base.store(ranges[i].base, std::memory_order_relaxed);
        table.entries[n].end.store(ranges[i].base + ranges[i].size, std::memory_order_relaxed);
        table.entries[n].patch.store(ranges[i].patch, std::memory_order_relaxed);
        ++n;
    }
    table.count.store(n, std::memory_order_release);
    active_table_.store(next, std::memory_order_release);
}

std::size_t ExceptionGate::range_count() const noexcept {
    return tables_[active_table_.load(std::memory_order_acquire)].count.load(std::memory_order_acquire);
}

void ExceptionGate::add_ignored_thread(std::uint32_t thread_id) noexcept {
    for (std::atomic<DWORD>& entry : ignored_threads_) {
        DWORD expected = 0;
        if (entry.compare_exchange_strong(expected, thread_id, std::memory_order_acq_rel) || expected == thread_id) {
            return;
        }
    }
}

void ExceptionGate::remove_ignored_thread(std::uint32_t thread_id) noexcept {
    for (std::atomic<DWORD>& entry : ignored_threads_) {
        DWORD expected = thread_id;
        entry.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
}

bool ExceptionGate::is_ignored_thread(DWORD thread_id) const noexcept {
    for (const std::atomic<DWORD>& entry : ignored_threads_) {
        if (entry.load(std::memory_order_acquire) == thread_id) {
            return true;
        }
    }
    return false;
}

bool ExceptionGate::classify(std::uint64_t pc, std::uint64_t* base, bool* patch) const noexcept {
    // Count in as a reader of the table that is active NOW; if the writer flipped meanwhile, count out and look again.
    RangeTable* table = nullptr;
    for (;;) {
        const int index = active_table_.load();
        table = &tables_[index];
        table->readers.fetch_add(1);
        if (active_table_.load() == index) {
            break;
        }
        table->readers.fetch_sub(1);
    }
    bool found = false;
    const std::uint32_t count = table->count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t b = table->entries[i].base.load(std::memory_order_relaxed);
        const std::uint64_t e = table->entries[i].end.load(std::memory_order_relaxed);
        if (pc >= b && pc < e) {
            *base = b;
            *patch = table->entries[i].patch.load(std::memory_order_relaxed);
            found = true;
            break;
        }
    }
    table->readers.fetch_sub(1);
    return found;
}

ExceptionGate::Slot* ExceptionGate::claim_slot() noexcept {
    for (Slot& slot : slots_) {
        int expected = kFree;
        if (slot.state.compare_exchange_strong(expected, kFilling, std::memory_order_acq_rel)) {
            return &slot;
        }
    }
    return nullptr;
}

LONG CALLBACK ExceptionGate::handler(PEXCEPTION_POINTERS info) {
    ExceptionGate* gate = g_instance.load(std::memory_order_acquire);
    if (gate == nullptr || !gate->enabled_.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return gate->on_exception(info);
}

LONG ExceptionGate::on_exception(PEXCEPTION_POINTERS info) {
    // Nothing in here allocates, logs, takes a lock or calls the broker (see the header).
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!is_handled_code(record->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (heap::alloc_forbidden()) {
        return EXCEPTION_CONTINUE_SEARCH;  // the hook installer has the other threads stopped
    }
    const DWORD thread_id = GetCurrentThreadId();
    if (is_ignored_thread(thread_id)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uint64_t pc = reinterpret_cast<std::uint64_t>(record->ExceptionAddress);
    std::uint64_t module_base = 0;
    bool in_patch = false;
    if (!classify(pc, &module_base, &in_patch)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!break_with_debugger_.load(std::memory_order_acquire) && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // A fault inside this very handler (the thread already holds a slot) is not ours to hold again.
    for (const Slot& slot : slots_) {
        const int state = slot.state.load(std::memory_order_acquire);
        if ((state == kPending || state == kReported) && slot.thread_id == thread_id) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    Slot* slot = claim_slot();
    if (slot == nullptr) {
        overflow_.fetch_add(1, std::memory_order_acq_rel);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    slot->id = next_id_.fetch_add(1, std::memory_order_acq_rel);
    slot->thread_id = thread_id;
    slot->code = record->ExceptionCode;
    slot->address = pc;
    slot->module_base = module_base;
    slot->in_patch = in_patch;
    slot->access = 0xFFFFFFFFu;
    slot->data_address = 0;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        slot->access = static_cast<std::uint32_t>(record->ExceptionInformation[0]);
        slot->data_address = static_cast<std::uint64_t>(record->ExceptionInformation[1]);
    }
    slot->stack_pointer = info->ContextRecord->Rsp;
    slot->context = *info->ContextRecord;
    slot->action.store(static_cast<std::int32_t>(Action::None), std::memory_order_release);
    slot->state.store(kPending, std::memory_order_release);
    if (const HANDLE notify = notify_.load(std::memory_order_acquire)) {
        SetEvent(notify);
    }
    for (;;) {
        if (slot->action.load(std::memory_order_acquire) != static_cast<std::int32_t>(Action::None)) {
            break;
        }
        // (A wake-up without an answer is a stale signal from an earlier occupant of this slot: wait again.)
        WaitForSingleObject(slot->event, kWaitSliceMs);
        if (!enabled_.load(std::memory_order_acquire)) {
            // The gate was switched off and the agent thread has not answered yet: pass the fault on. A no-op when an
            // answer is already there.
            std::int32_t none = static_cast<std::int32_t>(Action::None);
            slot->action.compare_exchange_strong(none, static_cast<std::int32_t>(Action::Ignore), std::memory_order_acq_rel);
        }
    }
    const Action action = static_cast<Action>(slot->action.load(std::memory_order_acquire));
    LONG result = EXCEPTION_CONTINUE_SEARCH;
    if (action == Action::Continue) {
        result = EXCEPTION_CONTINUE_EXECUTION;
    } else if (action == Action::Leave) {
        *info->ContextRecord = slot->out_context;
        result = EXCEPTION_CONTINUE_EXECUTION;
    }
    slot->state.store(kFree, std::memory_order_release);
    return result;
}

bool ExceptionGate::take_new(Held* out) {
    for (Slot& slot : slots_) {
        int expected = kPending;
        if (!slot.state.compare_exchange_strong(expected, kReported, std::memory_order_acq_rel)) {
            continue;
        }
        out->id = slot.id;
        out->thread_id = slot.thread_id;
        out->code = slot.code;
        out->access = slot.access;
        out->address = slot.address;
        out->data_address = slot.data_address;
        out->stack_pointer = slot.stack_pointer;
        out->module_base = slot.module_base;
        out->in_patch = slot.in_patch;
        return true;
    }
    return false;
}

ExceptionGate::Slot* ExceptionGate::find_open(std::uint32_t id) noexcept {
    for (Slot& slot : slots_) {
        const int state = slot.state.load(std::memory_order_acquire);
        if ((state == kPending || state == kReported) && slot.id == id &&
            slot.action.load(std::memory_order_acquire) == static_cast<std::int32_t>(Action::None)) {
            return &slot;
        }
    }
    return nullptr;
}

Result<void> ExceptionGate::resolve(std::uint32_t id, Action action) {
    Slot* slot = find_open(id);
    if (slot == nullptr) {
        return Diag::make(ids::kUnknownException, "no thread is held for exception " + std::to_string(id),
                          "It was answered already, or its process went through a reconnect; query `exceptions` for the open ones.");
    }
    if (action == Action::Leave) {
        Result<void> left = compute_leave(slot->context, &slot->out_context);
        if (!left) {
            return left.error();
        }
    }
    slot->action.store(static_cast<std::int32_t>(action), std::memory_order_release);
    SetEvent(slot->event);
    return Result<void>();
}

void ExceptionGate::release_all() noexcept {
    for (Slot& slot : slots_) {
        const int state = slot.state.load(std::memory_order_acquire);
        if (state != kPending && state != kReported) {
            continue;
        }
        std::int32_t none = static_cast<std::int32_t>(Action::None);
        if (slot.action.compare_exchange_strong(none, static_cast<std::int32_t>(Action::Ignore), std::memory_order_acq_rel)) {
            SetEvent(slot.event);
        }
    }
}

std::size_t ExceptionGate::held_count() const noexcept {
    std::size_t n = 0;
    for (const Slot& slot : slots_) {
        const int state = slot.state.load(std::memory_order_acquire);
        if ((state == kPending || state == kReported) && slot.action.load(std::memory_order_acquire) == static_cast<std::int32_t>(Action::None)) {
            ++n;
        }
    }
    return n;
}

Result<void> ExceptionGate::compute_leave(const CONTEXT& in, CONTEXT* out) {
    *out = in;
    CONTEXT& ctx = *out;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (function != nullptr) {
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, function, &ctx, &handler_data, &establisher_frame, nullptr);
    } else {
        // No unwind information: a leaf function, which has not touched RSP, so the return address is on top of the stack.
        DWORD64 return_address = 0;
        SIZE_T read = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(in.Rsp), &return_address, sizeof return_address, &read) == 0 ||
            read != sizeof return_address) {
            return Diag::make(ids::kLeaveUnwindFailed,
                              "cannot leave the function at " + cml::hex(in.Rip) + ": it has no unwind information and the stack top is unreadable",
                              "Use Ignore, or Continue after a patch fixed the cause.");
        }
        ctx.Rip = return_address;
        ctx.Rsp += 8;
    }
    if (ctx.Rip == 0 || ctx.Rsp <= in.Rsp || !executable_code(ctx.Rip)) {
        return Diag::make(ids::kLeaveBadFrame,
                          "unwinding the faulting function did not lead to a caller (return address " + cml::hex(ctx.Rip) + ")",
                          "The stack looks damaged or the frame is not described by unwind data; use Ignore, or Continue after a patch.");
    }
    ctx.Rax = 0;
    ctx.Xmm0.Low = 0;
    ctx.Xmm0.High = 0;
    return Result<void>();
}

}  // namespace cml::agent::crash

#endif  // _WIN32
