#pragma once

// Makes the tests able to see an allocation made while the installer has the world stopped.
//
// Two paths, both watched, both only for the thread that owns the stopped window (the thread that
// set heap::set_alloc_forbidden(true)); threads that were not stopped yet may allocate as they like:
//   * The agent heap. The hook test executables link agent/src/operator_new.cpp, so every `new` /
//     `delete` in them goes through the agent's heap exactly as inside cml_agent.dll, and
//     heap.cpp's forbidden flag is really tested. heap::set_violation_hook_for_test routes a
//     violation here instead of the abort.
//   * The C runtime (malloc / calloc / realloc / free and everything built on them). Debug CRT only:
//     _CrtSetAllocHook is called for each of them. The Release CRT has no such hook; the Release
//     cells therefore check the agent heap path only (docs/wp/WP-10.md says so).
//
// Outside the negative tests a violation is fatal: the message is written and the process ends, so
// the test that allocated in the window fails (and a run cannot pass with one).
#if defined(_WIN32)

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "heap.h"

namespace hooktest {

struct AllocViolations {
    std::atomic<bool> counting{false};  // negative tests: count instead of ending the process
    std::atomic<int> agent_heap{0};
    std::atomic<int> crt{0};
};

inline AllocViolations g_alloc_violations;

[[noreturn]] inline void alloc_violation_fatal(const char* message, DWORD length) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, length, &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 0x0A110C);
    std::abort();
}

inline bool window_owner_is_this_thread() noexcept {
    return cml::agent::heap::alloc_forbidden_thread() == GetCurrentThreadId();
}

inline void on_agent_heap_violation() noexcept {
    if (!window_owner_is_this_thread()) {
        return;
    }
    if (g_alloc_violations.counting.load()) {
        ++g_alloc_violations.agent_heap;
        return;
    }
    static const char kMessage[] = "WP10-ALLOC FATAL: the agent heap was used while the world is stopped\n";
    alloc_violation_fatal(kMessage, sizeof(kMessage) - 1);
}

#if defined(_DEBUG)
inline int crt_alloc_hook(int, void*, std::size_t, int, long, const unsigned char*, int) noexcept {
    if (!window_owner_is_this_thread()) {
        return TRUE;
    }
    if (g_alloc_violations.counting.load()) {
        ++g_alloc_violations.crt;
        return TRUE;
    }
    static const char kMessage[] = "WP10-ALLOC FATAL: the C runtime heap was used while the world is stopped\n";
    alloc_violation_fatal(kMessage, sizeof(kMessage) - 1);
}
#endif

constexpr bool kCrtHookAvailable =
#if defined(_DEBUG)
    true;
#else
    false;
#endif

struct AllocGuardInstaller {
    AllocGuardInstaller() {
        cml::agent::heap::set_violation_hook_for_test(&on_agent_heap_violation);
#if defined(_DEBUG)
        _CrtSetAllocHook(&crt_alloc_hook);
#endif
    }
};
inline const AllocGuardInstaller g_alloc_guard_installer;

}  // namespace hooktest

#endif  // _WIN32
