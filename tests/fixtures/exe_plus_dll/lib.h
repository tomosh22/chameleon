#pragma once

// The DLL of fixture exe_plus_dll. FIXTURE_LIB_EXPORTS is defined by lib.cpp only.
#ifdef FIXTURE_LIB_EXPORTS
#define FIXTURE_LIB_API __declspec(dllexport)
#else
#define FIXTURE_LIB_API __declspec(dllimport)
#endif

typedef int (*lib_callback)(int);

// Returns 3 * x (edit: 30 * x).
FIXTURE_LIB_API int lib_scale(int x);
// Returns x + 1. Never edited.
FIXTURE_LIB_API int lib_offset(int x);
// Calls back into the exe: returns cb(x).
FIXTURE_LIB_API int lib_apply(lib_callback cb, int x);
