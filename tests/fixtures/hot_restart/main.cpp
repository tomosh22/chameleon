// Fixture `hot_restart` (WP-21): prints START once and `TICK ...` every 100 ms; see CMakeLists.txt.
//
//   --ticks N        stop after N lines (ctest); default: run until killed
//   --agent          create the agent next to the exe; enable this module unless the broker relaunched the process
//   --agent-wait MS  bound the wait for the broker's answer at exit (default 30000)
//   --label=TEXT     printed in every TICK line (the command line survives a restart)
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include "Chameleon_API.h"
#include "logic.h"

namespace {

// Runs in the process just before the broker terminates it (CML_PRERESTART_HOOK).
void OnPreRestart()
{
    std::printf("PRERESTART pid=%lu value=%d\n", static_cast<unsigned long>(GetCurrentProcessId()), restart_value());
}

bool env_is_set(const wchar_t* name)
{
    wchar_t buffer[4096];
    const DWORD n = GetEnvironmentVariableW(name, buffer, 4096);
    return n != 0 && n < 4096;
}

bool cwd_is_exe_directory()
{
    wchar_t cwd[MAX_PATH * 4];
    const DWORD n = GetCurrentDirectoryW(MAX_PATH * 4, cwd);
    const std::wstring self = cml::GetCurrentModulePath();
    const std::size_t slash = self.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
    return n != 0 && n < MAX_PATH * 4 && _wcsicmp(cwd, directory.c_str()) == 0;
}

} // namespace

CML_PRERESTART_HOOK(OnPreRestart);

int main(int argc, char** argv)
{
    long max_ticks = -1;
    bool use_agent = false;
    DWORD agent_wait_ms = 30000;
    std::string label = "none";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--agent") == 0) {
            use_agent = true;
        } else if (std::strcmp(argv[i], "--agent-wait") == 0 && i + 1 < argc) {
            agent_wait_ms = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strncmp(argv[i], "--label=", 8) == 0) {
            label = argv[i] + 8;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const bool restarted = env_is_set(L"CML_RESTART_TOKEN");
    std::printf("START pid=%lu restarted=%d\n", static_cast<unsigned long>(GetCurrentProcessId()), restarted ? 1 : 0);

    cml::Agent agent;
    cml::Request enable_request = {CML_STATUS_INVALID_ARGUMENT, 0};
    bool enable_reported = true;
    if (use_agent) {
        const std::wstring self = cml::GetCurrentModulePath();
        const std::size_t slash = self.find_last_of(L"\\/");
        const std::wstring directory = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
        cml_status status = CML_STATUS_OK;
        agent = cml::CreateDefaultAgent(directory.c_str(), &status);
        if (!agent) {
            std::printf("AGENT create failed status=%d\n", static_cast<int>(status));
        } else if (!restarted) {
            enable_request = agent.EnableModule(self, cml::ModuleOptions::AllImports);
            enable_reported = false;
        } else {
            // A relaunched process does not enable its module: the broker does, from the list of the old process.
            std::printf("AGENT module not enabled by the application (restarted)\n");
        }
    }

    const bool env_ok = env_is_set(L"CML_FIXTURE_SIGNAL_FILE");
    const bool cwd_ok = cwd_is_exe_directory();
    for (long n = 0; max_ticks < 0 || n < max_ticks; ++n) {
        std::printf("TICK n=%ld value=%d pid=%lu restarted=%d label=%s cwd_ok=%d env_ok=%d\n", n, restart_value(),
                    static_cast<unsigned long>(GetCurrentProcessId()), restarted ? 1 : 0, label.c_str(), cwd_ok ? 1 : 0, env_ok ? 1 : 0);
        agent.SyncPoint();
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

    if (use_agent) {
        if (!enable_reported) {
            cml::Reply reply;
            const cml_status status = agent.WaitForRequest(enable_request, agent_wait_ms, &reply);
            std::printf("AGENT enabled=%d\n", status == CML_STATUS_OK && reply.status == 0 ? 1 : 0);
        }
        cml::DestroyAgent(agent);
    }
    return 0;
}
