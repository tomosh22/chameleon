// The Windows ProcessHost of the hot-restart (WP-21, restart.h): take the standard handles out of the old process,
// terminate it, start the same image again with those handles as the only inherited ones.
#include "cml/core/restart.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "cml/core/restart_diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

namespace rd = restart::diag;

namespace {

std::string win32_error(DWORD error) {
    return "Win32 error " + std::to_string(error);
}

std::wstring wide_of(const std::u16string& text) {
    return std::wstring(reinterpret_cast<const wchar_t*>(text.data()), text.size());
}

std::string utf8_of(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

bool is_none(std::uint64_t value) {
    return value == 0 || value == static_cast<std::uint64_t>(-1);
}

class WindowsRestartableProcess final : public RestartableProcess {
public:
    WindowsRestartableProcess(HANDLE old_process, const HANDLE duplicated[3]) : old_process_(old_process) {
        for (int i = 0; i < 3; ++i) {
            duplicated_[i] = duplicated[i];
        }
    }
    ~WindowsRestartableProcess() override {
        close_duplicates();
        if (new_process_ != nullptr) {
            CloseHandle(new_process_);
        }
        CloseHandle(old_process_);
    }

    Result<void> terminate(std::uint32_t timeout_ms) override {
        if (WaitForSingleObject(old_process_, 0) != WAIT_OBJECT_0) {
            if (!TerminateProcess(old_process_, 0)) {
                const DWORD error = GetLastError();
                if (WaitForSingleObject(old_process_, 0) != WAIT_OBJECT_0) {
                    return Diag::make(rd::kTerminateFailed, "TerminateProcess failed (" + win32_error(error) + ")", "Close the process yourself.");
                }
            }
        }
        if (WaitForSingleObject(old_process_, timeout_ms) != WAIT_OBJECT_0) {
            return Diag::make(rd::kTerminateFailed, "the process did not exit within " + std::to_string(timeout_ms) + " ms after TerminateProcess",
                              "Close the process yourself.");
        }
        return Result<void>();
    }

    Result<std::uint32_t> relaunch(const RelaunchSpec& spec) override {
        const auto fail = [](const std::string& why) {
            return Diag::make(rd::kLaunchFailed, why, "Check that the image still exists and can be started.");
        };
        // The handles to hand on: the distinct duplicates (stdout and stderr often share one), and these only.
        std::vector<HANDLE> inherited;
        for (int i = 0; i < 3; ++i) {
            if (duplicated_[i] != nullptr && std::find(inherited.begin(), inherited.end(), duplicated_[i]) == inherited.end()) {
                inherited.push_back(duplicated_[i]);
            }
        }
        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<char> attribute_storage(attribute_bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) {
            return fail("InitializeProcThreadAttributeList failed (" + win32_error(GetLastError()) + ")");
        }
        if (!inherited.empty() &&
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(attributes);
            return fail("UpdateProcThreadAttribute failed (" + win32_error(error) + ")");
        }
        STARTUPINFOEXW startup;
        ZeroMemory(&startup, sizeof(startup));
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = duplicated_[0];
        startup.StartupInfo.hStdOutput = duplicated_[1];
        startup.StartupInfo.hStdError = duplicated_[2];
        startup.lpAttributeList = inherited.empty() ? nullptr : attributes;

        const std::wstring image = wide_of(spec.image_path);
        const std::wstring cwd = wide_of(spec.working_directory);
        const std::wstring command_text = wide_of(spec.command_line);
        std::vector<wchar_t> command(command_text.begin(), command_text.end());
        command.push_back(L'\0');
        const std::wstring environment_text = wide_of(spec.environment);
        std::vector<wchar_t> environment(environment_text.begin(), environment_text.end());
        if (environment.size() < 2 || environment[environment.size() - 1] != L'\0' || environment[environment.size() - 2] != L'\0') {
            environment.push_back(L'\0');
            environment.push_back(L'\0');
        }

        PROCESS_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        DWORD flags = CREATE_UNICODE_ENVIRONMENT;
        if (!inherited.empty()) {
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        const BOOL created = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, inherited.empty() ? FALSE : TRUE, flags, environment.data(),
                                            cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
        const DWORD create_error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        close_duplicates();  // the new process has its own copies now
        if (!created) {
            return fail("CreateProcessW failed for '" + utf8_of(image) + "' (" + win32_error(create_error) + ")");
        }
        CloseHandle(info.hThread);
        new_process_ = info.hProcess;
        return static_cast<std::uint32_t>(info.dwProcessId);
    }

    std::optional<std::uint32_t> relaunched_exit_code() override {
        if (new_process_ == nullptr || WaitForSingleObject(new_process_, 0) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(new_process_, &code);
        return static_cast<std::uint32_t>(code);
    }

private:
    void close_duplicates() {
        for (int i = 0; i < 3; ++i) {
            if (duplicated_[i] == nullptr) {
                continue;
            }
            const HANDLE h = duplicated_[i];
            for (int j = i; j < 3; ++j) {  // stdout and stderr may share one duplicate: close it once
                if (duplicated_[j] == h) {
                    duplicated_[j] = nullptr;
                }
            }
            CloseHandle(h);
        }
    }

    HANDLE old_process_;
    HANDLE duplicated_[3] = {nullptr, nullptr, nullptr};
    HANDLE new_process_ = nullptr;
};

class WindowsProcessHost final : public ProcessHost {
public:
    Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t pid, const std::uint64_t std_handles[3], std::vector<Diag>* warnings) override {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            return Diag::make(rd::kOpenFailed, "OpenProcess(" + std::to_string(pid) + ") failed (" + win32_error(GetLastError()) + ")",
                              "Run the broker as the same user as the application.");
        }
        static const char* const kNames[3] = {"standard input", "standard output", "standard error"};
        HANDLE duplicated[3] = {nullptr, nullptr, nullptr};
        for (int i = 0; i < 3; ++i) {
            if (is_none(std_handles[i])) {
                continue;
            }
            bool shared = false;  // the same handle value as an earlier one: the same duplicate
            for (int j = 0; j < i; ++j) {
                if (std_handles[j] == std_handles[i] && duplicated[j] != nullptr) {
                    duplicated[i] = duplicated[j];
                    shared = true;
                    break;
                }
            }
            if (shared) {
                continue;
            }
            HANDLE copy = nullptr;
            // Inheritable: the new process receives it through the handle list of CreateProcess.
            if (!DuplicateHandle(process, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std_handles[i])), GetCurrentProcess(), &copy, 0, TRUE,
                                 DUPLICATE_SAME_ACCESS)) {
                if (warnings != nullptr) {
                    warnings->push_back(Diag::make(rd::kStdHandle,
                                                   std::string("the ") + kNames[i] + " handle of process " + std::to_string(pid) + " could not be copied (" +
                                                       win32_error(GetLastError()) + "); the new process starts without it",
                                                   "Redirect the output to a file or a pipe the application owns, or restart it from its launcher."));
                }
                continue;
            }
            duplicated[i] = copy;
        }
        return std::unique_ptr<RestartableProcess>(new WindowsRestartableProcess(process, duplicated));
    }
};

}  // namespace

std::shared_ptr<ProcessHost> native_process_host() {
    return std::make_shared<WindowsProcessHost>();
}

}  // namespace cml::core

#endif  // _WIN32
