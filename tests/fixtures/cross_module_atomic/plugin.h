#pragma once

// The plugin DLL of fixture cross_module_atomic. FIXTURE_PLUGIN_EXPORTS is defined by the DLL's sources only.
#ifdef FIXTURE_PLUGIN_EXPORTS
#define FIXTURE_PLUGIN_API __declspec(dllexport)
#else
#define FIXTURE_PLUGIN_API __declspec(dllimport)
#endif

// shared_scale() * 100 + kScale (101; 202 after the edit). plugin.cpp includes scale.h.
FIXTURE_PLUGIN_API int plugin_value();
// The calling thread's count of calls: a thread_local of the DLL (plugin_tls.cpp, never edited).
FIXTURE_PLUGIN_API int plugin_frames();
