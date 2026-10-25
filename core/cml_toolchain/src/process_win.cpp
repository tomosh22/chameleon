// CreateProcess with captured output, job-object cancellation, timeouts and @response files.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "cml/toolchain/command_line.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/invoke.h"
#include "cml/util/log.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    }
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    }
    return out;
}

bool iequal_w(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// The inherited environment with `overrides` applied, as a double-NUL terminated block.
std::wstring build_environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    wchar_t* env = GetEnvironmentStringsW();
    if (env != nullptr) {
        for (const wchar_t* p = env; *p != L'\0'; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const std::size_t eq = entry.find(L'=', 1);  // entries like "=C:=C:\" start with '='
            if (eq == std::wstring::npos) {
                continue;
            }
            vars.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& kv : overrides) {
        const std::wstring name = widen(kv.first);
        const std::wstring value = widen(kv.second);
        bool replaced = false;
        for (auto& v : vars) {
            if (iequal_w(v.first, name)) {
                v.second = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            vars.emplace_back(name, value);
        }
    }
    std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.first.c_str(), static_cast<int>(a.first.size()), b.first.c_str(),
                                    static_cast<int>(b.first.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& v : vars) {
        block += v.first;
        block.push_back(L'=');
        block += v.second;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// True for the shared PDB server (matched by image name, case-insensitive).
bool is_pdb_server(HANDLE process) {
    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (!QueryFullProcessImageNameW(process, 0, buf, &len)) {
        return true;  // cannot tell what it is: do not kill it
    }
    const wchar_t* base = buf;
    for (const wchar_t* c = buf; *c != L'\0'; ++c) {
        if (*c == L'\\' || *c == L'/') {
            base = c + 1;
        }
    }
    return _wcsicmp(base, L"mspdbsrv.exe") == 0;
}

// Process ids currently inside `job` (empty when the query fails).
std::vector<DWORD> job_process_ids(HANDLE job) {
    constexpr DWORD kMax = 4096;
    std::vector<char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + kMax * sizeof(ULONG_PTR));
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    std::vector<DWORD> ids;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buffer.size()), nullptr)) {
        for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
            ids.push_back(static_cast<DWORD>(list->ProcessIdList[i]));
        }
    }
    return ids;
}

// State shared between the running process and the cancellation / timeout killers.
//
// Rule: cancellation kills ONLY processes that are inside OUR job object, and NEVER mspdbsrv.exe. The tree is never
// found by walking parent process ids (mspdbsrv.exe is started by whichever cl /Zi needs it first, so its parent pid
// is often a compiler that is being cancelled, and parent pids are also stale after pid reuse).
//
// The job has JOB_OBJECT_LIMIT_BREAKAWAY_OK (no silent breakaway, no KILL_ON_JOB_CLOSE), so the compiler's helper
// processes (c1xx, link, ...) join the job. kill() enumerates the job (JobObjectBasicProcessIdList), skips mspdbsrv.exe
// by image name, and terminates each of the others individually. TerminateJobObject is never used: it would also kill
// the shared PDB server, which other concurrent /Zi compiles use. Closing the job handle leaves mspdbsrv.exe running.
struct Killable {
    std::mutex mutex;
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    bool finished = false;
    bool killed = false;

    // True when this call killed the process tree; false when the process had already exited or finish() ran.
    bool kill() {
        std::lock_guard<std::mutex> lock(mutex);
        if (finished || killed) {
            return false;
        }
        if (process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            return false;  // exited on its own just now: not a cancellation
        }
        killed = true;
        if (job != nullptr) {
            // A few passes: a dying helper may still have spawned a child between the query and the terminate.
            for (int pass = 0; pass < 4; ++pass) {
                bool any = false;
                for (const DWORD id : job_process_ids(job)) {
                    if (id == pid) {
                        continue;  // the driver goes last
                    }
                    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id);
                    if (h == nullptr) {
                        continue;
                    }
                    if (!is_pdb_server(h) && WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
                        TerminateProcess(h, 1);
                        any = true;
                    }
                    CloseHandle(h);
                }
                if (!any) {
                    break;
                }
            }
        }
        if (process != nullptr) {
            TerminateProcess(process, 1);
        }
        return true;
    }
    void finish() {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
};

std::atomic<unsigned> g_rsp_counter{0};

struct HandleCloser {
    HANDLE h = nullptr;
    ~HandleCloser() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};

}  // namespace

Result<ProcessResult> run_process(const ProcessSpec& spec, CancelToken* cancel) {
    ProcessResult res;
    if (cancel != nullptr && cancel->cancelled()) {
        res.cancelled = true;
        return res;
    }

    const std::string program = quote_argument(spec.exe);
    const std::string args_line = join_command_line(spec.args);
    std::string command = args_line.empty() ? program : program + " " + args_line;

    std::wstring rsp_path_w;
    if (spec.force_response_file || command.size() > spec.max_command_line) {
        std::wstring dir = widen(spec.response_file_dir);
        if (dir.empty()) {
            wchar_t tmp[MAX_PATH + 2];
            const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
            dir.assign(tmp, n);
        }
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
            dir.push_back(L'\\');
        }
        rsp_path_w = dir + L"cml_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(g_rsp_counter.fetch_add(1)) + L".rsp";
        HandleCloser file;
        file.h = CreateFileW(rsp_path_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file.h == INVALID_HANDLE_VALUE) {
            file.h = nullptr;
            return Diag::make(diag::kCannotPrepareOutput, "cannot create response file '" + narrow(rsp_path_w) + "'",
                              "Check that the temp directory is writable.");
        }
        const std::string bytes = std::string("\xEF\xBB\xBF") + args_line;  // UTF-8 with BOM
        DWORD written = 0;
        if (!WriteFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
            return Diag::make(diag::kCannotPrepareOutput, "cannot write response file '" + narrow(rsp_path_w) + "'",
                              "Check that the disk is not full.");
        }
        res.response_file = narrow(rsp_path_w);
        command = program + " @" + quote_argument(res.response_file);
    }
    struct RspCleanup {
        const std::wstring& path;
        ~RspCleanup() {
            if (!path.empty()) {
                DeleteFileW(path.c_str());
            }
        }
    } rsp_cleanup{rsp_path_w};
    res.command_line = command;

    // ---- pipe, NUL stdin ---------------------------------------------------------------------
    // The read end is an overlapped named-pipe handle so the reader can always be stopped (a grandchild that inherited
    // the write end would otherwise keep a blocking ReadFile alive for as long as it runs).
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HandleCloser read_end;
    HandleCloser write_end;
    HandleCloser nul;
    {
        static std::atomic<unsigned> pipe_counter{0};
        wchar_t pipe_name[96];
        swprintf(pipe_name, sizeof(pipe_name) / sizeof(pipe_name[0]), L"\\\\.\\pipe\\cml_toolchain_%lu_%u", GetCurrentProcessId(),
                 pipe_counter.fetch_add(1));
        read_end.h = CreateNamedPipeW(pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
        if (read_end.h == INVALID_HANDLE_VALUE) {
            read_end.h = nullptr;
        } else {
            write_end.h = CreateFileW(pipe_name, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (write_end.h == INVALID_HANDLE_VALUE) {
                write_end.h = nullptr;
            }
        }
    }
    if (read_end.h == nullptr || write_end.h == nullptr) {
        return Diag::make(diag::kCannotStartCompiler, "cannot create a pipe for '" + spec.exe + "' (error " + std::to_string(GetLastError()) + ")",
                          "Close other programs or retry.");
    }
    nul.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul.h == INVALID_HANDLE_VALUE) {
        nul.h = nullptr;
    }

    // Only the pipe and NUL are inherited: parallel CreateProcess calls must not leak each other's
    // pipe ends, or a reader would wait for a process that is not its own.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_storage(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        return Diag::make(diag::kCannotStartCompiler, "cannot prepare the process attributes for '" + spec.exe + "'",
                          "Retry; if this persists report it with the log.");
    }
    struct AttrCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttrCleanup() { DeleteProcThreadAttributeList(list); }
    } attr_cleanup{attrs};
    HANDLE inherit[2] = {write_end.h, nul.h};
    const DWORD inherit_count = nul.h != nullptr ? 2 : 1;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inherit_count * sizeof(HANDLE), nullptr, nullptr)) {
        return Diag::make(diag::kCannotStartCompiler, "cannot restrict the inherited handles for '" + spec.exe + "'",
                          "Retry; if this persists report it with the log.");
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul.h;
    si.StartupInfo.hStdOutput = write_end.h;
    si.StartupInfo.hStdError = write_end.h;
    si.lpAttributeList = attrs;

    std::wstring command_w = widen(command);
    command_w.push_back(L'\0');
    const std::wstring cwd_w = widen(spec.cwd);
    std::wstring env_block;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    if (!spec.env_overrides.empty()) {
        env_block = build_environment_block(spec.env_overrides);
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(nullptr, command_w.data(), nullptr, nullptr, TRUE, flags,
                                        env_block.empty() ? nullptr : env_block.data(),
                                        cwd_w.empty() ? nullptr : cwd_w.c_str(), &si.StartupInfo, &pi);
    if (!created) {
        const DWORD err = GetLastError();
        return Diag::make(diag::kCannotStartCompiler,
                          "cannot start '" + spec.exe + "' (error " + std::to_string(err) + ")" +
                              (spec.cwd.empty() ? "" : " in '" + spec.cwd + "'"),
                          "Check that the compiler exists and the working directory is valid.");
    }
    HandleCloser process{pi.hProcess};
    HandleCloser thread{pi.hThread};
    CloseHandle(write_end.h);  // the child owns its copy now
    write_end.h = nullptr;

    // ---- job object -----------------------------------------------------------------------------
    auto killable = std::make_shared<Killable>();
    HandleCloser job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    if (job.h != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // See Killable: no silent breakaway, no KILL_ON_JOB_CLOSE; the job is only used to enumerate our processes.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job.h, pi.hProcess)) {
            CML_LOG(Debug, "toolchain: AssignProcessToJobObject failed (%lu); cancelling will kill the main process only",
                    static_cast<unsigned long>(GetLastError()));
            CloseHandle(job.h);
            job.h = nullptr;
        }
    }
    killable->job = job.h;
    killable->process = pi.hProcess;
    killable->pid = pi.dwProcessId;

    CancelToken::Handle killer_handle = 0;
    if (cancel != nullptr) {
        killer_handle = cancel->add_killer([killable]() { killable->kill(); });
    }

    // Timeout watchdog: fires only while the process is still running (Killable::kill checks).
    std::atomic<bool> timed_out{false};
    HandleCloser done_event;
    std::thread watchdog;
    if (spec.timeout_ms != 0) {
        done_event.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE ev = done_event.h;
        const unsigned ms = spec.timeout_ms;
        watchdog = std::thread([killable, ev, ms, &timed_out]() {
            if (WaitForSingleObject(ev, ms) == WAIT_TIMEOUT && killable->kill()) {
                timed_out.store(true);
            }
        });
    }

    ResumeThread(pi.hThread);

    // ---- output reader (overlapped, stoppable) ------------------------------------------------------
    HandleCloser stop_event;
    stop_event.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HandleCloser reader_finished;
    reader_finished.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string captured;
    HANDLE read_handle = read_end.h;
    HANDLE stop_h = stop_event.h;
    HANDLE finished_h = reader_finished.h;
    std::thread reader([&captured, read_handle, stop_h, finished_h]() {
        HandleCloser io_event;
        io_event.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const HANDLE waits[2] = {io_event.h, stop_h};
        char buf[8192];
        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = io_event.h;
            ResetEvent(io_event.h);
            DWORD n = 0;
            if (!ReadFile(read_handle, buf, sizeof(buf), &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    break;  // broken pipe: every writer is gone
                }
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                    CancelIoEx(read_handle, &ov);
                    if (GetOverlappedResult(read_handle, &ov, &n, TRUE) && n > 0) {
                        captured.append(buf, n);
                    }
                    break;
                }
                if (!GetOverlappedResult(read_handle, &ov, &n, FALSE)) {
                    break;
                }
            }
            if (n == 0) {
                break;
            }
            captured.append(buf, n);
        }
        SetEvent(finished_h);
    });

    WaitForSingleObject(pi.hProcess, INFINITE);
    // The process is gone. Nothing may be reported as a timeout or cancellation from here on, so the killers are
    // disarmed first; only then the output is drained.
    killable->finish();
    if (watchdog.joinable()) {
        SetEvent(done_event.h);
        watchdog.join();
    }
    if (cancel != nullptr) {
        cancel->remove_killer(killer_handle);
    }
    // A grandchild that inherited the pipe may keep it open: give the reader a bounded grace period, then stop it.
    if (WaitForSingleObject(reader_finished.h, static_cast<DWORD>(kProcessDrainGraceMs)) != WAIT_OBJECT_0) {
        SetEvent(stop_event.h);
    }
    reader.join();

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = static_cast<int>(code);
    res.output = std::move(captured);
    res.timed_out = timed_out.load();
    res.cancelled = killable->killed && !res.timed_out;
    return res;
}

}  // namespace cml::toolchain
