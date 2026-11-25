// The plugin DLL of fixture plugin_reload (WP-45). The host loads it with LoadLibraryW, frees it and loads it again; this TU is the
// one the scenarios edit (1 -> 2 -> 3 ...). C linkage, so the host finds it with GetProcAddress.

extern "C" __declspec(dllexport) int plugin_value()
{
    return 1;
}
