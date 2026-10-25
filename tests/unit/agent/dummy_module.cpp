// A harmless DLL that the agent end-to-end test loads and unloads at will, to check that the agent
// reports ModuleLoaded / ModuleUnloaded to the broker.
#include <windows.h>

extern "C" __declspec(dllexport) int cml_dummy_value(void) {
    return 42;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
