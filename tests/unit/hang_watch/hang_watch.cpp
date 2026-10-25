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
//   4. terminates the parent with exit code 3 and exits with 3.
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

void write_dump(HANDLE process, DWORD pid, const char* directory) {
    using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp != nullptr ? reinterpret_cast<WriteDumpFn>(
                                                reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                                          : nullptr;
    if (write == nullptr) {
        std::fprintf(stderr, "HANGWATCH: MiniDumpWriteDump is not available; no dump written\n");
        return;
    }
    CreateDirectoryA(directory, nullptr);
    char path[MAX_PATH + 64];
    std::snprintf(path, sizeof(path), "%s\\cml_hang_%lu.dmp", directory, static_cast<unsigned long>(pid));
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "HANGWATCH: cannot create %s (Win32 error %lu)\n", path, GetLastError());
        return;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithHandleData |
                                                 MiniDumpWithUnloadedModules);
    const BOOL ok = write(process, pid, file, type, nullptr, nullptr, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (ok) {
        std::fprintf(stderr, "HANGWATCH: minidump written to %s\n", path);
    } else {
        std::fprintf(stderr, "HANGWATCH: MiniDumpWriteDump failed (Win32 error %lu)\n", error);
    }
}

int watch(DWORD pid, unsigned long timeout_s, const char* directory) {
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
    write_dump(parent, pid, directory);
    std::fprintf(stderr, "HANGWATCH: terminating process %lu with exit code %d\n", static_cast<unsigned long>(pid),
                 kExitCode);
    std::fflush(stderr);
    TerminateProcess(parent, kExitCode);
    WaitForSingleObject(parent, 10000);
    CloseHandle(parent);
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
