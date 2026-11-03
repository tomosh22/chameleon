#include "editor_launch.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#include "cml/proc/proc.h"
#include "cml/ui/ui_diag_ids.h"

namespace cml::ui::win {

namespace {

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring search_path(const std::wstring& name, const wchar_t* extension) {
    std::vector<wchar_t> buffer(MAX_PATH * 4);
    const DWORD n = SearchPathW(nullptr, name.c_str(), extension, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (n == 0 || n >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), n);
}

std::wstring env(const wchar_t* name) {
    std::vector<wchar_t> buffer(MAX_PATH * 4);
    const DWORD n = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), n);
}

std::wstring parent_of_parent(const std::wstring& path) {
    const std::size_t a = path.find_last_of(L"\\/");
    if (a == std::wstring::npos) {
        return {};
    }
    const std::size_t b = path.find_last_of(L"\\/", a == 0 ? 0 : a - 1);
    return b == std::wstring::npos ? std::wstring() : path.substr(0, b);
}

bool is_batch_file(const std::wstring& path) {
    const std::wstring l = lower(path);
    const auto ends = [&](const wchar_t* suffix) {
        const std::wstring s = suffix;
        return l.size() >= s.size() && l.compare(l.size() - s.size(), s.size(), s) == 0;
    };
    return ends(L".cmd") || ends(L".bat");
}

}  // namespace

Result<std::wstring> resolve_editor_program(const std::string& program) {
    const std::wstring name = proc::wide_from_utf8(program);
    const auto not_found = [&]() {
        return Diag::make(ids::kUiEditorLaunch, "cannot find the editor program '" + program + "'",
                          "Install the editor, put it on PATH, or set its full path in the settings (the 'program' field).");
    };
    std::wstring found;
    if (name.find_first_of(L"\\/") != std::wstring::npos || (name.size() > 1 && name[1] == L':')) {
        if (file_exists(name)) {
            found = name;
        } else if (file_exists(name + L".exe")) {
            found = name + L".exe";
        }
    } else {
        found = search_path(name, L".exe");
        if (found.empty() && lower(name) == L"code") {
            // The `code` on PATH is bin\code.cmd; the editor itself is Code.exe one level up.
            const std::wstring cmd = search_path(L"code.cmd", nullptr);
            if (!cmd.empty()) {
                const std::wstring candidate = parent_of_parent(cmd) + L"\\Code.exe";
                if (file_exists(candidate)) {
                    found = candidate;
                }
            }
            const std::wstring local = env(L"LOCALAPPDATA");
            const std::wstring program_files = env(L"ProgramFiles");
            if (found.empty() && !local.empty() && file_exists(local + L"\\Programs\\Microsoft VS Code\\Code.exe")) {
                found = local + L"\\Programs\\Microsoft VS Code\\Code.exe";
            }
            if (found.empty() && !program_files.empty() && file_exists(program_files + L"\\Microsoft VS Code\\Code.exe")) {
                found = program_files + L"\\Microsoft VS Code\\Code.exe";
            }
        }
    }
    if (found.empty()) {
        return not_found();
    }
    if (is_batch_file(found)) {
        return Diag::make(ids::kUiEditorBatchFile, "the editor '" + program + "' resolves to the batch file '" + proc::utf8_from_wide(found) + "'",
                          "Batch files are never started (file names could inject commands). Point the 'program' setting at the editor's .exe.");
    }
    return found;
}

Result<void> launch_editor(const EditorConfig& config, const SourceLocation& location) {
    Result<EditorLaunch> launch = make_editor_launch(config, location);
    if (!launch) {
        return launch.error();
    }
    Result<std::wstring> program = resolve_editor_program(launch->argv.front());
    if (!program) {
        return program.error();
    }
    std::vector<std::string> argv = launch->argv;
    argv.front() = proc::utf8_from_wide(*program);
    const std::wstring command_line = proc::wide_from_utf8(join_command_line(argv));
    std::vector<wchar_t> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(program->c_str(), mutable_line.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup,
                        &info)) {
        return Diag::make(ids::kUiEditorLaunch,
                          "cannot start the editor '" + proc::utf8_from_wide(*program) + "' (error " + std::to_string(GetLastError()) + ")",
                          "Check the editor command in the settings.");
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return {};
}

}  // namespace cml::ui::win
