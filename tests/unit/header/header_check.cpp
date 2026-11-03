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
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// --- hook macros (WP-17): every kind registers a record that survives the link and names its function ---
int g_hook_probe = 0;
void on_pre() { g_hook_probe |= 1; }
void on_post() { g_hook_probe |= 2; }
void on_compile_start() { g_hook_probe |= 4; }
void on_compile_success() { g_hook_probe |= 8; }
void on_compile_error(const char* message) { g_hook_probe |= (message != NULL) ? 16 : 0; }
void on_pre_restart() { g_hook_probe |= 32; }

CML_PREPATCH_HOOK(on_pre);
CML_POSTPATCH_HOOK(on_post);
CML_COMPILE_START_HOOK(on_compile_start);
CML_COMPILE_SUCCESS_HOOK(on_compile_success);
CML_COMPILE_ERROR_HOOK(on_compile_error);
CML_PRERESTART_HOOK(on_pre_restart);

// Scans this image's ".cmlhk" section the way the agent does and checks the six records.
void check_hook_records() {
    const unsigned char* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(NULL));
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    unsigned seen = 0;
    unsigned count = 0;
    expect(sizeof(cml_hook_entry) == 24, "a hook record is 24 bytes");
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(section[i].Name, ".cmlhk\0\0", 8) == 0) {
            const cml_hook_entry* records = reinterpret_cast<const cml_hook_entry*>(base + section[i].VirtualAddress);
            const size_t n = section[i].Misc.VirtualSize / sizeof(cml_hook_entry);
            for (size_t k = 0; k < n; ++k) {
                if (records[k].magic == CML_HOOK_MAGIC && records[k].kind >= 1 && records[k].kind <= 6 &&
                    records[k].fn != NULL && records[k].key != NULL && std::strchr(records[k].key, '|') != NULL) {
                    seen |= 1u << records[k].kind;
                    ++count;
                }
            }
        }
    }
    expect(count == 6, "six hook records in the .cmlhk section");
    expect(seen == 0x7Eu, "one record of each kind");
    on_pre();
    on_post();
    on_compile_start();
    on_compile_success();
    on_compile_error("x");
    on_pre_restart();
    expect(g_hook_probe == 63, "the hook functions are callable");
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
    expect(cml_last_patch_status() == CML_STATUS_AGENT_NOT_FOUND, "no agent loaded: cml_last_patch_status says so");
    expect(!agent.IsValid(), "DestroyAgent leaves the handle invalid");

    const std::wstring self = cml::GetCurrentModulePath();
    expect(!self.empty() && self.find(L".exe") != std::wstring::npos, "GetCurrentModulePath names this exe");

    check_hook_records();

    return g_failures == 0 ? 0 : 1;
}
