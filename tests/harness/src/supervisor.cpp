#include "supervisor.h"

#if defined(_WIN32)

#include <dbghelp.h>

#include <algorithm>
#include <cwchar>

#include "cml/ipc/control_ids.h"
#include "cml/proc/proc.h"

namespace cmltest {

namespace fs = std::filesystem;

bool CaseInsensitiveLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_LESS_THAN;
}

std::wstring quote_argument(const std::wstring& text) {
    return L"\"" + text + L"\"";
}

namespace {

struct HandleCloser {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit HandleCloser(HANDLE h = INVALID_HANDLE_VALUE) : handle(h) {}
    ~HandleCloser() { reset(); }
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
    bool valid() const { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
    void reset() {
        if (valid()) {
            CloseHandle(handle);
        }
        handle = INVALID_HANDLE_VALUE;
    }
};

std::string win32_error_text(DWORD error) {
    return "Win32 error " + std::to_string(error);
}

// The inherited environment with `overrides` applied, as a double-NUL terminated block.
std::vector<wchar_t> environment_block(const std::map<std::wstring, std::wstring, CaseInsensitiveLess>& overrides) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> merged;
    wchar_t* block = GetEnvironmentStringsW();
    if (block != nullptr) {
        for (const wchar_t* p = block; *p != L'\0'; p += std::wcslen(p) + 1) {
            const std::wstring entry(p);
            // Entries like "=C:=C:\dir" start with '=': the name begins after it.
            const std::size_t eq = entry.find(L'=', entry.empty() || entry[0] != L'=' ? 0 : 1);
            if (eq == std::wstring::npos) {
                continue;
            }
            merged[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& entry : overrides) {
        merged[entry.first] = entry.second;
    }
    std::vector<wchar_t> out;
    for (const auto& entry : merged) {
        out.insert(out.end(), entry.first.begin(), entry.first.end());
        out.push_back(L'=');
        out.insert(out.end(), entry.second.begin(), entry.second.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

}  // namespace

Supervisor::Supervisor() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_ != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }
}

Supervisor::~Supervisor() {
    kill_all();
    for (Child& child : children_) {
        if (child.process != nullptr) {
            CloseHandle(child.process);
        }
    }
    if (job_ != nullptr) {
        CloseHandle(job_);  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE kills anything left
    }
}

cml::Result<std::size_t> Supervisor::start(const ChildSpec& spec) {
    const auto fail = [&](const std::string& why) {
        return cml::Diag::make(cml::ids::kLaunchFailed, "cannot start " + spec.label + " (" + spec.exe.string() + "): " + why,
                               "Check that the tool exists, the path is right and the system has free resources.");
    };
    if (job_ == nullptr) {
        return fail("the kill-on-close job object could not be created, so the harness refuses to start children");
    }
    SECURITY_ATTRIBUTES inheritable;
    ZeroMemory(&inheritable, sizeof(inheritable));
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HandleCloser in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!in.valid()) {
        return fail("cannot open NUL (" + win32_error_text(GetLastError()) + ")");
    }
    HandleCloser out;
    if (spec.output_file.empty()) {
        out.handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    } else {
        std::error_code ec;
        fs::create_directories(spec.output_file.parent_path(), ec);
        out.handle = CreateFileW(spec.output_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (!out.valid()) {
        return fail("cannot open the output file '" + spec.output_file.string() + "' (" + win32_error_text(GetLastError()) + ")");
    }

    // Only these two handles are inherited by the child.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<char> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) {
        return fail("InitializeProcThreadAttributeList failed (" + win32_error_text(GetLastError()) + ")");
    }
    HANDLE inherited[2] = {in.handle, out.handle};
    const bool attribute_ok = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                                        sizeof(inherited), nullptr, nullptr) != FALSE;
    if (!attribute_ok) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        return fail("UpdateProcThreadAttribute failed (" + win32_error_text(error) + ")");
    }

    STARTUPINFOEXW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in.handle;
    startup.StartupInfo.hStdOutput = out.handle;
    startup.StartupInfo.hStdError = out.handle;
    startup.lpAttributeList = attributes;

    std::wstring command = quote_argument(spec.exe.wstring());
    if (!spec.arguments.empty()) {
        command += L" " + spec.arguments;
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    std::vector<wchar_t> env = environment_block(spec.environment);
    const std::wstring cwd = spec.working_directory.wstring();

    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    const BOOL created = CreateProcessW(spec.exe.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                        env.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        return fail(win32_error_text(create_error));
    }
    HandleCloser thread(info.hThread);
    if (!AssignProcessToJobObject(job_, info.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hProcess);
        return fail("cannot put the process into the kill-on-close job (" + win32_error_text(error) +
                    "); refusing to run it unsupervised");
    }
    ResumeThread(thread.handle);

    Child child;
    child.label = spec.label;
    child.process = info.hProcess;
    child.pid = info.dwProcessId;
    children_.push_back(std::move(child));
    return children_.size() - 1;
}

std::uint32_t Supervisor::pid(std::size_t id) const {
    return id < children_.size() ? children_[id].pid : 0;
}

const std::string& Supervisor::label(std::size_t id) const {
    static const std::string kEmpty;
    return id < children_.size() ? children_[id].label : kEmpty;
}

std::optional<std::uint32_t> Supervisor::exit_code(std::size_t id) const {
    if (id >= children_.size() || children_[id].process == nullptr) {
        return std::nullopt;
    }
    if (WaitForSingleObject(children_[id].process, 0) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(children_[id].process, &code);
    return static_cast<std::uint32_t>(code);
}

bool Supervisor::wait_exit(std::size_t id, std::uint32_t timeout_ms) {
    if (id >= children_.size() || children_[id].process == nullptr) {
        return true;
    }
    return WaitForSingleObject(children_[id].process, timeout_ms) == WAIT_OBJECT_0;
}

Supervisor::DumpResult Supervisor::dump_running(const fs::path& directory) {
    DumpResult result;
    std::error_code ec;
    fs::create_directories(directory, ec);
    for (const Child& child : children_) {
        if (child.process == nullptr || WaitForSingleObject(child.process, 0) == WAIT_OBJECT_0) {
            continue;
        }
        const fs::path file = directory / (child.label + "-" + std::to_string(child.pid) + ".dmp");
        HandleCloser handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid()) {
            result.errors.push_back(cml::Diag::make(cml::ids::kDumpFailed,
                                                    "cannot create the dump file '" + file.string() + "' (" + win32_error_text(GetLastError()) + ")",
                                                    "Check that the artifact directory is writable.").format());
            continue;
        }
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
                                                              MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo);
        if (!MiniDumpWriteDump(child.process, child.pid, handle.handle, type, nullptr, nullptr, nullptr)) {
            result.errors.push_back(cml::Diag::make(cml::ids::kDumpFailed,
                                                    "MiniDumpWriteDump failed for " + child.label + " (pid " + std::to_string(child.pid) + ", " +
                                                        win32_error_text(GetLastError()) + ")",
                                                    "The process may have exited meanwhile; the logs in the artifact directory still apply.")
                                        .format());
            continue;
        }
        result.files.push_back(file);
    }
    return result;
}

void Supervisor::kill(std::size_t id) {
    if (id < children_.size() && children_[id].process != nullptr && WaitForSingleObject(children_[id].process, 0) != WAIT_OBJECT_0) {
        TerminateProcess(children_[id].process, 1);
        WaitForSingleObject(children_[id].process, 5000);
    }
}

void Supervisor::kill_all() {
    for (std::size_t i = 0; i < children_.size(); ++i) {
        kill(i);
    }
    if (job_ != nullptr) {
        TerminateJobObject(job_, 1);  // grandchildren (compilers started by ninja, ...)
    }
}

}  // namespace cmltest

#endif  // _WIN32
