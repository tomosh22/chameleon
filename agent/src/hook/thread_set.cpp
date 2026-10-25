#include "thread_set.h"

#if defined(_WIN32)

namespace cml::agent::hook {

namespace {

constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001A);
constexpr ACCESS_MASK kThreadAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
constexpr int kThreadIsTerminated = 20;  // THREADINFOCLASS::ThreadIsTerminated
constexpr unsigned kSuspendRetries = 64;

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE thread, int information_class, PVOID buffer, ULONG length,
                                                PULONG returned);

// Resolved once in init(); read-only afterwards.
NtQueryInformationThreadFn g_query_information_thread = nullptr;

template <class Fn>
Fn resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

}  // namespace

ThreadSet::~ThreadSet() {
    // Nothing may still be suspended: that would be a bug in the installer, and resuming here is the
    // lesser evil compared with leaving threads stopped for ever.
    resume_all();
    delete[] entries_;
}

bool ThreadSet::init(std::size_t capacity) {
    if (capacity == 0) {
        return false;
    }
    if (entries_ == nullptr || capacity_ != capacity) {
        delete[] entries_;
        entries_ = new Entry[capacity];
        capacity_ = capacity;
    }
    count_ = 0;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }
    get_next_thread_ = resolve<NtGetNextThreadFn>(ntdll, "NtGetNextThread");
    g_query_information_thread = resolve<NtQueryInformationThreadFn>(ntdll, "NtQueryInformationThread");
    return get_next_thread_ != nullptr;
}

bool ThreadSet::known(DWORD id) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].id == id) {
            return true;
        }
    }
    return false;
}

// True when the thread has finished or is being torn down: it will not run user code again, so its
// RIP is irrelevant and a failure to suspend it is not a failure of the hook.
bool ThreadSet::exited(HANDLE thread) const noexcept {
    DWORD code = 0;
    if (GetExitCodeThread(thread, &code) != 0 && code != STILL_ACTIVE) {
        return true;
    }
    if (g_query_information_thread != nullptr) {
        ULONG terminated = 0;
        if (g_query_information_thread(thread, kThreadIsTerminated, &terminated, sizeof(terminated), nullptr) >= 0 &&
            terminated != 0) {
            return true;
        }
    }
    return false;
}

bool ThreadSet::suspend_only(std::size_t index) noexcept {
    Entry& e = entries_[index];
    unsigned tries = 0;
    while (SuspendThread(e.handle) == static_cast<DWORD>(-1)) {
        failed_error_ = GetLastError();
        if (exited(e.handle)) {
            return true;
        }
        // A thread that is just starting to terminate may refuse for a moment before it reports it.
        if (++tries >= kSuspendRetries) {
            return false;
        }
        SwitchToThread();
    }
    e.suspended = true;
    e.rip_valid = false;
    return true;
}

bool ThreadSet::capture_context(std::size_t index) noexcept {
    Entry& e = entries_[index];
    // GetThreadContext on a thread whose suspension is still in flight waits for it to stop, so the
    // RIP read here is the one the thread will resume from.
    CONTEXT context;
    context.ContextFlags = CONTEXT_CONTROL;
    for (unsigned tries = 0; GetThreadContext(e.handle, &context) == 0;) {
        failed_error_ = GetLastError();
        if (exited(e.handle)) {
            e.rip = 0;
            e.rip_valid = true;
            return true;
        }
        if (++tries >= kSuspendRetries) {
            return false;
        }
        SwitchToThread();
    }
    e.rip = context.Rip;
    e.rip_valid = true;
    return true;
}

bool ThreadSet::suspend_entry(std::size_t index) noexcept {
    return suspend_only(index) && (!entries_[index].suspended || capture_context(index));
}

bool ThreadSet::capture_all() noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].suspended && !entries_[i].rip_valid && !capture_context(i)) {
            failed_thread_id_ = entries_[i].id;
            return false;
        }
    }
    return true;
}

ThreadSet::Result ThreadSet::enumerate_pass(std::size_t* fresh) noexcept {
    const HANDLE process = GetCurrentProcess();
    // The scan chains handles: NtGetNextThread(previous) returns the thread after `previous` in the
    // kernel's per-process list, which is why it takes a handle: the handle keeps the thread object
    // (alive or already exited) in the list, so the chain stays valid while threads come and go.
    // Two guards keep a kernel that behaves differently (an exited thread that is no longer listed)
    // from turning this into an endless loop: a step budget, and a check that the kernel did not
    // hand back the thread we came from. Either ends the pass with NotConverging.
    const std::size_t step_limit = capacity_ * 2 + 256;
    HANDLE previous = nullptr;
    bool previous_is_temporary = false;
    DWORD previous_id = 0;
    for (std::size_t step = 0; step < step_limit; ++step) {
        HANDLE next = nullptr;
        const LONG status = get_next_thread_(process, previous, kThreadAccess, 0, 0, &next);
        if (previous_is_temporary && previous != nullptr) {
            CloseHandle(previous);
        }
        const DWORD came_from = previous_id;
        previous = nullptr;
        previous_is_temporary = false;
        previous_id = 0;
        if (status == kStatusNoMoreEntries) {
            return Result::Ok;
        }
        if (status < 0 || next == nullptr) {
            failed_error_ = static_cast<DWORD>(status);
            return Result::EnumFailed;
        }
        const DWORD id = GetThreadId(next);
        if (id == 0) {
            failed_error_ = GetLastError();
            CloseHandle(next);
            return Result::EnumFailed;
        }
        if (came_from != 0 && id == came_from) {
            CloseHandle(next);
            return Result::NotConverging;
        }
        if (id == self_id_ || known(id)) {
            previous = next;  // used only to find the next thread, then closed
            previous_is_temporary = true;
            previous_id = id;
            continue;
        }
        if (count_ >= capacity_) {
            CloseHandle(next);
            return Result::TooManyThreads;
        }
        const std::size_t index = count_++;
        entries_[index].handle = next;
        entries_[index].id = id;
        entries_[index].rip = 0;
        entries_[index].suspended = false;
        entries_[index].rip_valid = false;
        ++*fresh;
        if (!suspend_only(index)) {
            failed_thread_id_ = id;
            return Result::SuspendFailed;  // the handle stays in the set; resume_all() releases it
        }
        previous = next;
        previous_id = id;
    }
    return Result::NotConverging;
}

ThreadSet::Result ThreadSet::suspend_all(ULONGLONG deadline_tick, unsigned max_passes,
                                         unsigned fake_new_passes) noexcept {
    if (!ready()) {
        return Result::EnumFailed;
    }
    self_id_ = GetCurrentThreadId();
    failed_thread_id_ = 0;
    failed_error_ = 0;
    passes_ = 0;
    for (unsigned pass = 0; pass < max_passes; ++pass) {
        if (GetTickCount64() > deadline_tick) {
            return Result::Timeout;
        }
        std::size_t fresh = 0;
        const Result r = enumerate_pass(&fresh);
        ++passes_;
        if (r != Result::Ok) {
            return r;
        }
        if (!capture_all()) {
            return Result::SuspendFailed;
        }
        if (pass < fake_new_passes) {
            ++fresh;
        }
        if (fresh == 0) {
            return Result::Ok;  // a whole pass saw only threads that were already stopped
        }
    }
    return Result::NotConverging;
}

void ThreadSet::resume_one(std::size_t index) noexcept {
    Entry& e = entries_[index];
    if (e.suspended) {
        ResumeThread(e.handle);
        e.suspended = false;
    }
}

bool ThreadSet::suspend_one(std::size_t index) noexcept {
    return suspend_entry(index);
}

void ThreadSet::resume_all() noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].suspended) {
            ResumeThread(entries_[i].handle);
            entries_[i].suspended = false;
        }
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].handle != nullptr) {
            CloseHandle(entries_[i].handle);
            entries_[i].handle = nullptr;
        }
    }
    count_ = 0;
}

}  // namespace cml::agent::hook

#endif  // _WIN32
