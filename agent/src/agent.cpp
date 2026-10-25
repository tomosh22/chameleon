#include "Chameleon_API.h"

#if defined(_WIN32)
#include <windows.h>

// The agent does nothing on load yet; real initialisation happens on an explicit call from the
// broker, never inside DllMain (loader lock).
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
#endif

extern "C" int cml_agent_abi_version(void) {
    return CML_AGENT_ABI_VERSION;
}
