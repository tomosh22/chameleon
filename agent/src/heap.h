#pragma once

// The agent's private heap.
//
// Every C++ allocation made by code inside cml_agent.dll (new/delete, so std::string,
// std::vector, std::map, ... and the statically linked cml_util / cml_ipc / cml_proc code) goes
// through a Win32 heap created with HeapCreate instead of the process heap. Reasons:
//   * Isolation: the agent never frees into, fragments or depends on the host's heap, and a host
//     that replaced malloc/new cannot see or corrupt agent memory.
//   * Control for WP-10: once threads of the host are suspended during a patch, the agent must not
//     allocate (a suspended thread might hold the heap lock). set_alloc_forbidden() is the switch
//     WP-10 will flip around the suspended section; an allocation attempted while it is set is a
//     programmer error and aborts with a message instead of deadlocking.
// Allocations made directly by the C runtime (malloc, fopen, ...) and by Win32 are not covered.
//
// The heap is created on first use and never destroyed: the agent DLL stays loaded for the life
// of the process (see Chameleon_API.h) and static destructors may run after DLL_PROCESS_DETACH.

#include <cstddef>

namespace cml::agent::heap {

// Returns nullptr when out of memory (or `size` is absurd).
void* try_allocate(std::size_t size) noexcept;
void* try_allocate_aligned(std::size_t size, std::size_t alignment) noexcept;

// Abort the process with a message when out of memory; never return nullptr.
void* allocate(std::size_t size) noexcept;
void* allocate_aligned(std::size_t size, std::size_t alignment) noexcept;

// Accept nullptr.
void release(void* block) noexcept;
void release_aligned(void* block, std::size_t alignment) noexcept;

// While set, any allocation or release aborts the process (WP-10: threads are suspended).
void set_alloc_forbidden(bool forbidden) noexcept;
bool alloc_forbidden() noexcept;
// Id of the thread that set the flag (0 while it is clear). Tests use it to tell the thread that
// owns the stopped window from the threads that were not stopped yet.
unsigned long alloc_forbidden_thread() noexcept;

// Tests only. While set, an allocation or release during a forbidden window calls `hook` and then
// goes ahead, instead of aborting the process: the hook decides (count it, ignore a thread that is
// not the window's owner, terminate). nullptr restores the abort.
using ViolationHook = void (*)() noexcept;
void set_violation_hook_for_test(ViolationHook hook) noexcept;

// Number of blocks currently allocated and not yet released (for tests and leak checks).
std::size_t live_blocks() noexcept;

}  // namespace cml::agent::heap
