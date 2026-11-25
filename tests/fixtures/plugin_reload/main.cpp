// Fixture `plugin_reload` (WP-45): a host that loads a plugin DLL at run time, long after its agent sent the module list, frees it
// and loads it again (the reduction of the corpus project godot-gdextension, tests/corpus/repro/f42_plugin_reload). See CMakeLists.txt.
//
//   --ticks N        stop after N lines (ctest); default: run until killed
//   --agent          create the agent next to the exe and enable this exe (also in a process the broker relaunched)
//
// Every time the file named by CML_FIXTURE_SIGNAL_FILE appears (the scenario's `signal_fixture` step) the host deletes it and:
//   * when the plugin is not loaded: LoadLibraryW, then EnableModule for it;
//   * when it is loaded: FreeLibrary, 2 s (longer than the agent's 500 ms module poll, so the broker sees it go), LoadLibraryW,
//     EnableModule again.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Chameleon_API.h"

namespace {

using ValueFn = int (*)();

bool env_is_set(const wchar_t* name)
{
    wchar_t buffer[4096];
    const DWORD n = GetEnvironmentVariableW(name, buffer, 4096);
    return n != 0 && n < 4096;
}

std::wstring env_value(const wchar_t* name)
{
    wchar_t buffer[4096];
    const DWORD n = GetEnvironmentVariableW(name, buffer, 4096);
    return n != 0 && n < 4096 ? std::wstring(buffer, n) : std::wstring();
}

// True once per appearance of the signal file (it is deleted, so the next `signal_fixture` makes a new one).
bool consume_signal(const std::wstring& file)
{
    if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    DeleteFileW(file.c_str());
    return true;
}

bool report(const cml::Agent& agent, const char* what, const cml::Request& request)
{
    cml::Reply reply;
    const cml_status status = agent.WaitForRequest(request, 0, &reply);
    if (status == CML_STATUS_TIMEOUT) {
        return false;
    }
    std::printf("AGENT %s enabled=%d message=\"%s\"\n", what, status == CML_STATUS_OK && reply.status == 0 ? 1 : 0,
                status == CML_STATUS_OK ? reply.message.c_str() : "wait failed");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    long max_ticks = -1;
    bool use_agent = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--agent") == 0) {
            use_agent = true;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const bool restarted = env_is_set(L"CML_RESTART_TOKEN");
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
    std::printf("START pid=%lu restarted=%d\n", pid, restarted ? 1 : 0);

    const std::wstring self = cml::GetCurrentModulePath();
    const std::size_t slash = self.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
    const std::wstring plugin_path = directory + L"\\fixture_plugin_reload_plugin.dll";
    const std::wstring signal_file = env_value(L"CML_FIXTURE_SIGNAL_FILE");

    cml::Agent agent;
    cml::Request self_request = {CML_STATUS_INVALID_ARGUMENT, 0};
    bool self_reported = true;
    if (use_agent) {
        cml_status status = CML_STATUS_OK;
        agent = cml::CreateDefaultAgent(directory.c_str(), &status);
        if (!agent) {
            std::printf("AGENT create failed status=%d\n", static_cast<int>(status));
        } else {
            self_request = agent.EnableModule(self, cml::ModuleOptions::AllImports);
            self_reported = false;
        }
    }

    HMODULE plugin = nullptr;
    ValueFn value = nullptr;
    int generation = 0;  // LoadLibraryW calls in this process
    cml::Request plugin_request = {CML_STATUS_INVALID_ARGUMENT, 0};
    bool plugin_reported = true;
    for (long n = 0; max_ticks < 0 || n < max_ticks; ++n) {
        if (consume_signal(signal_file)) {
            if (plugin != nullptr) {
                value = nullptr;
                const BOOL freed = FreeLibrary(plugin);
                plugin = nullptr;
                const bool gone = GetModuleHandleW(plugin_path.c_str()) == nullptr;
                std::printf("UNLOADED gen=%d freed=%d gone=%d\n", generation, freed ? 1 : 0, gone ? 1 : 0);
                Sleep(2000);
            }
            plugin = LoadLibraryW(plugin_path.c_str());
            ++generation;
            value = plugin != nullptr ? reinterpret_cast<ValueFn>(reinterpret_cast<void*>(GetProcAddress(plugin, "plugin_value"))) : nullptr;
            std::printf("LOADED gen=%d ok=%d base=%p\n", generation, value != nullptr ? 1 : 0, static_cast<void*>(plugin));
            if (agent && plugin != nullptr) {
                plugin_request = agent.EnableModule(plugin_path, cml::ModuleOptions::None);
                plugin_reported = false;
            }
        }
        std::printf("TICK n=%ld value=%d loaded=%d gen=%d pid=%lu restarted=%d\n", n, value != nullptr ? value() : 0, plugin != nullptr ? 1 : 0,
                    generation, pid, restarted ? 1 : 0);
        agent.SyncPoint();
        if (!self_reported) {
            self_reported = report(agent, "exe", self_request);
        }
        if (!plugin_reported) {
            plugin_reported = report(agent, "plugin", plugin_request);
        }
        Sleep(100);
    }

    if (plugin != nullptr) {
        FreeLibrary(plugin);
    }
    if (use_agent) {
        cml::DestroyAgent(agent);
    }
    return 0;
}
