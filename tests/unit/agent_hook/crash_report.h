#pragma once

// Names where an unhandled exception happened (including one at process exit, after the tests have
// passed): exception code, address, and a symbolised stack of the faulting thread via dbghelp, loaded
// at run time. Included by the hook test executables; installing it costs nothing when nothing crashes.
#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hooktest {

// Writes to the process's stderr handle directly (no C runtime stream).
inline void crash_line(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer, static_cast<DWORD>(n), &written, nullptr);
    }
}

// "function + offset" (or "module + offset") for an address, via dbghelp loaded at run time.
inline std::string symbolize(std::uintptr_t address) {
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    static HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    static auto sym_initialize =
        dbghelp ? reinterpret_cast<SymInitializeFn>(reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, "SymInitialize"))) : nullptr;
    static auto sym_from_addr =
        dbghelp ? reinterpret_cast<SymFromAddrFn>(reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, "SymFromAddr"))) : nullptr;
    static const bool initialised = sym_initialize != nullptr && sym_initialize(GetCurrentProcess(), nullptr, TRUE) != 0;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(address));
    std::string text = buffer;
    if (initialised && sym_from_addr != nullptr) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<PSYMBOL_INFO>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (sym_from_addr(GetCurrentProcess(), address, &displacement, symbol)) {
            text += std::string(" ") + symbol->Name + "+0x";
            std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(displacement));
            text += buffer;
        }
    }
    return text;
}

inline LONG WINAPI report_unhandled_exception(PEXCEPTION_POINTERS info) {
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using StackWalk64Fn = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                        PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
                                        PTRANSLATE_ADDRESS_ROUTINE64);
    crash_line("WP10-CRASH exception 0x%08lx at %p, thread %lu\n",
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), info->ExceptionRecord->ExceptionAddress,
                 static_cast<unsigned long>(GetCurrentThreadId()));
    if (info->ExceptionRecord->NumberParameters >= 2 && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        crash_line("WP10-CRASH access violation: %s address %p\n",
                     info->ExceptionRecord->ExceptionInformation[0] == 0   ? "read of"
                     : info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write to"
                                                                          : "execute of",
                     reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (dbghelp != nullptr) {
        auto get = [&](const char* name) { return reinterpret_cast<void (*)()>(GetProcAddress(dbghelp, name)); };
        auto sym_initialize = reinterpret_cast<SymInitializeFn>(get("SymInitialize"));
        auto sym_set_options = reinterpret_cast<SymSetOptionsFn>(get("SymSetOptions"));
        auto sym_from_addr = reinterpret_cast<SymFromAddrFn>(get("SymFromAddr"));
        auto stack_walk = reinterpret_cast<StackWalk64Fn>(get("StackWalk64"));
        auto table_access = reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(get("SymFunctionTableAccess64"));
        auto module_base = reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(get("SymGetModuleBase64"));
        const HANDLE process = GetCurrentProcess();
        if (sym_initialize != nullptr && sym_set_options != nullptr && sym_from_addr != nullptr && stack_walk != nullptr) {
            sym_set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            sym_initialize(process, nullptr, TRUE);
            CONTEXT context = *info->ContextRecord;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            for (int depth = 0; depth < 24; ++depth) {
                if (!stack_walk(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                table_access, module_base, nullptr) ||
                    frame.AddrPC.Offset == 0) {
                    break;
                }
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                if (sym_from_addr(process, frame.AddrPC.Offset, &displacement, symbol)) {
                    crash_line("WP10-CRASH   #%d %s + 0x%llx\n", depth, symbol->Name,
                                 static_cast<unsigned long long>(displacement));
                } else {
                    crash_line("WP10-CRASH   #%d 0x%llx\n", depth,
                                 static_cast<unsigned long long>(frame.AddrPC.Offset));
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct CrashReporterInstaller {
    CrashReporterInstaller() {
        SetUnhandledExceptionFilter(&report_unhandled_exception);
        std::atexit([] { crash_line("WP10 process exit begins\n"); });
    }
};
inline const CrashReporterInstaller g_crash_reporter_installer;

}  // namespace hooktest

#endif  // _WIN32
