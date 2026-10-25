#ifdef _WIN32

#include "win_remote.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>

#include "common.h"

namespace spike {

std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

namespace {

std::string lower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::uint64_t align_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) & ~(a - 1); }
std::uint64_t align_down(std::uint64_t v, std::uint64_t a) { return v & ~(a - 1); }

}  // namespace

std::vector<RemoteModule> list_modules(std::uint32_t pid)
{
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; ++attempt) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_BAD_LENGTH) {
            break;
        }
    }
    if (snap == INVALID_HANDLE_VALUE) {
        die("CreateToolhelp32Snapshot(modules of pid %u) failed: error %lu", pid, GetLastError());
    }
    std::vector<RemoteModule> out;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    if (Module32FirstW(snap, &me)) {
        do {
            RemoteModule m;
            m.name = to_utf8(me.szModule);
            m.path = to_utf8(me.szExePath);
            m.base = reinterpret_cast<std::uint64_t>(me.modBaseAddr);
            m.size = me.modBaseSize;
            out.push_back(std::move(m));
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

bool find_module(std::uint32_t pid, const std::string& file_name, RemoteModule* out)
{
    const std::string want = lower(file_name);
    for (const RemoteModule& m : list_modules(pid)) {
        if (lower(m.name) == want) {
            *out = m;
            return true;
        }
    }
    return false;
}

RemoteModule require_module(std::uint32_t pid, const std::string& file_name)
{
    RemoteModule m;
    if (!find_module(pid, file_name, &m)) {
        die("module '%s' is not loaded in pid %u", file_name.c_str(), pid);
    }
    return m;
}

HANDLE open_target(std::uint32_t pid)
{
    const DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ |
                         PROCESS_VM_WRITE;
    HANDLE h = OpenProcess(rights, FALSE, pid);
    if (h == nullptr) {
        die("OpenProcess(pid %u) failed: error %lu", pid, GetLastError());
    }
    BOOL wow64 = FALSE;
    IsWow64Process(h, &wow64);
    if (wow64) {
        die("pid %u is a 32-bit process; the spike supports x64 targets only", pid);
    }
    return h;
}

std::uint64_t reserve_near(HANDLE process, std::uint64_t module_base, std::uint64_t module_size, std::uint64_t size,
                           std::uint64_t window)
{
    constexpr std::uint64_t kGranularity = 0x10000;
    constexpr std::uint64_t kReach = (1ull << 31) - 0x100000;  // 2 GB minus 1 MB margin
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uint64_t max_addr = reinterpret_cast<std::uint64_t>(si.lpMaximumApplicationAddress) + 1;
    const std::uint64_t module_end = module_base + module_size;

    // block_end - module_base <= kReach  and  module_end - block_start <= kReach, and within `window`.
    std::uint64_t lo = module_end > kReach ? module_end - kReach : 0x10000;
    if (module_base > window) {
        lo = std::max(lo, module_base - window);
    }
    lo = align_up(std::max<std::uint64_t>(lo, 0x10000), kGranularity);
    std::uint64_t hi = std::min({module_base + kReach, module_end + window, max_addr});
    log("reserve: module [%s, %s) size=0x%llx; searching [%s, %s) for a free 0x%llx block",
        hex64(module_base).c_str(), hex64(module_end).c_str(), static_cast<unsigned long long>(module_size),
        hex64(lo).c_str(), hex64(hi).c_str(), static_cast<unsigned long long>(size));

    struct Candidate {
        std::uint64_t start;
        std::uint64_t distance;
    };
    std::vector<Candidate> candidates;
    std::uint64_t regions = 0;
    std::uint64_t free_regions = 0;
    std::uint64_t addr = lo;
    while (addr < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) == 0) {
            break;
        }
        ++regions;
        const std::uint64_t r_start = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
        const std::uint64_t r_end = r_start + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            ++free_regions;
            const std::uint64_t usable_lo = std::max(r_start, lo);
            const std::uint64_t usable_hi = std::min(r_end, hi);
            if (usable_hi > usable_lo && usable_hi - usable_lo >= size) {
                // Closest to the module from below (top of the region) and from above (bottom).
                const std::uint64_t top = align_down(usable_hi - size, kGranularity);
                if (top >= align_up(usable_lo, kGranularity)) {
                    const std::uint64_t d = top + size <= module_base ? module_base - (top + size) : 0;
                    candidates.push_back({top, d});
                }
                const std::uint64_t bottom = align_up(usable_lo, kGranularity);
                if (bottom + size <= usable_hi) {
                    const std::uint64_t d = bottom >= module_end ? bottom - module_end : 0;
                    candidates.push_back({bottom, d});
                }
            }
        }
        if (r_end <= addr) {
            break;
        }
        addr = r_end;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    log("reserve: scanned %llu regions (%llu free), %zu candidate blocks",
        static_cast<unsigned long long>(regions), static_cast<unsigned long long>(free_regions), candidates.size());

    for (const Candidate& c : candidates) {
        void* p = VirtualAllocEx(process, reinterpret_cast<LPVOID>(c.start), size, MEM_RESERVE, PAGE_NOACCESS);
        if (p != nullptr) {
            const std::uint64_t got = reinterpret_cast<std::uint64_t>(p);
            log("reserve: reserved %s .. %s (distance from module %llu MB)", hex64(got).c_str(),
                hex64(got + size).c_str(), static_cast<unsigned long long>(c.distance >> 20));
            return got;
        }
        log("reserve: probe at %s failed (error %lu), trying the next candidate", hex64(c.start).c_str(),
            GetLastError());
    }
    die("no free 0x%llx byte block found within REL32 reach of the module", static_cast<unsigned long long>(size));
}

DWORD release_reservation(HANDLE process, std::uint64_t base)
{
    if (VirtualFreeEx(process, reinterpret_cast<LPVOID>(base), 0, MEM_RELEASE) == 0) {
        return GetLastError();
    }
    return 0;
}

void write_remote(HANDLE process, std::uint64_t va, const void* bytes, std::size_t size)
{
    LPVOID p = reinterpret_cast<LPVOID>(va);
    DWORD old = 0;
    if (VirtualProtectEx(process, p, size, PAGE_EXECUTE_READWRITE, &old) == 0) {
        die("VirtualProtectEx(%s, %zu, RWX) failed: error %lu", hex64(va).c_str(), size, GetLastError());
    }
    SIZE_T written = 0;
    const BOOL ok = WriteProcessMemory(process, p, bytes, size, &written);
    const DWORD werr = GetLastError();
    DWORD ignored = 0;
    VirtualProtectEx(process, p, size, old, &ignored);
    FlushInstructionCache(process, p, size);
    if (!ok || written != size) {
        die("WriteProcessMemory(%s, %zu) failed: error %lu (wrote %zu)", hex64(va).c_str(), size, werr,
            static_cast<std::size_t>(written));
    }
}

std::vector<std::uint8_t> read_remote(HANDLE process, std::uint64_t va, std::size_t size)
{
    std::vector<std::uint8_t> out(size);
    SIZE_T got = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(va), out.data(), size, &got) == 0 || got != size) {
        die("ReadProcessMemory(%s, %zu) failed: error %lu (read %zu)", hex64(va).c_str(), size, GetLastError(),
            static_cast<std::size_t>(got));
    }
    return out;
}

std::uint64_t alloc_remote_string(HANDLE process, const std::wstring& text)
{
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    void* p = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p == nullptr) {
        die("VirtualAllocEx(string buffer) failed: error %lu", GetLastError());
    }
    SIZE_T written = 0;
    if (WriteProcessMemory(process, p, text.c_str(), bytes, &written) == 0 || written != bytes) {
        die("WriteProcessMemory(string buffer) failed: error %lu", GetLastError());
    }
    return reinterpret_cast<std::uint64_t>(p);
}

DWORD remote_load_library(HANDLE process, std::uint64_t remote_string)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC fn = k32 != nullptr ? GetProcAddress(k32, "LoadLibraryW") : nullptr;
    if (fn == nullptr) {
        die("cannot resolve kernel32!LoadLibraryW: error %lu", GetLastError());
    }
    // kernel32 sits at the same address in every process of a boot session, so our address of
    // LoadLibraryW is valid in the target.
    const auto start = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(fn));
    DWORD tid = 0;
    HANDLE t = CreateRemoteThread(process, nullptr, 0, start, reinterpret_cast<LPVOID>(remote_string), 0, &tid);
    if (t == nullptr) {
        die("CreateRemoteThread(LoadLibraryW) failed: error %lu", GetLastError());
    }
    log("remote thread %lu started at kernel32!LoadLibraryW=%s", tid, hex64(reinterpret_cast<std::uint64_t>(fn)).c_str());
    const DWORD w = WaitForSingleObject(t, 30000);
    if (w != WAIT_OBJECT_0) {
        die("remote LoadLibraryW thread did not finish (wait result %lu)", w);
    }
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    return code;
}

void free_remote(HANDLE process, std::uint64_t va)
{
    VirtualFreeEx(process, reinterpret_cast<LPVOID>(va), 0, MEM_RELEASE);
}

SuspendedThreads suspend_all(std::uint32_t pid)
{
    SuspendedThreads out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        die("CreateToolhelp32Snapshot(threads) failed: error %lu", GetLastError());
    }
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) {
                continue;
            }
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   te.th32ThreadID);
            if (th == nullptr) {
                log("suspend: OpenThread(%lu) failed: error %lu (thread gone?)", te.th32ThreadID, GetLastError());
                continue;
            }
            if (SuspendThread(th) == static_cast<DWORD>(-1)) {
                log("suspend: SuspendThread(%lu) failed: error %lu", te.th32ThreadID, GetLastError());
                CloseHandle(th);
                continue;
            }
            out.handles.push_back(th);
            out.ids.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out;
}

void resume_all(SuspendedThreads& threads)
{
    for (HANDLE h : threads.handles) {
        ResumeThread(h);
        CloseHandle(h);
    }
    threads.handles.clear();
    threads.ids.clear();
}

std::vector<std::uint64_t> thread_rips(const SuspendedThreads& threads)
{
    std::vector<std::uint64_t> rips;
    for (std::size_t i = 0; i < threads.handles.size(); ++i) {
        CONTEXT ctx;
        std::memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(threads.handles[i], &ctx) == 0) {
            die("GetThreadContext(thread %lu) failed: error %lu", threads.ids[i], GetLastError());
        }
        rips.push_back(ctx.Rip);
    }
    return rips;
}

}  // namespace spike

#endif  // _WIN32
