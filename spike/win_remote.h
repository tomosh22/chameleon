#pragma once

// Out-of-process operations on the target (the "stub agent" of WP-03): everything the real agent
// will later do from inside the process is done here from the outside. Windows only.

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace spike {

struct RemoteModule {
    std::string name;  // file name, e.g. "fixture_basic.exe"
    std::string path;
    std::uint64_t base = 0;
    std::uint64_t size = 0;
};

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);

std::vector<RemoteModule> list_modules(std::uint32_t pid);
// Case-insensitive file name match; die()s if absent.
RemoteModule require_module(std::uint32_t pid, const std::string& file_name);
// True and fills `out` when present.
bool find_module(std::uint32_t pid, const std::string& file_name, RemoteModule* out);

// Opens the process with the rights the spike needs; die()s on failure.
HANDLE open_target(std::uint32_t pid);

// Reserves `size` bytes (MEM_RESERVE, PAGE_NOACCESS) at a 64 KB aligned address such that every
// byte of the block is within the REL32 reach (< 2 GB, with `margin`) of every byte of
// [module_base, module_base + module_size) and the block lies within +-`window` of the module.
// Scans with VirtualQueryEx and probes with VirtualAllocEx. Returns the base; die()s if none.
std::uint64_t reserve_near(HANDLE process, std::uint64_t module_base, std::uint64_t module_size, std::uint64_t size,
                           std::uint64_t window);

// VirtualFreeEx(base, 0, MEM_RELEASE); returns the Win32 error (0 = ok).
DWORD release_reservation(HANDLE process, std::uint64_t base);

// Writes `bytes` at `va`. Temporarily makes the pages writable (VirtualProtectEx) and restores the
// previous protection; FlushInstructionCache afterwards. die()s on failure.
void write_remote(HANDLE process, std::uint64_t va, const void* bytes, std::size_t size);
std::vector<std::uint8_t> read_remote(HANDLE process, std::uint64_t va, std::size_t size);

// Allocates a remote buffer holding `dll_path` (wide, NUL terminated); returned so the caller can
// do the time-critical part (release reservation + start thread) back to back.
std::uint64_t alloc_remote_string(HANDLE process, const std::wstring& text);
// CreateRemoteThread(LoadLibraryW, remote_string) and waits. Returns the thread's exit code (the
// low 32 bits of the HMODULE, 0 on failure).
DWORD remote_load_library(HANDLE process, std::uint64_t remote_string);
void free_remote(HANDLE process, std::uint64_t va);

struct SuspendedThreads {
    std::vector<HANDLE> handles;
    std::vector<DWORD> ids;
};
// Suspends every thread of `pid`. die()s on failure.
SuspendedThreads suspend_all(std::uint32_t pid);
void resume_all(SuspendedThreads& threads);
// RIP of each suspended thread.
std::vector<std::uint64_t> thread_rips(const SuspendedThreads& threads);

}  // namespace spike

#endif  // _WIN32
