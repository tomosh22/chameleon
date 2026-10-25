// A fake cml_agent.dll that claims an ABI version no header knows. Chameleon_API.h must refuse it
// with CML_STATUS_ABI_MISMATCH instead of calling anything else in it (it exports nothing else).
#include <windows.h>

extern "C" __declspec(dllexport) int cml_agent_abi_version(void) {
    return 9999;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
