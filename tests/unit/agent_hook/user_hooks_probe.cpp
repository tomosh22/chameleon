// See user_hooks_probe.h.
#if defined(_WIN32)

#include "user_hooks_probe.h"

#include "Chameleon_API.h"

#include <atomic>
#include <mutex>

namespace hooktest_user {

namespace {

std::mutex g_mutex;
std::vector<std::string> g_log;
std::vector<unsigned long> g_threads;
int (*g_probe)(int) = nullptr;
std::atomic<int> g_fault{0};
std::atomic<unsigned> g_compile_start_delay_ms{0};
std::mutex g_action_mutex;
Action g_action = nullptr;
void* g_action_context = nullptr;

void run_action(const char* where) {
    Action action = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_action_mutex);
        action = g_action;
        context = g_action_context;
    }
    if (action != nullptr) {
        action(context, where);
    }
}

// Overflows the stack for real; the base case keeps compilers from calling the recursion infinite.
__declspec(noinline) int recurse(int n) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(n);
    if (n > 0x7ffffff0) {
        return pad[0];
    }
    return recurse(n + 1) + pad[0];
}

void note(const std::string& what) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log.push_back(what);
    g_threads.push_back(GetCurrentThreadId());
}

std::string with_probe(const char* name) {
    int (*probe)(int) = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        probe = g_probe;
    }
    return std::string(name) + ":" + (probe != nullptr ? std::to_string(probe(1)) : std::string("-"));
}

void on_pre() {
    note(with_probe("pre"));
    run_action("pre");
}
void on_post_a() {
    note(with_probe("post-a"));
    run_action("post");
}
void on_post_fault() {
    const int mode = g_fault.load();
    if (mode == 1) {
        note("post-fault");
        RaiseException(0xE0000042u, 0, 0, nullptr);
    } else if (mode == 2) {
        note("post-overflow");
        note(std::to_string(recurse(0)));  // never returns normally
    }
}
void on_post_b() { note(with_probe("post-b")); }
void on_compile_start() {
    note("compile-start");
    run_action("compile-start");
    const unsigned delay = g_compile_start_delay_ms.load();
    if (delay != 0) {
        Sleep(delay);
        note("compile-start-done");
    }
}
void on_compile_success() { note("compile-success"); }
void on_compile_error(const char* text) { note(std::string("compile-error:") + (text != nullptr ? text : "(null)")); }
void on_pre_restart() { note("pre-restart"); }  // never called: restart is not implemented

CML_PREPATCH_HOOK(on_pre);
CML_POSTPATCH_HOOK(on_post_a);
CML_POSTPATCH_HOOK(on_post_fault);
CML_POSTPATCH_HOOK(on_post_b);
CML_COMPILE_START_HOOK(on_compile_start);
CML_COMPILE_SUCCESS_HOOK(on_compile_success);
CML_COMPILE_ERROR_HOOK(on_compile_error);
CML_PRERESTART_HOOK(on_pre_restart);

}  // namespace

void reset(int (*probe)(int)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log.clear();
    g_threads.clear();
    g_probe = probe;
    g_fault = 0;
    g_compile_start_delay_ms = 0;
    g_action = nullptr;
    g_action_context = nullptr;
}

std::vector<std::string> log() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_log;
}

std::vector<unsigned long> threads() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_threads;
}

void set_fault(int mode) {
    g_fault = mode;
}

void set_action(Action action, void* context) {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    g_action = action;
    g_action_context = context;
}

void set_compile_start_delay_ms(unsigned ms) {
    g_compile_start_delay_ms = ms;
}

std::string module_path_utf8() {
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    std::string utf8(static_cast<std::size_t>(length) * 4 + 4, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), &utf8[0], static_cast<int>(utf8.size()), nullptr,
                                      nullptr);
    utf8.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return utf8;
}

}  // namespace hooktest_user

#endif  // _WIN32
