#if defined(_WIN32)

#include <windows.h>

// The agent does nothing in DllMain: no threads, no pipes, no allocation of its own. Everything
// starts on an explicit cml_agent_create() call, outside the loader lock, and stops in
// cml_agent_destroy(). (Do not call DisableThreadLibraryCalls here: with the static CRT the
// runtime needs the thread notifications to release per-thread state.)
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}

#endif  // _WIN32
