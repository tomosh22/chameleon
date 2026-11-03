#pragma once

// The DLL shared by the server and the client of fixture two_processes. FIXTURE_LIB_EXPORTS is defined by lib.cpp only.
#ifdef FIXTURE_LIB_EXPORTS
#define FIXTURE_LIB_API __declspec(dllexport)
#else
#define FIXTURE_LIB_API __declspec(dllimport)
#endif

// Returns 7 (edit: 70). Every call bumps the per-process call counter.
FIXTURE_LIB_API int tp_value();
FIXTURE_LIB_API int tp_calls();
