#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// Hang watchdog (Windows). A test case that runs longer than CML_TEST_CASE_TIMEOUT_S seconds
// (default 120, 0 = off) is a hang, not a slow test: the process then says which test case it was,
// lists its thread ids, writes a minidump (all thread stacks; open it in WinDbg or Visual Studio)
// and exits with code 3, instead of sitting silent until ctest's TIMEOUT kills it with no
// evidence. The dump goes to CML_TEST_DUMP_DIR, else the working directory (ctest's), else %TEMP%.
// CI sets CML_TEST_DUMP_DIR to the directory it uploads (.github/workflows/ci.yml).
#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr unsigned long kDefaultTimeoutSeconds = 120;
constexpr unsigned kExitCode = 3;

// Written with WriteFile, not stdio: by then a thread of the hung test may hold a CRT lock.
void say(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer,
                  static_cast<DWORD>(n < static_cast<int>(sizeof(buffer)) ? n : static_cast<int>(sizeof(buffer)) - 1),
                  &written, nullptr);
    }
}

unsigned long timeout_seconds() {
    char text[32];
    const DWORD n = GetEnvironmentVariableA("CML_TEST_CASE_TIMEOUT_S", text, sizeof(text));
    if (n == 0 || n >= sizeof(text)) {
        return kDefaultTimeoutSeconds;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    return (end != text && *end == '\0') ? value : kDefaultTimeoutSeconds;
}

void list_threads() {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        say("WATCHDOG: cannot list threads (Win32 error %lu)\n", GetLastError());
        return;
    }
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    unsigned count = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid) {
            ++count;
            say("WATCHDOG: thread %lu%s\n", entry.th32ThreadID, entry.th32ThreadID == self ? " (watchdog)" : "");
        }
    }
    CloseHandle(snapshot);
    say("WATCHDOG: %u thread(s)\n", count);
}

// Tries each directory in turn; returns the path written, empty if none worked.
std::string write_dump() {
    using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (dbghelp == nullptr) {
        say("WATCHDOG: dbghelp.dll is not available (Win32 error %lu); no dump written\n", GetLastError());
        return {};
    }
    const auto write = reinterpret_cast<WriteDumpFn>(reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (write == nullptr) {
        say("WATCHDOG: MiniDumpWriteDump is not available; no dump written\n");
        return {};
    }

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string name = exe;
    name = name.substr(name.find_last_of("\\/") + 1);
    name = "cml_hang_" + name + "_" + std::to_string(GetCurrentProcessId()) + ".dmp";

    std::string directories[3];
    char value[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("CML_TEST_DUMP_DIR", value, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        directories[0] = value;
    }
    directories[1] = ".";
    const DWORD t = GetTempPathA(MAX_PATH, value);
    if (t > 0 && t < MAX_PATH) {
        directories[2] = value;
    }
    for (const std::string& directory : directories) {
        if (directory.empty()) {
            continue;
        }
        CreateDirectoryA(directory.c_str(), nullptr);  // CI's log directory may not exist yet
        char full[MAX_PATH] = {};
        const std::string path = directory + "\\" + name;
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            continue;
        }
        const DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr);
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                              MiniDumpWithHandleData);
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
        const DWORD error = GetLastError();
        CloseHandle(file);
        if (ok) {
            return (length > 0 && length < MAX_PATH) ? std::string(full) : path;
        }
        say("WATCHDOG: MiniDumpWriteDump into %s failed (Win32 error %lu)\n", path.c_str(), error);
    }
    return {};
}

class Watchdog : public doctest::IReporter {
public:
    explicit Watchdog(const doctest::ContextOptions&) {}

    void test_run_start() override {
        limit_seconds_ = timeout_seconds();
        if (limit_seconds_ == 0) {
            return;
        }
        thread_ = std::thread([this] { watch(); });
    }
    void test_run_end(const doctest::TestRunStats&) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void test_case_start(const doctest::TestCaseData& data) override { begin(data); }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }

    void report_query(const doctest::QueryData&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}

private:
    void begin(const doctest::TestCaseData& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        name_ = data.m_name != nullptr ? data.m_name : "(unnamed)";
        started_ = std::chrono::steady_clock::now();
        running_ = true;
    }

    void watch() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!done_) {
            wake_.wait_for(lock, std::chrono::milliseconds(500));
            if (done_) {
                break;
            }
            if (running_ && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(limit_seconds_)) {
                const std::string name = name_;
                lock.unlock();
                fire(name);  // the process is gone after this; the return is only for the compiler
                return;
            }
        }
    }

    void fire(const std::string& name) const {
        say("\nWATCHDOG: test case '%s' exceeded %lu s\n", name.c_str(), limit_seconds_);
        list_threads();
        // The dump suspends every other thread of this process; if one of them holds a lock the
        // dump needs, it would never finish. So it runs on its own thread, and the process is
        // terminated whether or not it did.
        std::string path;
        std::mutex done_mutex;
        std::condition_variable done_cv;
        bool finished = false;
        std::thread dumper([&] {
            std::string written = write_dump();
            std::lock_guard<std::mutex> lock(done_mutex);
            path = std::move(written);
            finished = true;
            done_cv.notify_all();
        });
        dumper.detach();  // never joined: the process ends below
        {
            std::unique_lock<std::mutex> lock(done_mutex);
            done_cv.wait_for(lock, std::chrono::seconds(60), [&] { return finished; });
            if (finished) {
                if (path.empty()) {
                    say("WATCHDOG: no minidump was written\n");
                } else {
                    say("WATCHDOG: minidump written to %s\n", path.c_str());
                }
            } else {
                say("WATCHDOG: the minidump did not finish within 60 s; giving up on it\n");
            }
        }
        say("WATCHDOG: terminating the process with exit code %u\n", kExitCode);
        TerminateProcess(GetCurrentProcess(), kExitCode);
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    unsigned long limit_seconds_ = kDefaultTimeoutSeconds;
    bool done_ = false;
    bool running_ = false;
    std::string name_;
    std::chrono::steady_clock::time_point started_;
};

}  // namespace

DOCTEST_REGISTER_LISTENER("cml_hang_watchdog", 1, Watchdog);

#endif  // _WIN32
