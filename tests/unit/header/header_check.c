/*
 * Header quality gate for Chameleon_API.h (C99 side).
 *
 * Compiled as C (clang-cl: -std=c99; cl: /std:c11, the closest mode cl offers) at /W4 /WX and, for
 * clang-cl, -Wall -Wextra -Werror, once with WIN32_LEAN_AND_MEAN / NOMINMAX as the project defines
 * them and once undefined ("bare"). It calls every C API function on invalid input and checks that
 * each fails gracefully; no agent DLL is loaded and no broker is contacted.
 */
#if defined(CML_HEADER_CHECK_BARE)
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
#endif

#include "Chameleon_API.h"

#include <stdio.h>

static int g_failures = 0;

static void expect(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

/* --- hook macros (WP-17): every kind registers a record that survives the link and names its function --- */
static int g_hook_probe = 0;
static void on_pre(void) { g_hook_probe |= 1; }
static void on_post(void) { g_hook_probe |= 2; }
static void on_compile_start(void) { g_hook_probe |= 4; }
static void on_compile_success(void) { g_hook_probe |= 8; }
static void on_compile_error(const char* message) { g_hook_probe |= (message != NULL) ? 16 : 0; }
static void on_pre_restart(void) { g_hook_probe |= 32; }
CML_PREPATCH_HOOK(on_pre);
CML_POSTPATCH_HOOK(on_post);
CML_COMPILE_START_HOOK(on_compile_start);
CML_COMPILE_SUCCESS_HOOK(on_compile_success);
CML_COMPILE_ERROR_HOOK(on_compile_error);
CML_PRERESTART_HOOK(on_pre_restart);

/* Scans this image's ".cmlhk" section the way the agent does and checks the six records. */
static void check_hook_records(void) {
    const unsigned char* base = (const unsigned char*)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    unsigned seen = 0;
    unsigned count = 0;
    unsigned i;
    expect(sizeof(cml_hook_entry) == 24, "a hook record is 24 bytes");
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".cmlhk\0\0", 8) == 0) {
            const cml_hook_entry* records = (const cml_hook_entry*)(base + section[i].VirtualAddress);
            const size_t n = section[i].Misc.VirtualSize / sizeof(cml_hook_entry);
            size_t k;
            for (k = 0; k < n; ++k) {
                if (records[k].magic == CML_HOOK_MAGIC && records[k].kind >= 1 && records[k].kind <= 6 &&
                    records[k].fn != NULL && records[k].key != NULL && strchr(records[k].key, '|') != NULL) {
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

int main(void) {
    cml_status status = CML_STATUS_OK;
    cml_agent missing = cml_create_agent(L"Z:\\cml_no_such_directory", CML_MODE_IMMEDIATE, &status);
    uint64_t id = 99;
    cml_reply reply;
    cml_hotkeys hotkeys;
    wchar_t path[32768];
    wchar_t tiny[2];
    uint32_t length = 0;

    expect(missing == NULL, "create from a missing directory returns NULL");
    expect(status == CML_STATUS_AGENT_NOT_FOUND, "missing directory reports AGENT_NOT_FOUND");
    expect(cml_create_agent(L"Z:\\cml_no_such_directory", 7, &status) == NULL &&
               status == CML_STATUS_INVALID_ARGUMENT,
           "an unknown mode is rejected");
    expect(cml_create_default_agent(L"Z:\\cml_no_such_directory") == NULL, "default agent fails cleanly");
    expect(cml_create_synchronized_agent(L"Z:\\cml_no_such_directory") == NULL,
           "synchronized agent fails cleanly");

    reply.status = 0;
    reply.message_length = 0;
    reply.message[0] = '\0';
    hotkeys.struct_size = (uint32_t)sizeof(hotkeys);
    hotkeys.reload.modifiers = CML_HOTKEY_CONTROL | CML_HOTKEY_ALT;
    hotkeys.reload.virtual_key = 'R';
    hotkeys.restart.modifiers = CML_HOTKEY_SHIFT | CML_HOTKEY_WIN;
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

    expect(cml_get_current_module_path(path, 32768, &length) == CML_STATUS_OK && length > 0,
           "current module path");
    expect(cml_get_current_module_path(tiny, 2, &length) == CML_STATUS_INVALID_ARGUMENT,
           "a too-small buffer is reported");
    expect(cml_get_current_module_path(NULL, 0, NULL) == CML_STATUS_INVALID_ARGUMENT, "null buffer");

    check_hook_records();

    return g_failures == 0 ? 0 : 1;
}
