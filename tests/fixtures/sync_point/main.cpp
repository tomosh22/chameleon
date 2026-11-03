// Fixture `sync_point` (WP-17): a synchronised agent whose hooks may only be written inside SyncPoint().
//
//   --ticks N       stop after N lines (ctest); default: run until killed
//   --agent         create the agent next to the exe and enable this module
//   --immediate     with --agent: an immediate-mode agent instead of the synchronised one
//   --hold-sync     do not call SyncPoint() until the file named by CML_FIXTURE_SIGNAL_FILE exists
//   --agent-wait MS bound the wait for the broker's answer at exit (default 30000)
//
// The "watcher" thread polls compute_value() every millisecond. When the value changes it asks: was the main thread
// inside SyncPoint() when the change happened? The install runs with every other thread suspended, so the watcher
// cannot see "inside" directly; it compares the number of finished SyncPoint() calls before and after the change,
// and the in-SyncPoint flag. A change made by the agent thread between two SyncPoint() calls shows neither.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "Chameleon_API.h"
#include "logic.h"
#include "state.h"

std::atomic<DWORD> g_main_thread{0};
std::atomic<int> g_in_sync{0};

namespace {

std::atomic<unsigned> g_sync_exits{0};  // finished SyncPoint() calls
std::atomic<int> g_change{0};           // 0 = no change seen, 1 = seen in SyncPoint(), 2 = seen outside it
std::atomic<bool> g_stop{false};

void watch()
{
    int last = compute_value();
    unsigned exits_at_last = g_sync_exits.load();
    while (!g_stop.load()) {
        const int value = compute_value();
        const unsigned exits = g_sync_exits.load();
        if (value != last) {
            // Inside SyncPoint() now, or a SyncPoint() finished since the last look: the change was written there.
            const bool inside = g_in_sync.load() != 0 || exits != exits_at_last;
            g_change.store(inside ? 1 : 2);
            std::printf("WATCH changed from=%d to=%d inside_sync=%d\n", last, value, inside ? 1 : 0);
            last = value;
        }
        exits_at_last = exits;
        Sleep(1);
    }
}

bool file_exists(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

int main(int argc, char** argv)
{
    long max_ticks = -1;
    bool use_agent = false;
    bool immediate = false;
    bool hold_sync = false;
    DWORD agent_wait_ms = 30000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--agent") == 0) {
            use_agent = true;
        } else if (std::strcmp(argv[i], "--immediate") == 0) {
            immediate = true;
        } else if (std::strcmp(argv[i], "--hold-sync") == 0) {
            hold_sync = true;
        } else if (std::strcmp(argv[i], "--agent-wait") == 0 && i + 1 < argc) {
            agent_wait_ms = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_main_thread.store(GetCurrentThreadId());

    wchar_t signal_file[4096] = L"";
    GetEnvironmentVariableW(L"CML_FIXTURE_SIGNAL_FILE", signal_file, 4096);

    cml::Agent agent;
    cml::Request enable_request = {CML_STATUS_INVALID_ARGUMENT, 0};
    bool enable_reported = true;
    if (use_agent) {
        const std::wstring self = cml::GetCurrentModulePath();
        const std::size_t slash = self.find_last_of(L"\\/");
        const std::wstring directory = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
        cml_status status = CML_STATUS_OK;
        agent = immediate ? cml::CreateDefaultAgent(directory.c_str(), &status)
                          : cml::CreateSynchronizedAgent(directory.c_str(), &status);
        if (!agent) {
            std::printf("AGENT create failed status=%d\n", static_cast<int>(status));
        } else {
            std::printf("AGENT mode=%s\n", immediate ? "immediate" : "synchronized");
            enable_request = agent.EnableModule(self, cml::ModuleOptions::AllImports);
            enable_reported = false;
        }
    }

    std::thread watcher;
    if (use_agent) {
        watcher = std::thread(watch);
    }

    bool released = !hold_sync;
    for (long n = 0; max_ticks < 0 || n < max_ticks; ++n) {
        std::printf("TICK n=%ld value=%d change=%d\n", n, compute_value(), g_change.load());
        if (!released && signal_file[0] != L'\0' && file_exists(signal_file)) {
            released = true;
            std::printf("SYNC released\n");
        }
        if (released) {
            // An immediate agent does nothing here; the flag is only meaningful for a synchronised one.
            g_in_sync.store(immediate ? 0 : 1);
            agent.SyncPoint();
            g_in_sync.store(0);
            g_sync_exits.fetch_add(1);
        }
        if (!enable_reported) {
            cml::Reply reply;
            const cml_status status = agent.WaitForRequest(enable_request, 0, &reply);
            if (status != CML_STATUS_TIMEOUT) {
                std::printf("AGENT enabled=%d message=\"%s\"\n", status == CML_STATUS_OK && reply.status == 0 ? 1 : 0,
                            status == CML_STATUS_OK ? reply.message.c_str() : "wait failed");
                enable_reported = true;
            }
        }
        Sleep(100);
    }

    g_stop.store(true);
    if (watcher.joinable()) {
        watcher.join();
    }
    if (use_agent && agent) {
        if (!enable_reported) {
            cml::Reply reply;
            if (agent.WaitForRequest(enable_request, agent_wait_ms, &reply) == CML_STATUS_TIMEOUT) {
                std::printf("AGENT enabled=0 message=\"no answer from the broker\"\n");
            }
        }
        cml::DestroyAgent(agent);
    }
    return 0;
}
