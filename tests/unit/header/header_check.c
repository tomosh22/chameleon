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

    return g_failures == 0 ? 0 : 1;
}
