#include "broker_launch.h"

#if defined(_WIN32)

#include <windows.h>

#include <vector>

#include "cml/proc/diag_ids.h"
#include "cml/proc/proc.h"

namespace cml::agent {

namespace {

// Quotes one argument for CommandLineToArgvW / the MSVC runtime: wrap in quotes and escape
// embedded quotes (and the backslashes that precede them or the closing quote).
std::wstring quote_argument(const std::wstring& arg) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

Diag start_failed(const std::wstring& path, const std::string& why) {
    return Diag::make(ids::kBrokerStartFailed,
                      "cannot start the broker '" + proc::utf8_from_wide(path) + "': " + why,
                      "Set CML_BROKER_PATH to cml_broker.exe, or start the broker yourself with "
                      "'cml_broker --headless'.");
}

}  // namespace

std::wstring broker_path_for(const std::wstring& override_path, const std::wstring& agent_dir) {
    if (!override_path.empty()) {
        return override_path;
    }
    std::wstring path = agent_dir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += L"cml_broker.exe";
    return path;
}

Result<void> launch_broker(const std::wstring& broker_path, const std::string& pipe_name,
                           const std::string& extra_args) {
    const DWORD attributes = GetFileAttributesW(broker_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return start_failed(broker_path, "the file does not exist");
    }

    std::wstring command = quote_argument(broker_path) + L" --headless --pipe " +
                           quote_argument(proc::wide_from_utf8(pipe_name));
    if (!extra_args.empty()) {
        command += L" " + proc::wide_from_utf8(extra_args);
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');  // CreateProcessW may modify the buffer

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    // bInheritHandles = FALSE: the broker must not keep the application's pipes or files open.
    const BOOL ok = CreateProcessW(broker_path.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                                   DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                                   &startup, &info);
    if (!ok) {
        return start_failed(broker_path, "CreateProcess failed with Win32 error " +
                                             std::to_string(GetLastError()));
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return {};
}

}  // namespace cml::agent

#endif  // _WIN32
