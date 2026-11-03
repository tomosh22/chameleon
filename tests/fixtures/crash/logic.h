#pragma once

#include <windows.h>

#include <atomic>

// Owned by main.cpp (never patched): the page cell_store() writes, and the state the helper thread and the patch share.
extern char* g_cell;                      // a committed page; main.cpp makes it inaccessible when the harness arms the fixture
extern std::atomic<bool> g_repaired;      // set by the patched maintain()
extern std::atomic<int> g_maintain_calls;

// Writes `value` into the page and returns what it reads back. Faults while the page is inaccessible.
int cell_store(int value);

// Called every 10 ms by the helper thread.
void maintain();
