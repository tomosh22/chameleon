// The Windows ProcessHost of the hot-restart (WP-21, restart.h): take the standard handles out of the old process,
// terminate it, start the same image again with those handles as the only inherited ones.
#include "cml/core/restart.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "cml/core/restart_diag_ids.h"
#include "cml/ipc/messages.h"
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

// Resumes every suspend count of `thread` (the broker died while the process was held).
void resume_fully(HANDLE thread) {
    for (int i = 0; i < 64; ++i) {
        const DWORD previous = ResumeThread(thread);
        if (previous == static_cast<DWORD>(-1) || previous <= 1) {
            return;
        }
    }
}

HANDLE handle_from_text(const std::string& text) {
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(text.c_str(), nullptr, 10)));
}

constexpr DWORD kWatchdogDoneWaitMs = 10000;  // how long the watchdog waits for the broker to finish a resume it announced

class WindowsRestartableProcess final : public RestartableProcess {
public:
    WindowsRestartableProcess(HANDLE old_process, std::uint32_t old_pid, bool had_console, const HANDLE duplicated[3])
        : old_process_(old_process), old_pid_(old_pid), had_console_(had_console) {
        for (int i = 0; i < 3; ++i) {
            duplicated_[i] = duplicated[i];
        }
    }
    ~WindowsRestartableProcess() override {
        detach_console();
        close_duplicates();
        // WP-34: a process that is still suspended is never left behind (an error path that returned before resume()).
        (void)resume();
        close_watchdog();
        if (new_process_ != nullptr) {
            CloseHandle(new_process_);
        }
        CloseHandle(old_process_);
    }

    Result<void> terminate(std::uint32_t timeout_ms) override {
        // Console (docs/wp/WP-21.md): the new process should end up where the old one was. A broker that has no console of its own
        // (detached, e.g. started by the agent) takes the old process's console over while the old process still exists, so the child
        // it creates inherits that console instead of getting a window of its own. A broker that has a console gives the child its own
        // (the child cannot be attached to a different one by flags); a process that had none gets none (DETACHED_PROCESS).
        if (had_console_) {
            DWORD attached[1] = {0};
            if (GetConsoleProcessList(attached, 1) == 0) {
                if (AttachConsole(old_pid_)) {
                    console_attached_ = true;
                } else {
                    CML_LOG(Warn, "restart: cannot take over the console of process %u (%s); the new process gets a console of its own", old_pid_,
                            win32_error(GetLastError()).c_str());
                }
            }
        }
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
        if (spec.suspended) {
            flags |= CREATE_SUSPENDED;  // WP-34: no instruction runs (no agent, no broker connection) until resume()
        }
        if (!had_console_) {
            flags |= DETACHED_PROCESS;  // the old process had no console: a console application must not get a window now
        }
        if (!inherited.empty()) {
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        const BOOL created = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, inherited.empty() ? FALSE : TRUE, flags, environment.data(),
                                            cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
        const DWORD create_error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        close_duplicates();  // the new process has its own copies now
        detach_console();    // the broker lets go of the console it borrowed
        if (!created) {
            return fail("CreateProcessW failed for '" + utf8_of(image) + "' (" + win32_error(create_error) + ")");
        }
        if (spec.suspended) {
            main_thread_ = info.hThread;  // kept to resume; closed by resume()
            start_watchdog(info.hProcess);
        } else {
            CloseHandle(info.hThread);
        }
        new_process_ = info.hProcess;
        return static_cast<std::uint32_t>(info.dwProcessId);
    }

    // WP-34. Resumes the initial thread of a process created suspended. A debugger that attached meanwhile keeps its own suspend counts
    // (ResumeThread takes back only the one this host added). A process that is gone needs nothing. When the thread cannot be resumed
    // the process is terminated: a process that stays suspended forever is worse than one that is not there.
    Result<void> resume() override {
        if (main_thread_ == nullptr) {
            return Result<void>();
        }
        const HANDLE thread = main_thread_;
        main_thread_ = nullptr;
        // The watchdog (start_watchdog) is told first that the hold ended, so it never resumes a thread this broker resumes itself. Should the
        // broker die between the two events, the watchdog resumes (a no-op if the thread already runs, which is what suspend count 0 is).
        if (hold_ended_ != nullptr) {
            SetEvent(hold_ended_);
        }
        Result<void> outcome;
        if (new_process_ != nullptr && WaitForSingleObject(new_process_, 0) == WAIT_OBJECT_0) {
            // already exited (a debugger or the user ended it): nothing to resume
        } else if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
            const DWORD error = GetLastError();
            if (new_process_ != nullptr) {
                TerminateProcess(new_process_, 1);
            }
            outcome = Diag::make(rd::kResumeFailed, "ResumeThread failed (" + win32_error(error) + "); the process was terminated instead of being left suspended",
                                 "Start the application yourself.");
        }
        CloseHandle(thread);
        if (hold_done_ != nullptr) {
            SetEvent(hold_done_);  // the watchdog exits without touching anything
        }
        close_watchdog();
        return outcome;
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
    // WP-34: a broker that is killed while it holds the process cannot resume it. A small child, this very executable in the hidden mode
    // `--hold-watchdog`, waits for the broker's process (and for the hold to end normally) and resumes the thread when the broker goes first.
    // It inherits exactly four handles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): the broker process, the held thread, the two events; none of the
    // broker's pipes. It ends with the hold, when the thread ends, or after hold_max (a hold lasts at most 60 s) at the latest.
    void start_watchdog(HANDLE new_process) {
        wchar_t exe[MAX_PATH * 2];
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        if (length == 0 || length >= MAX_PATH * 2) {
            return;
        }
        const std::wstring path(exe, length);
        const std::size_t slash = path.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (_wcsicmp(name.c_str(), L"cml_broker.exe") != 0) {
            CML_LOG(Debug, "restart: no hold watchdog (this executable is not cml_broker.exe)");
            return;  // a test host or a tool embedding the library: there is no hidden mode to start
        }
        SECURITY_ATTRIBUTES inheritable;
        ZeroMemory(&inheritable, sizeof(inheritable));
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;
        HANDLE ended = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
        HANDLE done = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
        HANDLE broker_copy = nullptr;
        HANDLE thread_copy = nullptr;
        const HANDLE self = GetCurrentProcess();
        const bool ok = ended != nullptr && done != nullptr &&
                        DuplicateHandle(self, self, self, &broker_copy, SYNCHRONIZE, TRUE, 0) &&
                        DuplicateHandle(self, main_thread_, self, &thread_copy, THREAD_SUSPEND_RESUME | SYNCHRONIZE, TRUE, 0);
        (void)new_process;
        bool started = false;
        if (ok) {
            HANDLE list[4] = {broker_copy, thread_copy, ended, done};
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<char> storage(bytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
                if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(list), nullptr, nullptr)) {
                    const auto text = [](HANDLE h) { return std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(h))); };
                    std::wstring command = L"\"" + path + L"\" --hold-watchdog " + text(broker_copy) + L" " + text(thread_copy) + L" " + text(ended) + L" " +
                                           text(done) + L" 75000";
                    std::vector<wchar_t> mutable_command(command.begin(), command.end());
                    mutable_command.push_back(L'\0');
                    STARTUPINFOEXW startup;
                    ZeroMemory(&startup, sizeof(startup));
                    startup.StartupInfo.cb = sizeof(startup);
                    startup.lpAttributeList = attributes;
                    PROCESS_INFORMATION info;
                    ZeroMemory(&info, sizeof(info));
                    if (CreateProcessW(path.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                       nullptr, &startup.StartupInfo, &info)) {
                        CloseHandle(info.hThread);
                        watchdog_ = info.hProcess;
                        started = true;
                    }
                }
                DeleteProcThreadAttributeList(attributes);
            }
        }
        if (broker_copy != nullptr) {
            CloseHandle(broker_copy);  // the watchdog has its own
        }
        if (thread_copy != nullptr) {
            CloseHandle(thread_copy);
        }
        if (started) {
            hold_ended_ = ended;
            hold_done_ = done;
            CML_LOG(Debug, "restart: hold watchdog started (resumes the held process if this broker dies)");
        } else {
            if (ended != nullptr) {
                CloseHandle(ended);
            }
            if (done != nullptr) {
                CloseHandle(done);
            }
            CML_LOG(Warn, "restart: could not start the hold watchdog (Win32 error %lu); a broker that is killed while holding would leave the process suspended",
                    GetLastError());
        }
    }

    void close_watchdog() {
        for (HANDLE* h : {&watchdog_, &hold_ended_, &hold_done_}) {
            if (*h != nullptr) {
                CloseHandle(*h);
                *h = nullptr;
            }
        }
    }

    void detach_console() {
        if (console_attached_) {
            FreeConsole();
            console_attached_ = false;
        }
    }

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
    std::uint32_t old_pid_;
    bool had_console_;
    bool console_attached_ = false;
    HANDLE duplicated_[3] = {nullptr, nullptr, nullptr};
    HANDLE new_process_ = nullptr;
    HANDLE watchdog_ = nullptr;     // WP-34: the watchdog child (its handle only; it ends by itself)
    HANDLE hold_ended_ = nullptr;   // WP-34: set before the broker resumes the thread
    HANDLE hold_done_ = nullptr;    // WP-34: set after
    HANDLE main_thread_ = nullptr;  // WP-34: the suspended initial thread of the new process; null once resumed
};

class WindowsProcessHost final : public ProcessHost {
public:
    bool supports_hold() const override { return true; }

    Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t pid, const std::uint64_t std_handles[3], std::uint32_t info_flags,
                                                     std::vector<Diag>* warnings) override {
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
        return std::unique_ptr<RestartableProcess>(new WindowsRestartableProcess(process, pid, (info_flags & ipc::kRestartInfoHadConsole) != 0, duplicated));
    }
};

}  // namespace

std::shared_ptr<ProcessHost> native_process_host() {
    return std::make_shared<WindowsProcessHost>();
}

// The watchdog process (see start_watchdog). Waits for: the held thread to end (nothing to do), the broker's `ended` event (the broker is resuming;
// wait for `done`), or the broker's process to end (resume the thread unless `done` is set). It must not outlive the hold: `max_ms` bounds it.
int run_hold_watchdog(const std::vector<std::string>& args) {
    if (args.size() < 5) {
        return 2;
    }
    const HANDLE broker = handle_from_text(args[0]);
    const HANDLE thread = handle_from_text(args[1]);
    const HANDLE ended = handle_from_text(args[2]);
    const HANDLE done = handle_from_text(args[3]);
    const DWORD max_ms = static_cast<DWORD>(std::strtoul(args[4].c_str(), nullptr, 10));
    const ULONGLONG deadline = GetTickCount64() + max_ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return 0;  // the hold is over by now whatever happened
        }
        const HANDLE waits[4] = {done, ended, thread, broker};
        const DWORD r = WaitForMultipleObjects(4, waits, FALSE, static_cast<DWORD>(deadline - now));
        if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 2) {
            return 0;  // the broker finished the resume / the thread has ended
        }
        if (r == WAIT_OBJECT_0 + 1) {
            // The broker announced the resume: wait for it to finish; if it dies first, finish it.
            const HANDLE after[3] = {done, thread, broker};
            const DWORD d = WaitForMultipleObjects(3, after, FALSE, kWatchdogDoneWaitMs);
            if (d == WAIT_OBJECT_0 + 2 && WaitForSingleObject(done, 0) != WAIT_OBJECT_0) {
                resume_fully(thread);
            }
            return 0;
        }
        if (r == WAIT_OBJECT_0 + 3) {
            if (WaitForSingleObject(done, 0) != WAIT_OBJECT_0) {
                resume_fully(thread);  // the broker is gone and did not resume the process
            }
            return 0;
        }
        if (r == WAIT_TIMEOUT) {
            return 0;
        }
        return 1;
    }
}

}  // namespace cml::core

#endif  // _WIN32
