// Integration driver for the agent <-> headless broker acceptance tests (WP-11).
//
//   cml_it_agent --broker <cml_broker.exe> --fixture <fixture.exe> --module-name <file name>
//                --mode prestart|autostart|nobroker --workdir <dir> [--ticks N] [--timeout-ms N]
//
// prestart   starts `cml_broker --headless --pipe <unique> --exit-when-idle 2000 --log <file>`,
//            then the fixture with `--agent`; the broker log must show the agent's Hello,
//            ModuleList and a successful EnableModule for the fixture's exe.
// autostart  does not start a broker: the agent must find none, start cml_broker.exe itself
//            (CML_BROKER_PATH, with the same --exit-when-idle / --log options passed through
//            CML_BROKER_ARGS) and register with it. The agent's own log proves the auto-start.
// nobroker   no broker exists and the agent cannot start one: the fixture must still run all its
//            ticks and exit 0 (the application is never held up by the agent).
//
// The pipe name is unique per run, so parallel CI jobs and reruns never collide. Everything the
// test starts is put in a kill-on-close job object, so a failure (or a timeout) can never leave
// a stray fixture or broker behind. Every wait has a timeout; nothing sleeps for a fixed time.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "cml/ipc/pipe.h"
#include "cml/proc/proc.h"

namespace {

namespace fs = std::filesystem;

struct Options {
    std::wstring broker;
    std::wstring fixture;
    std::string module_name;
    std::string mode;
    fs::path workdir;
    long ticks = 20;
    DWORD timeout_ms = 120000;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() {
        HANDLE h = handle_;
        handle_ = nullptr;
        return h;
    }
    void reset(HANDLE handle = nullptr) {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::wstring quoted(const std::wstring& text) {
    return L"\"" + text + L"\"";
}

bool parse_args(int argc, wchar_t** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--broker") {
            options->broker = next();
        } else if (arg == L"--fixture") {
            options->fixture = next();
        } else if (arg == L"--module-name") {
            options->module_name = cml::proc::utf8_from_wide(next());
        } else if (arg == L"--mode") {
            options->mode = cml::proc::utf8_from_wide(next());
        } else if (arg == L"--workdir") {
            options->workdir = next();
        } else if (arg == L"--ticks") {
            options->ticks = std::wcstol(next().c_str(), nullptr, 10);
        } else if (arg == L"--timeout-ms") {
            options->timeout_ms = static_cast<DWORD>(std::wcstoul(next().c_str(), nullptr, 10));
        } else {
            std::fprintf(stderr, "cml_it_agent: unknown argument '%s'\n", cml::proc::utf8_from_wide(arg).c_str());
            return false;
        }
    }
    // CMake hands over forward slashes; the broker log and CML_BROKER_PATH are cleaner with
    // native ones.
    std::replace(options->broker.begin(), options->broker.end(), L'/', L'\\');
    std::replace(options->fixture.begin(), options->fixture.end(), L'/', L'\\');
    options->workdir.make_preferred();
    const bool mode_ok = options->mode == "prestart" || options->mode == "autostart" || options->mode == "nobroker";
    return mode_ok && !options->broker.empty() && !options->fixture.empty() && !options->workdir.empty() &&
           !options->module_name.empty() && options->ticks > 0;
}

Handle make_kill_on_close_job() {
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        return Handle();
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return Handle();
    }
    return job;
}

struct Process {
    Handle process;
    DWORD pid = 0;
};

// Starts `exe args` suspended, puts it in `job` (so it and everything it starts dies with the
// driver), then lets it run. Standard output and error go to `output` when given.
bool launch(const std::wstring& exe, const std::wstring& args, HANDLE job, HANDLE input, HANDLE output,
            Process* out) {
    std::wstring command = quoted(exe) + L" " + args;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    if (output != nullptr) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = output;
        startup.hStdError = output;
    }
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    if (!CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, output != nullptr ? TRUE : FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) {
        std::fprintf(stderr, "cml_it_agent: cannot start %s (Win32 error %lu)\n",
                     cml::proc::utf8_from_wide(exe).c_str(), GetLastError());
        return false;
    }
    Handle thread(info.hThread);
    out->process.reset(info.hProcess);
    out->pid = info.dwProcessId;
    if (!AssignProcessToJobObject(job, info.hProcess)) {
        // Not fatal (e.g. an old OS refusing nested jobs), but cleanup is then best effort.
        std::fprintf(stderr, "cml_it_agent: warning: cannot put pid %lu in the job (Win32 error %lu)\n",
                     info.dwProcessId, GetLastError());
    }
    ResumeThread(thread.get());
    return true;
}

// Waits for the process to exit; false on timeout (the process is left running).
bool wait_exit(const Process& process, DWORD timeout_ms, DWORD* exit_code) {
    if (WaitForSingleObject(process.process.get(), timeout_ms) != WAIT_OBJECT_0) {
        return false;
    }
    GetExitCodeProcess(process.process.get(), exit_code);
    return true;
}

// The pid the broker logged on its first line: `cml_broker 0.1.0 pid=1234 pipe=...`.
DWORD broker_pid_from_log(const std::string& log) {
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t start = line.find("cml_broker ");
        const std::size_t pid = line.find(" pid=");
        if (start != std::string::npos && pid != std::string::npos) {
            return static_cast<DWORD>(std::strtoul(line.c_str() + pid + 5, nullptr, 10));
        }
    }
    return 0;
}

bool contains_line(const std::string& text, const std::vector<std::string>& needles) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string folded = lower(line);
        const bool all = std::all_of(needles.begin(), needles.end(),
                                     [&](const std::string& n) { return folded.find(lower(n)) != std::string::npos; });
        if (all) {
            return true;
        }
    }
    return false;
}

struct Report {
    int failures = 0;
    void check(bool condition, const std::string& what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++failures;
        }
    }
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parse_args(argc, argv, &options)) {
        std::fputs("usage: cml_it_agent --broker EXE --fixture EXE --module-name NAME --workdir DIR\n"
                   "                    --mode prestart|autostart|nobroker [--ticks N] [--timeout-ms N]\n",
                   stderr);
        return 2;
    }

    std::error_code ec;
    fs::create_directories(options.workdir, ec);
    const std::string unique = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    const std::string pipe_name = "\\\\.\\pipe\\cml_it_" + unique;
    const std::wstring pipe_wide = cml::proc::wide_from_utf8(pipe_name);
    const fs::path stem = options.workdir / (options.mode + "_" + cml::proc::utf8_from_wide(fs::path(options.fixture).stem().wstring()) + "_" + unique);
    const fs::path broker_log = fs::path(stem).concat(".broker.log");
    const fs::path agent_log = fs::path(stem).concat(".agent.log");
    const fs::path fixture_out = fs::path(stem).concat(".fixture.out");

    Handle job = make_kill_on_close_job();
    if (!job.valid()) {
        std::fprintf(stderr, "cml_it_agent: cannot create the job object (Win32 error %lu)\n", GetLastError());
        return 2;
    }

    // Everything started below inherits these.
    SetEnvironmentVariableW(L"CML_PIPE_NAME", pipe_wide.c_str());
    SetEnvironmentVariableW(L"CML_AGENT_LOG", agent_log.c_str());
    const std::wstring broker_options = L"--exit-when-idle 2000 --startup-timeout 60000 --log " + quoted(broker_log.wstring());
    if (options.mode == "nobroker") {
        SetEnvironmentVariableW(L"CML_BROKER_PATH", (options.workdir / "no_such_broker.exe").c_str());
    } else {
        SetEnvironmentVariableW(L"CML_BROKER_PATH", options.broker.c_str());
        SetEnvironmentVariableW(L"CML_BROKER_ARGS", options.mode == "autostart" ? broker_options.c_str() : nullptr);
    }

    Report report;
    Process broker;
    if (options.mode == "prestart") {
        const std::wstring args = L"--headless --pipe " + quoted(pipe_wide) + L" " + broker_options;
        if (!launch(options.broker, args, job.get(), nullptr, nullptr, &broker)) {
            return 2;
        }
        // Wait for the pipe to exist: an observable condition, bounded by the broker dying or the timeout.
        const ULONGLONG deadline = GetTickCount64() + options.timeout_ms;
        while (!cml::ipc::pipe_exists(pipe_name)) {
            if (WaitForSingleObject(broker.process.get(), 10) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) {
                std::fprintf(stderr, "cml_it_agent: the broker never created %s\n", pipe_name.c_str());
                std::fputs(read_file(broker_log).c_str(), stderr);
                return 1;
            }
        }
    }

    // The fixture, with stdout and stderr captured in a file (a file cannot fill up and block it).
    SECURITY_ATTRIBUTES inheritable;
    ZeroMemory(&inheritable, sizeof(inheritable));
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    Handle out_file(CreateFileW(fixture_out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out_file.valid() || !nul.valid()) {
        std::fprintf(stderr, "cml_it_agent: cannot open the fixture output file (Win32 error %lu)\n", GetLastError());
        return 2;
    }
    const std::wstring fixture_args = L"--agent --ticks " + std::to_wstring(options.ticks) +
                                      (options.mode == "nobroker" ? L" --agent-wait 500" : L"");
    Process fixture;
    if (!launch(options.fixture, fixture_args, job.get(), nul.get(), out_file.get(), &fixture)) {
        return 2;
    }
    out_file.reset();  // the child has its own inherited copy
    nul.reset();

    DWORD fixture_code = 0xFFFFFFFFu;
    const bool fixture_exited = wait_exit(fixture, options.timeout_ms, &fixture_code);
    report.check(fixture_exited, "the fixture exited within the timeout");
    if (!fixture_exited) {
        TerminateProcess(fixture.process.get(), 1);
    }
    report.check(fixture_code == 0, "the fixture exited with code 0 (was " + std::to_string(fixture_code) + ")");

    const std::string output = read_file(fixture_out);
    report.check(output.find("TICK n=0") != std::string::npos, "the fixture printed its first TICK line");
    report.check(output.find("TICK n=" + std::to_string(options.ticks - 1)) != std::string::npos,
                 "the fixture printed all its TICK lines");

    if (options.mode == "nobroker") {
        report.check(output.find("AGENT enabled=0") != std::string::npos,
                     "without a broker the enable request stays unanswered");
        report.check(output.find("AGENT enabled=1") == std::string::npos, "no broker, so nothing was enabled");
    } else {
        report.check(output.find("AGENT enabled=1") != std::string::npos, "the fixture printed 'AGENT enabled=1'");
        report.check(output.find("AGENT create failed") == std::string::npos, "the agent was created");
    }

    // The broker side. In prestart mode we own the process; in autostart mode the agent started
    // it, so find it through the pid it logged.
    DWORD broker_code = 0xFFFFFFFFu;
    if (options.mode != "nobroker") {
        Process autostarted;
        const Process* watched = &broker;
        if (options.mode == "autostart") {
            const DWORD pid = broker_pid_from_log(read_file(broker_log));
            report.check(pid != 0, "the auto-started broker logged its pid (the agent did not start one?)");
            if (pid != 0) {
                autostarted.pid = pid;
                autostarted.process.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
                report.check(autostarted.process.valid(),
                             "the auto-started broker (pid " + std::to_string(pid) + ") is still there to be waited for");
            }
            watched = &autostarted;
        }
        if (watched->process.valid()) {
            const bool broker_exited = wait_exit(*watched, options.timeout_ms, &broker_code);
            report.check(broker_exited, "the broker exited by itself after the agent left (--exit-when-idle)");
            if (broker_exited) {
                report.check(broker_code == 0, "the broker exited with code 0 (was " + std::to_string(broker_code) + ")");
            }
        } else if (options.mode == "prestart") {
            report.check(false, "the broker process handle is valid");
        }

        const std::string log = read_file(broker_log);
        report.check(contains_line(log, {"hello request=", "pid="}), "the broker log shows the agent's Hello");
        report.check(contains_line(log, {"modulelist request=", "modules="}), "the broker log shows the ModuleList");
        report.check(contains_line(log, {"enablemodule request=", "-> ok", options.module_name}),
                     "the broker log shows EnableModule -> OK for " + options.module_name);
        report.check(contains_line(log, {"disconnected"}), "the broker log shows the agent disconnecting");
    }

    const std::string agent_text = read_file(agent_log);
    if (options.mode == "autostart") {
        report.check(contains_line(agent_text, {"no broker is listening", "starting"}),
                     "the agent log shows that it started the broker");
    } else if (options.mode == "prestart") {
        report.check(!contains_line(agent_text, {"no broker is listening"}),
                     "the agent found the pre-started broker and did not start another");
    } else {
        report.check(contains_line(agent_text, {"no_such_broker.exe"}),
                     "the agent log shows that the broker could not be started");
    }

    if (report.failures != 0) {
        std::fprintf(stderr, "\n---- fixture output (%s) ----\n%s\n", fixture_out.string().c_str(), output.c_str());
        std::fprintf(stderr, "---- broker log (%s) ----\n%s\n", broker_log.string().c_str(), read_file(broker_log).c_str());
        std::fprintf(stderr, "---- agent log (%s) ----\n%s\n", agent_log.string().c_str(), agent_text.c_str());
        return 1;
    }
    std::printf("cml_it_agent %s: ok (pipe %s)\n", options.mode.c_str(), pipe_name.c_str());
    return 0;  // the job object closes here and kills anything still running
}
