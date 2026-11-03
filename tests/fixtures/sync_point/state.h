#pragma once

// State shared by main.cpp (which owns it) and hooks.cpp. Neither file is ever patched.
#include <windows.h>

#include <atomic>

extern std::atomic<DWORD> g_main_thread;  // the thread that calls SyncPoint()
extern std::atomic<int> g_in_sync;        // 1 while that thread is inside SyncPoint()
