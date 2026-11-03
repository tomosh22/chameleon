// cml_test_hang_watch: an out-of-process hang watchdog for the hook tests (Windows).
//
// The hook installer stops every other thread of its process. The in-process hang watchdog of
// doctest_main.cpp is one of those threads, so a hang in a stopped world produces no evidence at all.
// This helper runs in its OWN process, started by the test executable (doctest_main.cpp, when the
// environment variable CML_TEST_HANG_WATCH_EXE names it):
//
//   cml_test_hang_watch <parent pid> <timeout seconds> <dump directory>
//
// It waits for the parent to exit. When the timeout passes first it
//   1. lists the parent's modules,
//   2. prints, for every thread of the parent, the thread id, its suspend count and its RIP as
//      module+offset (SuspendThread returns the previous count, GetThreadContext reads the RIP,
//      ResumeThread puts the count back),
//   3. writes a minidump (thread info, handle data, unloaded modules) into the dump directory,
//   4. terminates the parent with exit code 3 (the dump is complete, nothing more is needed from it), then
//      runs cdb.exe on that dump, when cdb is installed, to print a symbolised stack of EVERY thread
//      (`cdb -z <dump> -y <exe dir>[;%_NT_SYMBOL_PATH%] -c "~*kn;q"`; the test executable's PDB sits next
//      to it). cdb writes straight to the helper's stderr, so a part of the output survives if ctest
//      kills the tree first. It is bounded by CML_HANG_CDB_BUDGET_S (default 15 s) AND by the time left
//      before ctest's own TIMEOUT (see cdb_budget_ms) and found through CML_CDB_EXE, WindowsSdkDir,
//      Program Files\Windows Kits\10 and PATH. Without cdb the lines of 2 are all there is (the output
//      says so),
//   5. exits with 3.
// Lines start with "HANGWATCH:" and go to the helper's stderr, which is the parent's stderr (inherited),
// so ctest and the CI log show them.
//
//   cml_test_hang_watch --victim
// is the self-test partner: it starts three threads, suspends them, prints READY and hangs.
#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

constexpr int kExitCode = 3;
constexpr std::size_t kMaxModules = 1024;

struct ModuleRange {
    std::uintptr_t base;
    std::uintptr_t size;
    char name[64];
};

ModuleRange g_modules[kMaxModules];
std::size_t g_module_count = 0;

void load_modules(HANDLE process) {
    static HMODULE handles[kMaxModules];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, handles, sizeof(handles), &needed, LIST_MODULES_ALL)) {
        std::fprintf(stderr, "HANGWATCH: cannot list the modules of the process (Win32 error %lu)\n", GetLastError());
        return;
    }
    const std::size_t count = needed / sizeof(HMODULE) < kMaxModules ? needed / sizeof(HMODULE) : kMaxModules;
    for (std::size_t i = 0; i < count; ++i) {
        MODULEINFO info = {};
        if (!GetModuleInformation(process, handles[i], &info, sizeof(info))) {
            continue;
        }
        ModuleRange& range = g_modules[g_module_count];
        range.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        range.size = info.SizeOfImage;
        range.name[0] = '\0';
        GetModuleBaseNameA(process, handles[i], range.name, sizeof(range.name));
        ++g_module_count;
    }
    std::fprintf(stderr, "HANGWATCH: %u module(s) in the process\n", static_cast<unsigned>(g_module_count));
}

void print_location(std::uintptr_t rip, char* out, std::size_t capacity) {
    for (std::size_t i = 0; i < g_module_count; ++i) {
        if (rip >= g_modules[i].base && rip - g_modules[i].base < g_modules[i].size) {
            std::snprintf(out, capacity, "%s+0x%llx", g_modules[i].name,
                          static_cast<unsigned long long>(rip - g_modules[i].base));
            return;
        }
    }
    std::snprintf(out, capacity, "(no module)");
}

void report_threads(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "HANGWATCH: cannot list threads (Win32 error %lu)\n", GetLastError());
        return;
    }
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    unsigned count = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        ++count;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   entry.th32ThreadID);
        if (thread == nullptr) {
            std::fprintf(stderr, "HANGWATCH: thread %lu cannot be opened (Win32 error %lu)\n", entry.th32ThreadID,
                         GetLastError());
            continue;
        }
        const DWORD previous = SuspendThread(thread);  // the suspend count the thread had
        char where[128] = "(context not available)";
        if (previous != static_cast<DWORD>(-1)) {
            CONTEXT context;
            std::memset(&context, 0, sizeof(context));
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread, &context)) {
                char location[96];
                print_location(static_cast<std::uintptr_t>(context.Rip), location, sizeof(location));
                std::snprintf(where, sizeof(where), "rip=0x%llx %s", static_cast<unsigned long long>(context.Rip),
                              location);
            }
            ResumeThread(thread);
            std::fprintf(stderr, "HANGWATCH: thread %lu suspend_count=%lu %s\n", entry.th32ThreadID,
                         static_cast<unsigned long>(previous), where);
        } else {
            std::fprintf(stderr, "HANGWATCH: thread %lu cannot be suspended to read it (Win32 error %lu)\n",
                         entry.th32ThreadID, GetLastError());
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    std::fprintf(stderr, "HANGWATCH: %u thread(s)\n", count);
}

// Returns true and the path of the dump in `path_out` when a dump was written.
bool write_dump(HANDLE process, DWORD pid, const char* directory, char* path_out, std::size_t path_capacity) {
    using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp != nullptr ? reinterpret_cast<WriteDumpFn>(
                                                reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                                          : nullptr;
    if (write == nullptr) {
        std::fprintf(stderr, "HANGWATCH: MiniDumpWriteDump is not available; no dump written\n");
        return false;
    }
    CreateDirectoryA(directory, nullptr);
    char path[MAX_PATH + 64];
    std::snprintf(path, sizeof(path), "%s\\cml_hang_%lu.dmp", directory, static_cast<unsigned long>(pid));
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "HANGWATCH: cannot create %s (Win32 error %lu)\n", path, GetLastError());
        return false;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithHandleData |
                                                 MiniDumpWithUnloadedModules);
    const BOOL ok = write(process, pid, file, type, nullptr, nullptr, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (ok) {
        std::fprintf(stderr, "HANGWATCH: minidump written to %s\n", path);
        std::snprintf(path_out, path_capacity, "%s", path);
    } else {
        std::fprintf(stderr, "HANGWATCH: MiniDumpWriteDump failed (Win32 error %lu)\n", error);
    }
    return ok != FALSE;
}

bool file_exists(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Where cdb.exe is, or false. Looked for in the order: CML_CDB_EXE, the Windows SDK the build
// environment points at (WindowsSdkDir), the default Windows Kits install, PATH.
bool find_cdb(char* out, std::size_t capacity) {
    char value[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("CML_CDB_EXE", value, sizeof(value));
    if (n > 0 && n < sizeof(value) && file_exists(value)) {
        std::snprintf(out, capacity, "%s", value);
        return true;
    }
    n = GetEnvironmentVariableA("WindowsSdkDir", value, sizeof(value));
    if (n > 0 && n < sizeof(value)) {
        const bool slash = value[n - 1] == '\\' || value[n - 1] == '/';
        std::snprintf(out, capacity, "%s%sDebuggers\\x64\\cdb.exe", value, slash ? "" : "\\");
        if (file_exists(out)) {
            return true;
        }
    }
    for (const char* variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
        n = GetEnvironmentVariableA(variable, value, sizeof(value));
        if (n > 0 && n < sizeof(value)) {
            std::snprintf(out, capacity, "%s\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe", value);
            if (file_exists(out)) {
                return true;
            }
        }
    }
    // SearchPathA returns the length needed when the buffer is too small (>= capacity): not found.
    const DWORD found = SearchPathA(nullptr, "cdb.exe", nullptr, static_cast<DWORD>(capacity), out, nullptr);
    return found > 0 && found < capacity;
}

constexpr unsigned long kMinCdbBudgetMs = 3000;
constexpr unsigned long kCtestMarginMs = 5000;  // left before ctest's TIMEOUT when everything is done

// How long cdb may run. The unit-test registration (tests/unit/CMakeLists.txt) sets
// CML_TEST_CASE_TIMEOUT_S to the ctest TIMEOUT minus 30, and doctest_main.cpp starts this helper with
// timeout_s = that + 10. So the helper fires at TIMEOUT - 20 s, and ctest kills the whole tree at
// TIMEOUT. The time between firing and then is spent on: modules, thread lines and the dump (a few
// seconds), terminating the parent (waited for at most 2 s), cdb, and a margin of 5 s. The budget is
// therefore the time still left, measured from the moment this helper started (a little after the test
// process started, so the real end is a little earlier than computed; the margin covers it), and at most
// CML_HANG_CDB_BUDGET_S (default 15 s). With the registration's numbers it comes to about 10 s. Without
// CML_TEST_CASE_TIMEOUT_S (self-test, manual use) there is no ctest limit and only the cap applies.
unsigned long cdb_budget_ms(ULONGLONG helper_started_ms) {
    unsigned long cap_ms = 15000;
    char text[16] = {};
    DWORD n = GetEnvironmentVariableA("CML_HANG_CDB_BUDGET_S", text, sizeof(text));
    if (n > 0 && n < sizeof(text)) {
        const unsigned long value = std::strtoul(text, nullptr, 10);
        if (value > 0) {
            cap_ms = value * 1000UL;
        }
    }
    n = GetEnvironmentVariableA("CML_TEST_CASE_TIMEOUT_S", text, sizeof(text));
    if (n > 0 && n < sizeof(text)) {
        const unsigned long limit_s = std::strtoul(text, nullptr, 10);
        if (limit_s > 0) {
            const ULONGLONG ctest_end = helper_started_ms + (static_cast<ULONGLONG>(limit_s) + 30) * 1000ULL;
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG usable = ctest_end > now + kCtestMarginMs ? ctest_end - now - kCtestMarginMs : 0;
            if (usable < cap_ms) {
                cap_ms = static_cast<unsigned long>(usable);
            }
        }
    }
    return cap_ms;
}

// Runs cdb on the dump and lets it print the symbolised stack of every thread ("~*kn") to this
// process's stderr. Gives up on it after the budget.
void run_cdb(const char* directory, const char* dump, unsigned long budget_ms) {
    char cdb[MAX_PATH] = {};
    if (!find_cdb(cdb, sizeof(cdb))) {
        std::fprintf(stderr, "HANGWATCH: cdb.exe not found; no symbolised stacks (the lines above and the dump are all there is)\n");
        return;
    }
    if (budget_ms < kMinCdbBudgetMs) {
        std::fprintf(stderr, "HANGWATCH: only %lu ms are left before ctest's timeout; cdb is not run (open the dump)\n", budget_ms);
        return;
    }
    char symbols[2 * MAX_PATH] = {};
    char configured[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("_NT_SYMBOL_PATH", configured, sizeof(configured));
    if (n > 0 && n < sizeof(configured)) {
        std::snprintf(symbols, sizeof(symbols), "%s;%s", directory, configured);
    } else {
        std::snprintf(symbols, sizeof(symbols), "%s", directory);
    }
    static char command[8 * MAX_PATH];
    std::snprintf(command, sizeof(command), "\"%s\" -z \"%s\" -y \"%s\" -i \"%s\" -lines -c \"~*kn;q\"", cdb, dump,
                  symbols, directory);
    HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = INVALID_HANDLE_VALUE;
    startup.hStdOutput = out;
    startup.hStdError = out;
    PROCESS_INFORMATION info = {};
    std::fprintf(stderr, "HANGWATCH: cdb stacks begin (%s, symbols %s, budget %lu ms)\n", cdb, symbols, budget_ms);
    std::fflush(stderr);
    if (!CreateProcessA(nullptr, command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        std::fprintf(stderr, "HANGWATCH: cdb stacks end: cannot start cdb (Win32 error %lu)\n", GetLastError());
        return;
    }
    CloseHandle(info.hThread);
    if (WaitForSingleObject(info.hProcess, static_cast<DWORD>(budget_ms)) != WAIT_OBJECT_0) {
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 1000);
        std::fprintf(stderr, "\nHANGWATCH: cdb stacks end: cdb did not finish within %lu ms; its output above is partial\n",
                     budget_ms);
    } else {
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        std::fprintf(stderr, "\nHANGWATCH: cdb stacks end: cdb exit code %lu\n", static_cast<unsigned long>(code));
    }
    std::fflush(stderr);
    CloseHandle(info.hProcess);
}

int watch(DWORD pid, unsigned long timeout_s, const char* directory) {
    const ULONGLONG started = GetTickCount64();
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE |
                                    PROCESS_TERMINATE,
                                FALSE, pid);
    if (parent == nullptr) {
        std::fprintf(stderr, "HANGWATCH: cannot open process %lu (Win32 error %lu)\n", static_cast<unsigned long>(pid),
                     GetLastError());
        return 1;
    }
    const DWORD waited = WaitForSingleObject(parent, static_cast<DWORD>(timeout_s * 1000UL));
    if (waited == WAIT_OBJECT_0) {
        CloseHandle(parent);
        return 0;  // the parent ended by itself
    }
    std::fprintf(stderr, "\nHANGWATCH: process %lu is still running after %lu s\n", static_cast<unsigned long>(pid),
                 timeout_s);
    std::fflush(stderr);
    load_modules(parent);
    report_threads(pid);
    char dump_path[MAX_PATH + 64] = {};
    const bool dumped = write_dump(parent, pid, directory, dump_path, sizeof(dump_path));
    // The directory of the test executable (its PDB is next to it) must be read while the process lives.
    char image_directory[MAX_PATH] = ".";
    char image[MAX_PATH] = {};
    if (GetModuleFileNameExA(parent, nullptr, image, sizeof(image)) > 0) {
        std::snprintf(image_directory, sizeof(image_directory), "%s", image);
        char* slash = std::strrchr(image_directory, '\\');
        if (slash != nullptr) {
            *slash = '\0';
        }
    }
    // Terminate first: the dump is complete, and ctest must see the failure even if it kills the tree
    // while cdb is still running.
    std::fprintf(stderr, "HANGWATCH: terminating process %lu with exit code %d\n", static_cast<unsigned long>(pid),
                 kExitCode);
    std::fflush(stderr);
    TerminateProcess(parent, kExitCode);
    WaitForSingleObject(parent, 2000);
    CloseHandle(parent);
    if (dumped) {
        run_cdb(image_directory, dump_path, cdb_budget_ms(started));
    }
    return kExitCode;
}

// The self-test partner: three threads that are suspended by the main thread, as the installer does
// it, and then the main thread hangs.
DWORD WINAPI sleeper(LPVOID) {
    Sleep(INFINITE);
    return 0;
}

int victim() {
    HANDLE threads[3];
    for (HANDLE& t : threads) {
        t = CreateThread(nullptr, 0, &sleeper, nullptr, 0, nullptr);
    }
    Sleep(200);  // let them reach their wait
    for (HANDLE t : threads) {
        SuspendThread(t);
    }
    std::fprintf(stdout, "READY\n");
    std::fflush(stdout);
    Sleep(INFINITE);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--victim") == 0) {
        return victim();
    }
    if (argc != 4) {
        std::fprintf(stderr, "usage: cml_test_hang_watch <parent pid> <timeout seconds> <dump directory>\n");
        return 2;
    }
    return watch(static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10)), std::strtoul(argv[2], nullptr, 10), argv[3]);
}

#else

int main() {
    return 0;
}

#endif  // _WIN32
