/* Companion of host.c (F-42.1, F-42.2, F-42.3): the plugin enables its own module through the host's agent when the
 * host calls init(); value() is the edit target. */
#include <windows.h>

#include "Chameleon_API.h"

__declspec(dllexport) void init(void) {
    void* (*host_agent)(void) = (void* (*)(void))GetProcAddress(GetModuleHandleW(NULL), "repro_host_agent");
    cml_agent agent = host_agent != NULL ? (cml_agent)host_agent() : NULL;
    wchar_t self[MAX_PATH];
    uint64_t id = 0;
    if (agent != NULL && cml_get_current_module_path(self, MAX_PATH, NULL) == CML_STATUS_OK) {
        cml_enable_module(agent, self, CML_MODULE_NONE, &id);
    }
}

__declspec(dllexport) int value(void) {
    return 1; /* edit: 2, then 3 */
}
