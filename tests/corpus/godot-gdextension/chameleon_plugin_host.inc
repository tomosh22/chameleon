// WP-42 corpus support code (Chameleon's own, MIT like the rest of this repository): the host side of the plugin
// architecture. tests/corpus/godot-gdextension/build.ps1 copies this file next to chameleon_corpus.cpp in the Godot
// checkout of its own cell and appends `#include "chameleon_plugin_host.inc"` to that file, so the two functions below
// see its file-local agent. Nothing changes in the Godot patch, its build script or the cell `godot / msvc`.
//
// A plugin DLL that Godot loads at run time (LoadLibraryExW, from GDExtensionManager::load_extension) finds them with
// GetProcAddress(GetModuleHandleW(NULL), ...): the host's agent, through which the plugin enables its own module
// once it is loaded, and the host's marker list, through which edits of the plugin become visible in the CORPUS line.

extern "C" __declspec(dllexport) void* cml_corpus_host_agent(void) {
    return static_cast<void*>(g_agent.NativeHandle());
}

extern "C" __declspec(dllexport) void cml_corpus_host_marker(const char* name) {
    cml_corpus_marker(name);
}
