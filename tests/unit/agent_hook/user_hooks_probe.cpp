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
std::atomic<bool> g_fault{false};

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

void on_pre() { note(with_probe("pre")); }
void on_post_a() { note(with_probe("post-a")); }
void on_post_fault() {
    if (g_fault.load()) {
        note("post-fault");
        RaiseException(0xE0000042u, 0, 0, nullptr);
    }
}
void on_post_b() { note(with_probe("post-b")); }
void on_compile_start() { note("compile-start"); }
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
    g_fault = false;
}

std::vector<std::string> log() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_log;
}

std::vector<unsigned long> threads() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_threads;
}

void set_fault(bool on) {
    g_fault = on;
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
