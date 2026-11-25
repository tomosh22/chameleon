// Fixture `crash` (WP-20): see CMakeLists.txt. Prints `TICK n=<k> cell=<v> armed=<0|1>`.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "fixture_runtime.h"
#include "logic.h"

char* g_cell = nullptr;
std::atomic<bool> g_repaired{false};
std::atomic<int> g_maintain_calls{0};

namespace {

bool g_armed = false;
bool g_leave_mode = false;
bool g_seh_mode = false;  // WP-44: the application handles a fault the crash handler passes on (failure injection, exception-hold)
wchar_t g_signal_file[4096] = L"";

void set_cell_access(DWORD protection)
{
    DWORD old = 0;
    VirtualProtect(g_cell, 4096, protection, &old);
}

// The helper thread: the "service" whose maintain() the patch fixes. It reports the repair once, from the unpatched TU.
void helper()
{
    bool announced = false;
    for (;;) {
        maintain();
        if (!announced && g_repaired.load()) {
            announced = true;
            std::printf("REPAIRED\n");
        }
        Sleep(10);
    }
}

// WP-44 (--seh): the application's own structured exception handler around the store. A fault the crash handler passes on reaches
// it, as it would without Chameleon; the code of the fault is returned in `code` (0: no fault). No C++ objects in here (__try).
int guarded_store(int value, DWORD* code)
{
    *code = 0;
    __try {
        return cell_store(value);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        *code = EXCEPTION_ACCESS_VIOLATION;
        return -1;
    }
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--leave") == 0) {
            g_leave_mode = true;
        }
        if (std::strcmp(argv[i], "--seh") == 0) {
            g_seh_mode = true;
        }
    }
    GetEnvironmentVariableW(L"CML_FIXTURE_SIGNAL_FILE", g_signal_file, 4096);
    g_cell = static_cast<char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_cell == nullptr) {
        std::printf("FIXTURE cannot allocate the page\n");
        return 1;
    }
    std::thread(helper).detach();

    return cml_fixture::run(argc, argv, [](int n) {
        // The harness' signal file arms the fixture (the broker must be listening for exceptions by then).
        if (!g_armed && g_signal_file[0] != L'\0' && GetFileAttributesW(g_signal_file) != INVALID_FILE_ATTRIBUTES) {
            g_armed = true;
            set_cell_access(PAGE_NOACCESS);
            std::printf("ARMED\n");
            if (g_seh_mode) {
                DeleteFileW(g_signal_file);  // --seh: every new signal arms the fixture again
            }
        } else if (g_seh_mode && g_armed && g_signal_file[0] != L'\0' && GetFileAttributesW(g_signal_file) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(g_signal_file);
            set_cell_access(PAGE_NOACCESS);
            std::printf("ARMED again\n");
        }
        int value = 0;
        if (g_seh_mode) {
            DWORD code = 0;
            value = guarded_store(n + 100, &code);
            if (code != 0) {
                // The crash handler did not hold the thread: the application's own handler saw the fault, and recovers.
                std::printf("PASSED_ON n=%d code=0x%08lX\n", n, static_cast<unsigned long>(code));
                set_cell_access(PAGE_READWRITE);
            }
        } else {
            value = cell_store(n + 100);
        }
        if (g_leave_mode && g_armed && value == 0) {
            set_cell_access(PAGE_READWRITE);  // the application recovers from the failed store
        }
        std::printf("TICK n=%d cell=%d armed=%d\n", n, value, g_armed ? 1 : 0);
    });
}
