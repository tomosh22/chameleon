#include "cml/proc/proc.h"

#if defined(_WIN32)

#include <windows.h>

#include <sddl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>
#include <memory>

#include "cml/proc/diag_ids.h"

namespace cml::proc {

namespace {

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept { LocalFree(memory); }
};

std::string win_error_text(DWORD error) {
    return "Win32 error " + std::to_string(error);
}

// Fills `out` with the TOKEN_USER blob of an open token.
bool query_token_user(HANDLE token, std::vector<unsigned char>& out) {
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        return false;
    }
    out.assign(needed, 0);
    return GetTokenInformation(token, TokenUser, out.data(), needed, &needed) != FALSE;
}

std::string sid_to_string(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) {
        return {};
    }
    std::unique_ptr<void, LocalFreeDeleter> guard(text);
    return utf8_from_wide(text, std::wcslen(text));
}

Diag sid_diag(const char* what, DWORD error) {
    return Diag::make(ids::kSidQueryFailed,
                      std::string("cannot query ") + what + " (" + win_error_text(error) + ")",
                      "Check that the process can open its own token and run Chameleon as the "
                      "same user as the application.");
}

}  // namespace

Result<std::vector<ModuleInfo>> enumerate_modules(std::uint32_t pid) {
    const DWORD target = pid == 0 ? GetCurrentProcessId() : pid;
    UniqueHandle snapshot;
    DWORD last_error = 0;
    // ERROR_BAD_LENGTH means the module list changed while the snapshot was taken; retry.
    for (int attempt = 0; attempt < 8; ++attempt) {
        HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target);
        if (h != INVALID_HANDLE_VALUE) {
            snapshot.reset(h);
            break;
        }
        last_error = GetLastError();
        if (last_error != ERROR_BAD_LENGTH) {
            break;
        }
    }
    if (!snapshot) {
        return Diag::make(ids::kModuleEnumFailed,
                          "cannot enumerate the modules of process " + std::to_string(target) +
                              " (" + win_error_text(last_error) + ")",
                          "Check that the process still exists and that you may inspect it.");
    }

    std::vector<ModuleInfo> modules;
    MODULEENTRY32W entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot.get(), &entry); ok;
         ok = Module32NextW(snapshot.get(), &entry)) {
        ModuleInfo info;
        info.base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(entry.modBaseAddr));
        info.size = entry.modBaseSize;
        info.path = utf8_from_wide(entry.szExePath, std::wcslen(entry.szExePath));
        modules.push_back(std::move(info));
    }
    return modules;
}

Result<std::string> current_user_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        return sid_diag("the process token", GetLastError());
    }
    UniqueHandle token(raw);
    std::vector<unsigned char> blob;
    if (!query_token_user(token.get(), blob)) {
        return sid_diag("the token user", GetLastError());
    }
    const std::string text = sid_to_string(reinterpret_cast<TOKEN_USER*>(blob.data())->User.Sid);
    if (text.empty()) {
        return sid_diag("the user SID string", GetLastError());
    }
    return text;
}

Result<std::string> logon_session_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        return sid_diag("the process token", GetLastError());
    }
    UniqueHandle token(raw);
    DWORD needed = 0;
    GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &needed);
    if (needed != 0) {
        std::vector<unsigned char> blob(needed, 0);
        if (GetTokenInformation(token.get(), TokenGroups, blob.data(), needed, &needed)) {
            const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(blob.data());
            for (DWORD i = 0; i < groups->GroupCount; ++i) {
                if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
                    const std::string text = sid_to_string(groups->Groups[i].Sid);
                    if (!text.empty()) {
                        return text;
                    }
                }
            }
        }
    }
    // No logon-id group (unusual, e.g. some service tokens): the user SID still separates users.
    return current_user_sid();
}

Result<bool> process_runs_as_current_user(std::uint32_t pid) {
    // PROCESS_QUERY_LIMITED_INFORMATION is enough to open the token on Vista and later and is
    // granted even for elevated processes; fall back to the full right for older behaviour.
    HANDLE raw_other = nullptr;
    DWORD token_error = ERROR_SUCCESS;
    for (const DWORD access : {PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_QUERY_INFORMATION}) {
        UniqueHandle process(OpenProcess(access, FALSE, pid));
        if (!process) {
            token_error = GetLastError();
            continue;
        }
        if (OpenProcessToken(process.get(), TOKEN_QUERY, &raw_other)) {
            break;
        }
        token_error = GetLastError();
        raw_other = nullptr;
    }
    if (raw_other == nullptr) {
        return sid_diag("the pipe server process token", token_error);
    }
    UniqueHandle other_token(raw_other);
    HANDLE raw_self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_self)) {
        return sid_diag("the process token", GetLastError());
    }
    UniqueHandle self_token(raw_self);
    std::vector<unsigned char> a;
    std::vector<unsigned char> b;
    if (!query_token_user(other_token.get(), a) || !query_token_user(self_token.get(), b)) {
        return sid_diag("the token user", GetLastError());
    }
    return EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid,
                    reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid) != FALSE;
}

std::uint32_t current_process_id() noexcept {
    return GetCurrentProcessId();
}

std::string current_command_line() {
    const wchar_t* line = GetCommandLineW();
    return line == nullptr ? std::string() : utf8_from_wide(line, std::wcslen(line));
}

std::optional<std::string> get_env(std::string_view name) {
    const std::wstring wide_name = wide_from_utf8(name);
    std::wstring value(128, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableW(wide_name.c_str(), value.data(),
                                                static_cast<DWORD>(value.size()));
        if (n == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string();  // set but empty
        }
        if (n < value.size()) {
            return utf8_from_wide(value.data(), n);
        }
        value.assign(static_cast<std::size_t>(n) + 1, L'\0');  // too small: n is the needed size
    }
}

std::string utf8_from_wide(const wchar_t* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return {};
    }
    const int in_len = static_cast<int>(length);
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, in_len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, in_len, out.data(), n, nullptr, nullptr);
    return out;
}

std::string utf8_from_wide(std::wstring_view text) {
    return utf8_from_wide(text.data(), text.size());
}

std::wstring wide_from_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int in_len = static_cast<int>(text.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), in_len, nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), in_len, out.data(), n);
    return out;
}

std::vector<std::string> command_line_arguments() {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return args;
    }
    std::unique_ptr<void, LocalFreeDeleter> guard(argv);
    for (int i = 0; i < argc; ++i) {
        args.push_back(utf8_from_wide(argv[i], std::wcslen(argv[i])));
    }
    return args;
}

}  // namespace cml::proc

#endif  // _WIN32
