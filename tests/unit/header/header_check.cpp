// Header quality gate for Chameleon_API.h (C++ side).
//
// Built once per language standard (C++14, C++20) and once with WIN32_LEAN_AND_MEAN / NOMINMAX
// undefined ("bare"), at the maximum warning levels (/W4 /WX for cl; additionally
// -Wall -Wextra -Werror for clang-cl). The point is that this file compiles without a single
// warning; at run time it calls every API function on invalid input and checks that each fails
// gracefully (no agent DLL is loaded, no broker is contacted).
#if defined(CML_HEADER_CHECK_BARE)
// The project defines these for every target; a user's translation unit may not.
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
#endif

#include "Chameleon_API.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main() {
    // --- C API on invalid input ---------------------------------------------------------------
    cml_status status = CML_STATUS_OK;
    cml_agent missing = cml_create_agent(L"Z:\\cml_no_such_directory", CML_MODE_IMMEDIATE, &status);
    expect(missing == NULL, "create from a missing directory returns NULL");
    expect(status == CML_STATUS_AGENT_NOT_FOUND, "missing directory reports AGENT_NOT_FOUND");

    expect(cml_create_agent(L"Z:\\cml_no_such_directory", 7, &status) == NULL &&
               status == CML_STATUS_INVALID_ARGUMENT,
           "an unknown mode is rejected");
    expect(cml_create_default_agent(L"Z:\\cml_no_such_directory") == NULL, "default agent fails cleanly");
    expect(cml_create_synchronized_agent(L"Z:\\cml_no_such_directory") == NULL,
           "synchronized agent fails cleanly");

    uint64_t id = 99;
    cml_reply reply;
    reply.status = 0;
    reply.message_length = 0;
    reply.message[0] = '\0';
    cml_hotkeys hotkeys;
    hotkeys.struct_size = (uint32_t)sizeof(hotkeys);
    hotkeys.reload.modifiers = CML_HOTKEY_CONTROL | CML_HOTKEY_ALT;
    hotkeys.reload.virtual_key = 'R';
    hotkeys.restart.modifiers = 0;
    hotkeys.restart.virtual_key = 0;

    expect(cml_enable_module(NULL, L"Game.dll", CML_MODULE_ALL_IMPORTS, &id) == CML_STATUS_INVALID_ARGUMENT &&
               id == 0,
           "enable on a null agent");
    expect(cml_disable_module(NULL, L"Game.dll", &id) == CML_STATUS_INVALID_ARGUMENT, "disable on a null agent");
    expect(cml_wait_request(NULL, 1, 0, &reply) == CML_STATUS_INVALID_ARGUMENT, "wait on a null agent");
    cml_sync_point(NULL);
    expect(cml_schedule_reload(NULL) == CML_STATUS_INVALID_ARGUMENT, "schedule_reload on a null agent");
    expect(cml_wait_for_reload(NULL, 10) == CML_STATUS_INVALID_ARGUMENT, "wait_for_reload on a null agent");
    expect(cml_schedule_restart(NULL) == CML_STATUS_INVALID_ARGUMENT, "schedule_restart on a null agent");
    expect(cml_set_global_hotkeys(NULL, &hotkeys) == CML_STATUS_INVALID_ARGUMENT, "hotkeys on a null agent");
    expect(cml_is_connected(NULL) == 0, "null agent is not connected");
    cml_destroy_agent(NULL);

    wchar_t path[32768];
    uint32_t length = 0;
    expect(cml_get_current_module_path(path, 32768, &length) == CML_STATUS_OK && length > 0,
           "current module path");
    wchar_t tiny[2];
    expect(cml_get_current_module_path(tiny, 2, &length) == CML_STATUS_INVALID_ARGUMENT,
           "a too-small buffer is reported");
    expect(cml_get_current_module_path(NULL, 0, NULL) == CML_STATUS_INVALID_ARGUMENT, "null buffer");

    // --- C++ wrapper ------------------------------------------------------------------------
    cml_status create_status = CML_STATUS_OK;
    cml::Agent agent = cml::CreateDefaultAgent(L"Z:\\cml_no_such_directory", &create_status);
    expect(!agent.IsValid() && !agent && create_status == CML_STATUS_AGENT_NOT_FOUND,
           "CreateDefaultAgent from a missing directory");
    cml::Agent synchronized = cml::CreateSynchronizedAgent(L"Z:\\cml_no_such_directory");
    expect(!synchronized.IsValid(), "CreateSynchronizedAgent from a missing directory");

    const cml::Request enable = agent.EnableModule(L"Game.dll", cml::ModuleOptions::None);
    expect(enable.status == CML_STATUS_INVALID_ARGUMENT && enable.id == 0, "Agent::EnableModule(wchar_t*)");
    const cml::Request enable_all = agent.EnableModule(std::wstring(L"Game.dll"));
    expect(enable_all.status == CML_STATUS_INVALID_ARGUMENT, "Agent::EnableModule(wstring, default options)");
    const cml::Request combined = agent.EnableModule(cml::GetCurrentModulePath(),
                                                     cml::ModuleOptions::AllImports | cml::ModuleOptions::None);
    expect(combined.status == CML_STATUS_INVALID_ARGUMENT, "Agent::EnableModule(GetCurrentModulePath())");
    expect(agent.DisableModule(L"Game.dll").status == CML_STATUS_INVALID_ARGUMENT, "Agent::DisableModule(wchar_t*)");
    expect(agent.DisableModule(std::wstring(L"Game.dll")).status == CML_STATUS_INVALID_ARGUMENT,
           "Agent::DisableModule(wstring)");
    cml::Reply wrapper_reply;
    expect(agent.WaitForRequest(enable, 0, &wrapper_reply) == CML_STATUS_INVALID_ARGUMENT, "Agent::WaitForRequest");
    expect(agent.WaitForRequest(enable, 0) == CML_STATUS_INVALID_ARGUMENT, "Agent::WaitForRequest without reply");
    agent.SyncPoint();
    expect(agent.ScheduleReload() == CML_STATUS_INVALID_ARGUMENT, "Agent::ScheduleReload");
    expect(agent.WaitForReload(10) == CML_STATUS_INVALID_ARGUMENT, "Agent::WaitForReload");
    expect(agent.ScheduleRestart() == CML_STATUS_INVALID_ARGUMENT, "Agent::ScheduleRestart");
    expect(agent.SetGlobalHotkeys(hotkeys) == CML_STATUS_INVALID_ARGUMENT, "Agent::SetGlobalHotkeys");
    expect(!agent.IsConnected(), "Agent::IsConnected");
    expect(agent.NativeHandle() == NULL, "Agent::NativeHandle");
    cml::DestroyAgent(agent);
    expect(!agent.IsValid(), "DestroyAgent leaves the handle invalid");

    const std::wstring self = cml::GetCurrentModulePath();
    expect(!self.empty() && self.find(L".exe") != std::wstring::npos, "GetCurrentModulePath names this exe");

    return g_failures == 0 ? 0 : 1;
}
