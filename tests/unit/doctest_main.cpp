#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// Hang watchdog (Windows). Off unless CML_TEST_CASE_TIMEOUT_S is set to a number of seconds above
// 0; the unit-test registration in tests/unit/CMakeLists.txt sets it for every ctest test to that
// test's TIMEOUT minus 30, so the watchdog always speaks before ctest kills the process silently.
// The number is a budget that applies twice: to the running test case (restarted for each subcase
// pass) and to the whole executable, because ctest's TIMEOUT covers the whole executable.
//
// When the budget is exceeded the process names the test case, lists its thread ids, writes a
// minidump (all thread stacks; open it in WinDbg or Visual Studio) and exits with code 3. The dump
// goes to CML_TEST_DUMP_DIR, else the working directory (ctest's), else %TEMP%; CI sets
// CML_TEST_DUMP_DIR to the directory it uploads.
//
// The code that runs when the budget is exceeded (fire() and what it starts) must not depend on
// anything the hung test could be holding: no CRT, no heap, no C++ library. It uses fixed buffers,
// a hand-written number formatter, WriteFile, and plain CreateThread for the steps that could
// themselves block (each is waited for with a limit, and the process is terminated either way).
// Everything that needs the CRT or the heap is done up front in test_run_start().
#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdlib>
#include <string>

namespace {

constexpr unsigned kExitCode = 3;
constexpr std::size_t kNameCapacity = 512;
constexpr std::size_t kPathCapacity = 520;
constexpr DWORD kStepLimitMs = 60000;

using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Plain data only; written by the listener under g_lock, read by the watchdog thread.
SRWLOCK g_lock = SRWLOCK_INIT;
char g_name[kNameCapacity];
bool g_running = false;
ULONGLONG g_case_started_ms = 0;
ULONGLONG g_run_started_ms = 0;
unsigned long g_limit_s = 0;
char g_dump_paths[3][kPathCapacity];  // candidates, in order; empty = unused
WriteDumpFn g_write_dump = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_thread = nullptr;

// A line of output built in a stack buffer and written with WriteFile.
class Line {
public:
    Line& s(const char* text) {
        while (*text != '\0' && n_ < kCapacity - 1) {
            buffer_[n_++] = *text++;
        }
        return *this;
    }
    Line& u(unsigned long long value) {
        char digits[24];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + static_cast<int>(value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0 && n_ < kCapacity - 1) {
            buffer_[n_++] = digits[--count];
        }
        return *this;
    }
    void emit() const {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer_, static_cast<DWORD>(n_), &written, nullptr);
    }

private:
    static constexpr std::size_t kCapacity = kNameCapacity + 256;
    char buffer_[kCapacity];
    std::size_t n_ = 0;
};

void copy_text(char* destination, std::size_t capacity, const char* source) {
    std::size_t i = 0;
    for (; source[i] != '\0' && i + 1 < capacity; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

unsigned long parse_limit() {
    char text[32];
    const DWORD n = GetEnvironmentVariableA("CML_TEST_CASE_TIMEOUT_S", text, sizeof(text));
    if (n == 0 || n >= sizeof(text)) {
        return 0;  // unset: off
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    return (end != text && *end == '\0') ? value : 0;
}

DWORD WINAPI list_threads_step(LPVOID) {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Line().s("WATCHDOG: cannot list threads (Win32 error ").u(GetLastError()).s(")\n").emit();
        return 0;
    }
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    unsigned count = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid) {
            ++count;
            Line().s("WATCHDOG: thread ").u(entry.th32ThreadID).s(entry.th32ThreadID == self ? " (this one reports)" : "").s("\n").emit();
        }
    }
    CloseHandle(snapshot);
    Line().s("WATCHDOG: ").u(count).s(" thread(s)\n").emit();
    return 0;
}

DWORD WINAPI dump_step(LPVOID) {
    if (g_write_dump == nullptr) {
        Line().s("WATCHDOG: MiniDumpWriteDump is not available; no dump written\n").emit();
        return 0;
    }
    for (const auto& path : g_dump_paths) {
        if (path[0] == '\0') {
            continue;
        }
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            continue;
        }
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                              MiniDumpWithHandleData);
        const BOOL ok = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
        const DWORD error = GetLastError();
        CloseHandle(file);
        if (ok) {
            char full[kPathCapacity];
            const DWORD n = GetFullPathNameA(path, kPathCapacity, full, nullptr);
            Line().s("WATCHDOG: minidump written to ").s(n > 0 && n < kPathCapacity ? full : path).s("\n").emit();
            return 0;
        }
        Line().s("WATCHDOG: MiniDumpWriteDump into ").s(path).s(" failed (Win32 error ").u(error).s(")\n").emit();
    }
    Line().s("WATCHDOG: no minidump was written\n").emit();
    return 0;
}

// Runs `step` on its own thread and waits for it at most kStepLimitMs: a step that blocks on a lock
// the hung test holds must not keep the process alive.
void run_bounded(LPTHREAD_START_ROUTINE step) {
    HANDLE thread = CreateThread(nullptr, 0, step, nullptr, 0, nullptr);
    if (thread == nullptr) {
        return;
    }
    if (WaitForSingleObject(thread, kStepLimitMs) != WAIT_OBJECT_0) {
        Line().s("WATCHDOG: a diagnostic step did not finish within ").u(kStepLimitMs / 1000).s(" s; giving up on it\n").emit();
    }
    CloseHandle(thread);
}

void fire(const char* name, bool whole_run) {
    Line line;
    if (whole_run) {
        line.s("\nWATCHDOG: test run exceeded ").u(g_limit_s).s(" s (the budget of the whole executable), in test case '");
        line.s(name).s("'\n");
    } else {
        line.s("\nWATCHDOG: test case '").s(name).s("' exceeded ").u(g_limit_s).s(" s\n");
    }
    line.emit();
    run_bounded(&list_threads_step);
    run_bounded(&dump_step);
    Line().s("WATCHDOG: terminating the process with exit code ").u(kExitCode).s("\n").emit();
    TerminateProcess(GetCurrentProcess(), kExitCode);
}

DWORD WINAPI watch_thread(LPVOID) {
    const ULONGLONG limit_ms = static_cast<ULONGLONG>(g_limit_s) * 1000ULL;
    for (;;) {
        if (WaitForSingleObject(g_stop_event, 500) == WAIT_OBJECT_0) {
            return 0;
        }
        const ULONGLONG now = GetTickCount64();
        char name[kNameCapacity];
        bool case_over = false;
        bool run_over = false;
        AcquireSRWLockShared(&g_lock);
        case_over = g_running && now - g_case_started_ms > limit_ms;
        run_over = now - g_run_started_ms > limit_ms;
        if (case_over || run_over) {
            copy_text(name, kNameCapacity, g_running ? g_name : "(between test cases)");
        }
        ReleaseSRWLockShared(&g_lock);
        if (case_over || run_over) {
            fire(name, !case_over);
            return 0;
        }
    }
}

// Candidate dump paths, resolved now (this uses the CRT and the heap) so that fire() does not.
void prepare_dump_paths() {
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
        CreateDirectoryA(value, nullptr);  // CI's log directory may not exist yet
    }
    directories[1] = ".";
    const DWORD t = GetTempPathA(MAX_PATH, value);
    if (t > 0 && t < MAX_PATH) {
        directories[2] = value;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        g_dump_paths[i][0] = '\0';
        if (!directories[i].empty()) {
            copy_text(g_dump_paths[i], kPathCapacity, (directories[i] + "\\" + name).c_str());
        }
    }
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        g_write_dump = reinterpret_cast<WriteDumpFn>(reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    }
}

// The in-process watchdog is one of the threads the hook installer stops, so for the executables that
// stop the world (unit.agent_hook, unit.agent_hook_stress) the ctest registration also sets
// CML_TEST_HANG_WATCH_EXE: a helper process (tests/unit/hang_watch) that waits for this process, and
// when it is still running 10 s after the in-process budget it prints every thread's suspend count
// and RIP, writes a minidump and terminates this process (exit code 3). The helper inherits this
// process's standard handles, so its lines reach ctest's output. It exits by itself when this process does.
void start_external_watch(unsigned long limit_s) {
    char exe[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("CML_TEST_HANG_WATCH_EXE", exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    char directory[MAX_PATH] = ".";
    const DWORD d = GetEnvironmentVariableA("CML_TEST_DUMP_DIR", directory, MAX_PATH);
    if (d == 0 || d >= MAX_PATH) {
        copy_text(directory, MAX_PATH, ".");
    }
    std::string command = "\"" + std::string(exe) + "\" " + std::to_string(GetCurrentProcessId()) + " " +
                          std::to_string(limit_s + 10) + " \"" + directory + "\"";
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = INVALID_HANDLE_VALUE;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE h : {startup.hStdOutput, startup.hStdError}) {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }
    PROCESS_INFORMATION info = {};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
}

class Watchdog : public doctest::IReporter {
public:
    explicit Watchdog(const doctest::ContextOptions&) {}

    void test_run_start() override {
        g_limit_s = parse_limit();
        if (g_limit_s == 0) {
            return;
        }
        prepare_dump_paths();
        start_external_watch(g_limit_s);
        g_run_started_ms = GetTickCount64();
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_stop_event != nullptr) {
            g_thread = CreateThread(nullptr, 0, &watch_thread, nullptr, 0, nullptr);
        }
    }
    void test_run_end(const doctest::TestRunStats&) override {
        if (g_thread != nullptr) {
            SetEvent(g_stop_event);
            WaitForSingleObject(g_thread, 5000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        if (g_stop_event != nullptr) {
            CloseHandle(g_stop_event);
            g_stop_event = nullptr;
        }
    }
    void test_case_start(const doctest::TestCaseData& data) override {
        if (g_limit_s == 0) {
            return;
        }
        AcquireSRWLockExclusive(&g_lock);
        copy_text(g_name, kNameCapacity, data.m_name != nullptr ? data.m_name : "(unnamed)");
        g_case_started_ms = GetTickCount64();
        g_running = true;
        ReleaseSRWLockExclusive(&g_lock);
    }
    // Another pass over the test case for the next subcase: it gets its own clock.
    void test_case_reenter(const doctest::TestCaseData&) override {
        if (g_limit_s == 0) {
            return;
        }
        AcquireSRWLockExclusive(&g_lock);
        g_case_started_ms = GetTickCount64();
        g_running = true;
        ReleaseSRWLockExclusive(&g_lock);
    }
    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        if (g_limit_s == 0) {
            return;
        }
        AcquireSRWLockExclusive(&g_lock);
        g_running = false;
        ReleaseSRWLockExclusive(&g_lock);
    }

    void report_query(const doctest::QueryData&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

}  // namespace

DOCTEST_REGISTER_LISTENER("cml_hang_watchdog", 1, Watchdog);

#endif  // _WIN32
