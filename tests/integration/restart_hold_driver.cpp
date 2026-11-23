// Integration driver for `restart {hold_new}` (WP-34, ADR-0040): a headless cml_broker, the fixture `hot_restart` with its agent,
// and the control pipe, all real processes.
//
//   cml_it_restart_hold --broker <cml_broker.exe> --fixture <fixture_hot_restart.exe> --workdir <dir>
//                       --scenario resume|timeout|disconnect|not-held|broker-killed|two-holds [--timeout-ms N]
//
// resume      restart {hold_new}: `restarting` arrives, the new process (created suspended) prints nothing and is alive; after
//             `restart-resume` (sent on the same connection) it runs, registers, and `restarted` arrives.
// timeout     the same with nobody resuming: the broker resumes the process after hold_ms (warning CML5813), the restart completes,
//             and a late `restart-resume` is the clean diagnostic CML5814.
// disconnect  the client that asked for the restart disconnects while the process is held (hold_ms 30 s): the process runs within
//             seconds anyway (CML5816), registers, and `restarted` reaches another client.
// broker-killed  the broker itself is killed (TerminateProcess) while the process is held (hold_ms 60 s): the watchdog the broker started
//             (`cml_broker.exe --hold-watchdog`) resumes the process within seconds.
// two-holds  `wait` sent on the connection of a held restart is refused busy at once (CML5803: the connection thread must stay free to read
//             restart-resume); two held restarts back to back on one connection, the second resumed promptly on the same connection.
// not-held    `restart-resume` for a pid that is not held (CML5814) and without a pid (CML5818) are diagnostics; the broker is still
//             fine; a plain `restart` (no hold_new) behaves as before (no `restarting` event).
//
// Each scenario prints one greppable `WP34-HOLD <scenario>: key=value ...` line when every check passed. The fixture's stdout goes
// to a file, which the new process appends to (the broker hands the standard handles on): "no TICK before restart-resume" is a
// statement about that file. Everything started is put in a kill-on-close job object; every wait is bounded.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include "cml/ipc/control.h"
#include "cml/ipc/pipe.h"
#include "cml/proc/proc.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using cml::ipc::json::Value;

struct Options {
    std::wstring broker;
    std::wstring fixture;
    fs::path workdir;
    std::string scenario;
    DWORD timeout_ms = 180000;
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

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::wstring quoted(const std::wstring& text) {
    return L"\"" + text + L"\"";
}

long long ms_since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Failure {
    std::string what;
};

// Checks collect their failures; the scenario ends at the first one that cannot be recovered from (Failure).
struct Report {
    int failures = 0;
    void check(bool condition, const std::string& what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++failures;
        }
    }
    void require(bool condition, const std::string& what) {
        if (!condition) {
            throw Failure{what};
        }
    }
};

bool parse_args(int argc, wchar_t** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--broker") {
            options->broker = next();
        } else if (arg == L"--fixture") {
            options->fixture = next();
        } else if (arg == L"--workdir") {
            options->workdir = next();
        } else if (arg == L"--scenario") {
            options->scenario = cml::proc::utf8_from_wide(next());
        } else if (arg == L"--timeout-ms") {
            options->timeout_ms = static_cast<DWORD>(std::wcstoul(next().c_str(), nullptr, 10));
        } else {
            std::fprintf(stderr, "cml_it_restart_hold: unknown argument '%s'\n", cml::proc::utf8_from_wide(arg).c_str());
            return false;
        }
    }
    std::replace(options->broker.begin(), options->broker.end(), L'/', L'\\');
    std::replace(options->fixture.begin(), options->fixture.end(), L'/', L'\\');
    options->workdir.make_preferred();
    const bool known = options->scenario == "resume" || options->scenario == "timeout" || options->scenario == "disconnect" ||
                       options->scenario == "not-held" || options->scenario == "broker-killed" ||
                       options->scenario == "two-holds";
    return known && !options->broker.empty() && !options->fixture.empty() && !options->workdir.empty();
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

// Starts `exe args` suspended, puts it in `job`, lets it run. Output goes to `output` when given.
bool launch(const std::wstring& exe, const std::wstring& args, HANDLE job, HANDLE input, HANDLE output, Process* out) {
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
    if (!CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, output != nullptr ? TRUE : FALSE, CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &info)) {
        std::fprintf(stderr, "cml_it_restart_hold: cannot start %s (Win32 error %lu)\n", cml::proc::utf8_from_wide(exe).c_str(), GetLastError());
        return false;
    }
    Handle thread(info.hThread);
    out->process.reset(info.hProcess);
    out->pid = info.dwProcessId;
    if (!AssignProcessToJobObject(job, info.hProcess)) {
        std::fprintf(stderr, "cml_it_restart_hold: warning: cannot put pid %lu in the job (Win32 error %lu)\n", info.dwProcessId, GetLastError());
    }
    ResumeThread(thread.get());
    return true;
}

// ---- the control connection -------------------------------------------------------------------------------------------------

// A raw control connection: requests are sent without waiting, answers and events are collected as they arrive, so a test can send
// `restart-resume` on the same connection while `restart` is still unanswered, and can drop the connection at any moment.
class Raw {
public:
    static std::optional<Raw> connect(const std::string& pipe, DWORD timeout_ms) {
        cml::Result<cml::ipc::PipeConnection> connection = cml::ipc::connect_pipe(pipe, timeout_ms);
        if (!connection) {
            std::fprintf(stderr, "cml_it_restart_hold: cannot connect to %s: %s\n", pipe.c_str(), connection.error().format().c_str());
            return std::nullopt;
        }
        Raw raw;
        raw.channel_ = cml::ipc::ControlChannel(std::move(*connection));
        return raw;
    }

    std::int64_t send(const std::string& cmd, Value args = Value::object()) {
        cml::ipc::ControlRequest request;
        request.id = next_id_++;
        request.cmd = cmd;
        request.args = std::move(args);
        cml::Result<std::string> line = cml::ipc::encode_request(request);
        if (line) {
            (void)channel_.send_line(*line, 5000);
        }
        return request.id;
    }

    // Reads at most one line (waiting up to timeout_ms) and files it. False on timeout or when the connection ended.
    bool pump(int timeout_ms) {
        cml::Result<cml::ipc::ControlChannel::Line> line = channel_.receive_line(static_cast<std::uint32_t>(timeout_ms));
        if (!line || line->status != cml::ipc::IoStatus::Ok) {
            closed_ = closed_ || (line && (line->status == cml::ipc::IoStatus::Closed || line->status == cml::ipc::IoStatus::Cancelled)) || !line;
            return false;
        }
        cml::Result<std::variant<cml::ipc::ControlResponse, cml::ipc::ControlEvent>> message = cml::ipc::decode_server_message(line->text);
        if (!message) {
            return true;
        }
        if (const auto* response = std::get_if<cml::ipc::ControlResponse>(&*message)) {
            if (response->id) {
                responses_[*response->id] = *response;
            }
        } else {
            events_.push_back(std::get<cml::ipc::ControlEvent>(*message));
        }
        return true;
    }

    std::optional<cml::ipc::ControlResponse> wait_response(std::int64_t id, int timeout_ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (const auto it = responses_.find(id); it != responses_.end()) {
                cml::ipc::ControlResponse out = it->second;
                responses_.erase(it);
                return out;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            if (left <= 0 || closed_) {
                return std::nullopt;
            }
            pump(static_cast<int>(std::min<long long>(left, 200)));
        }
    }

    // The first collected event for which `matches` is true (removed from the queue), waiting up to timeout_ms.
    std::optional<cml::ipc::ControlEvent> wait_event(const std::function<bool(const cml::ipc::ControlEvent&)>& matches, int timeout_ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            for (auto it = events_.begin(); it != events_.end(); ++it) {
                if (matches(*it)) {
                    cml::ipc::ControlEvent out = *it;
                    events_.erase(it);
                    return out;
                }
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            if (left <= 0 || closed_) {
                return std::nullopt;
            }
            pump(static_cast<int>(std::min<long long>(left, 200)));
        }
    }
    std::optional<cml::ipc::ControlEvent> wait_event(const std::string& name, int timeout_ms) {
        return wait_event([&](const cml::ipc::ControlEvent& e) { return e.name == name; }, timeout_ms);
    }
    // A `diag` event with this id among those collected so far (or arriving within timeout_ms).
    bool wait_diag(const std::string& id, int timeout_ms) {
        return wait_event([&](const cml::ipc::ControlEvent& e) { return e.name == "diag" && e.body.get_string("id") == id; }, timeout_ms).has_value();
    }

    std::optional<cml::ipc::ControlResponse> call(const std::string& cmd, Value args, int timeout_ms) {
        return wait_response(send(cmd, std::move(args)), timeout_ms);
    }

    void close() { channel_.close(); }

private:
    cml::ipc::ControlChannel channel_;
    std::int64_t next_id_ = 1;
    std::map<std::int64_t, cml::ipc::ControlResponse> responses_;
    std::deque<cml::ipc::ControlEvent> events_;
    bool closed_ = false;
};

Value object_of(std::initializer_list<std::pair<const char*, Value>> members) {
    Value value = Value::object();
    for (const auto& member : members) {
        value.set(member.first, member.second);
    }
    return value;
}

Value subscribe_args(std::initializer_list<const char*> events) {
    Value list = Value::array();
    for (const char* e : events) {
        list.push(Value::string(e));
    }
    return object_of({{"events", list}});
}

std::string diag_id_of(const cml::ipc::ControlResponse& response) {
    return response.diag ? response.diag->id : std::string();
}

// ---- the fixture's output ---------------------------------------------------------------------------------------------------

// "TICK n=3 value=1 pid=123 restarted=1 ..." lines of a relaunched process (the first process prints restarted=0).
int count_new_process_lines(const std::string& text) {
    int count = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if ((line.rfind("TICK ", 0) == 0 || line.rfind("START ", 0) == 0) && line.find("restarted=1") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

int count_new_ticks(const std::string& text) {
    int count = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("TICK ", 0) == 0 && line.find("restarted=1") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// ---- the rig -----------------------------------------------------------------------------------------------------------------

struct Rig {
    Handle job;
    Process broker;
    Process fixture;
    fs::path out;
    fs::path broker_log;
    std::string agent_pipe;
    std::string control_pipe;
    std::uint32_t old_pid = 0;
};

// Broker, fixture, and the wait until the fixture's module is enabled and its index is ready (the state a restart normally runs in).
void start_rig(const Options& options, Rig* rig, Report* report) {
    std::error_code ec;
    fs::create_directories(options.workdir, ec);
    const std::string unique = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    rig->agent_pipe = "\\\\.\\pipe\\cml_it34_" + unique;
    rig->control_pipe = "\\\\.\\pipe\\cml_it34c_" + unique;
    const fs::path stem = options.workdir / ("restart_hold_" + options.scenario + "_" + unique);
    rig->broker_log = fs::path(stem).concat(".broker.log");
    rig->out = fs::path(stem).concat(".fixture.out");
    rig->job = make_kill_on_close_job();
    report->require(rig->job.valid(), "cannot create the job object");

    const std::wstring agent_pipe_wide = cml::proc::wide_from_utf8(rig->agent_pipe);
    // The fixture (and the process the broker starts again) inherit these.
    SetEnvironmentVariableW(L"CML_PIPE_NAME", agent_pipe_wide.c_str());
    SetEnvironmentVariableW(L"CML_AGENT_LOG", fs::path(stem).concat(".agent.log").c_str());
    SetEnvironmentVariableW(L"CML_BROKER_PATH", options.broker.c_str());

    const std::wstring broker_args = L"--headless --pipe " + quoted(agent_pipe_wide) + L" --control-pipe " +
                                     quoted(cml::proc::wide_from_utf8(rig->control_pipe)) + L" --startup-timeout 120000 --verbose --log " +
                                     quoted(rig->broker_log.wstring());
    report->require(launch(options.broker, broker_args, rig->job.get(), nullptr, nullptr, &rig->broker), "cannot start the broker");
    const auto pipe_deadline = Clock::now() + std::chrono::seconds(60);
    while (!cml::ipc::pipe_exists(rig->control_pipe)) {
        report->require(WaitForSingleObject(rig->broker.process.get(), 10) != WAIT_OBJECT_0 && Clock::now() < pipe_deadline,
                        "the broker never created the control pipe " + rig->control_pipe);
    }

    SECURITY_ATTRIBUTES inheritable;
    ZeroMemory(&inheritable, sizeof(inheritable));
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    Handle out_file(CreateFileW(rig->out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    report->require(out_file.valid() && nul.valid(), "cannot open the fixture output file");
    report->require(launch(options.fixture, L"--agent --label=hold", rig->job.get(), nul.get(), out_file.get(), &rig->fixture), "cannot start the fixture");
    out_file.reset();
    nul.reset();
    rig->old_pid = rig->fixture.pid;

    // The agent registered, the module is enabled and its index is ready.
    std::optional<Raw> status = Raw::connect(rig->control_pipe, 10000);
    report->require(status.has_value(), "cannot connect to the control pipe");
    const auto ready_deadline = Clock::now() + std::chrono::seconds(90);
    bool ready = false;
    while (!ready && Clock::now() < ready_deadline) {
        std::optional<cml::ipc::ControlResponse> reply = status->call("status", Value::object(), 10000);
        if (reply && reply->ok) {
            cml::Result<cml::ipc::StatusReport> parsed = cml::ipc::status_from_json(reply->result);
            if (parsed) {
                if (const cml::ipc::StatusProcess* process = parsed->find_process(rig->old_pid)) {
                    for (const cml::ipc::StatusModule& module : process->modules) {
                        ready = ready || (module.enabled && (module.index_state.empty() || module.index_state == "ready"));
                    }
                }
                report->require(parsed->has_capability(cml::ipc::capability::kRestartHold), "the broker does not advertise the capability restart-hold");
            }
        }
        if (!ready) {
            sleep_ms(200);
        }
    }
    report->require(ready, "the fixture's module was not enabled with a ready index within 90 s\n" + read_file(rig->out));
    const auto tick_deadline = Clock::now() + std::chrono::seconds(30);
    while (read_file(rig->out).find("TICK n=") == std::string::npos) {
        report->require(Clock::now() < tick_deadline, "the fixture printed no TICK line");
        sleep_ms(50);
    }
}

bool process_alive(std::uint32_t pid) {
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return process.valid() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
}

// Waits until the fixture's output has a line of the relaunched process, returns the milliseconds from `from`.
std::optional<long long> wait_new_process_output(const Rig& rig, Clock::time_point from, int timeout_ms, int min_ticks = 1) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < end) {
        const std::string text = read_file(rig.out);
        if (count_new_ticks(text) >= min_ticks) {
            return ms_since(from);
        }
        sleep_ms(25);
    }
    return std::nullopt;
}

std::uint32_t pid_of(const cml::ipc::ControlEvent& e, const char* key) {
    return static_cast<std::uint32_t>(e.body.get_integer(key));
}

// ---- the scenarios -----------------------------------------------------------------------------------------------------------

Value restart_args(std::uint32_t pid, std::uint32_t hold_ms) {
    return object_of({{"pid", Value::integer(pid)},
                      {"hold_new", Value::boolean(true)},
                      {"hold_ms", Value::integer(hold_ms)},
                      {"timeout_ms", Value::integer(60000)},
                      {"registration_timeout_ms", Value::integer(60000)}});
}

void scenario_resume(Rig& rig, Report& report) {
    std::optional<Raw> client = Raw::connect(rig.control_pipe, 10000);
    report.require(client.has_value(), "cannot connect");
    std::optional<cml::ipc::ControlResponse> subscribed = client->call("subscribe", subscribe_args({"restarting", "restarted", "diag"}), 10000);
    report.require(subscribed && subscribed->ok, "subscribe failed");
    report.check(count_new_process_lines(read_file(rig.out)) == 0, "the fixture already printed lines of a restarted process");

    const std::int64_t restart_id = client->send("restart", restart_args(rig.old_pid, 30000));
    std::optional<cml::ipc::ControlEvent> restarting = client->wait_event("restarting", 90000);
    report.require(restarting.has_value(), "no `restarting` event\n" + read_file(rig.broker_log));
    const Clock::time_point held_since = Clock::now();
    const std::uint32_t new_pid = pid_of(*restarting, "new_pid");
    report.check(pid_of(*restarting, "old_pid") == rig.old_pid, "restarting.old_pid is not the old process");
    report.check(new_pid != 0 && new_pid != rig.old_pid, "restarting.new_pid is not a new process");

    // Held: the process exists, but not a line of it reached the output, and the restart has not answered.
    sleep_ms(2000);
    const std::string held_out = read_file(rig.out);
    const int lines_while_held = count_new_process_lines(held_out);
    report.check(process_alive(new_pid), "the held process is not alive");
    report.check(lines_while_held == 0, "the new process printed " + std::to_string(lines_while_held) + " line(s) before restart-resume:\n" + held_out);
    report.check(!client->wait_event("restarted", 200).has_value(), "`restarted` arrived before restart-resume");

    // Resume on the same connection while `restart` is unanswered.
    const Clock::time_point resume_sent = Clock::now();
    std::optional<cml::ipc::ControlResponse> resumed = client->call("restart-resume", object_of({{"pid", Value::integer(new_pid)}}), 20000);
    report.require(resumed.has_value() && resumed->ok, "restart-resume failed: " + (resumed ? diag_id_of(*resumed) : std::string("no answer")));
    report.check(resumed->result.get_bool("resumed"), "restart-resume did not report resumed");
    std::optional<cml::ipc::ControlResponse> restarted_answer = client->wait_response(restart_id, 120000);
    report.require(restarted_answer.has_value() && restarted_answer->ok,
                   "restart failed: " + (restarted_answer ? diag_id_of(*restarted_answer) : std::string("no answer")) + "\n" + read_file(rig.broker_log));
    report.check(static_cast<std::uint32_t>(restarted_answer->result.get_integer("new_pid")) == new_pid, "the restart result names another new pid");
    std::optional<cml::ipc::ControlEvent> restarted = client->wait_event("restarted", 10000);
    report.check(restarted.has_value() && pid_of(*restarted, "new_pid") == new_pid, "no `restarted` event with the new pid");

    const std::optional<long long> first_tick = wait_new_process_output(rig, resume_sent, 30000, 3);
    report.check(first_tick.has_value(), "the new process printed no TICK lines after restart-resume");
    const int ticks_after = count_new_ticks(read_file(rig.out));
    std::printf("WP34-HOLD resume: old_pid=%u new_pid=%u held_ms=%lld new_process_lines_while_held=%d alive_while_held=1 resume_ok=1 same_connection=1 "
                "restarted_event=%d new_ticks_after_resume=%d first_tick_after_resume_ms=%lld\n",
                rig.old_pid, new_pid, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(resume_sent - held_since).count()),
                lines_while_held, restarted.has_value() ? 1 : 0, ticks_after, first_tick.value_or(-1));
}

void scenario_timeout(Rig& rig, Report& report) {
    constexpr int kHoldMs = 3000;
    std::optional<Raw> client = Raw::connect(rig.control_pipe, 10000);
    report.require(client.has_value(), "cannot connect");
    report.require(client->call("subscribe", subscribe_args({"restarting", "restarted", "diag"}), 10000).has_value(), "subscribe failed");

    const std::int64_t restart_id = client->send("restart", restart_args(rig.old_pid, kHoldMs));
    std::optional<cml::ipc::ControlEvent> restarting = client->wait_event("restarting", 90000);
    report.require(restarting.has_value(), "no `restarting` event\n" + read_file(rig.broker_log));
    const Clock::time_point held_since = Clock::now();
    const std::uint32_t new_pid = pid_of(*restarting, "new_pid");

    sleep_ms(kHoldMs * 6 / 10);  // well inside hold_ms: nobody resumes, nothing runs
    const int lines_before = count_new_process_lines(read_file(rig.out));
    report.check(lines_before == 0, "the new process printed " + std::to_string(lines_before) + " line(s) before hold_ms passed");
    report.check(process_alive(new_pid), "the held process is not alive");

    const std::optional<long long> first_tick = wait_new_process_output(rig, held_since, 60000);
    report.require(first_tick.has_value(), "nobody resumed the process, and the broker did not either\n" + read_file(rig.broker_log));
    report.check(*first_tick >= kHoldMs - 500, "the process ran after " + std::to_string(*first_tick) + " ms, before hold_ms (" + std::to_string(kHoldMs) + ")");
    report.check(*first_tick <= kHoldMs + 20000, "the process ran only after " + std::to_string(*first_tick) + " ms");

    std::optional<cml::ipc::ControlResponse> answer = client->wait_response(restart_id, 120000);
    report.require(answer.has_value() && answer->ok, "restart failed: " + (answer ? diag_id_of(*answer) : std::string("no answer")));
    report.check(static_cast<std::uint32_t>(answer->result.get_integer("new_pid")) == new_pid, "the restart result names another new pid");
    const bool expired_diag = client->wait_diag("CML5813", 5000);
    report.check(expired_diag, "no warning CML5813 (hold expired)");
    const bool restarted = client->wait_event("restarted", 5000).has_value();
    report.check(restarted, "no `restarted` event");

    // Too late: the clean diagnostic.
    std::optional<cml::ipc::ControlResponse> late = client->call("restart-resume", object_of({{"pid", Value::integer(new_pid)}}), 10000);
    report.require(late.has_value(), "no answer to the late restart-resume");
    report.check(!late->ok && diag_id_of(*late) == "CML5814", "the late restart-resume is not CML5814 (" + diag_id_of(*late) + ")");
    std::printf("WP34-HOLD timeout: hold_ms=%d new_process_lines_before_expiry=%d first_tick_after_hold_start_ms=%lld resumed_by_broker=1 diag_expired=%s "
                "restarted_event=%d late_resume_diag=%s new_pid=%u\n",
                kHoldMs, lines_before, *first_tick, expired_diag ? "CML5813" : "none", restarted ? 1 : 0, diag_id_of(*late).c_str(), new_pid);
}

void scenario_disconnect(Rig& rig, Report& report) {
    constexpr int kHoldMs = 30000;
    std::optional<Raw> watcher = Raw::connect(rig.control_pipe, 10000);
    report.require(watcher.has_value(), "cannot connect (watcher)");
    report.require(watcher->call("subscribe", subscribe_args({"restarted", "diag"}), 10000).has_value(), "subscribe failed");

    std::optional<Raw> client = Raw::connect(rig.control_pipe, 10000);
    report.require(client.has_value(), "cannot connect (client)");
    report.require(client->call("subscribe", subscribe_args({"restarting"}), 10000).has_value(), "subscribe failed");
    (void)client->send("restart", restart_args(rig.old_pid, kHoldMs));
    std::optional<cml::ipc::ControlEvent> restarting = client->wait_event("restarting", 90000);
    report.require(restarting.has_value(), "no `restarting` event\n" + read_file(rig.broker_log));
    const std::uint32_t new_pid = pid_of(*restarting, "new_pid");

    sleep_ms(1500);
    const int lines_before = count_new_process_lines(read_file(rig.out));
    report.check(lines_before == 0, "the new process printed " + std::to_string(lines_before) + " line(s) while held");
    report.check(process_alive(new_pid), "the held process is not alive");

    client->close();  // the client that asked for the restart goes away while the process is held
    client.reset();
    const Clock::time_point disconnected = Clock::now();
    const std::optional<long long> first_tick = wait_new_process_output(rig, disconnected, 20000);
    report.check(first_tick.has_value(), "the held process was stranded: no output within 20 s of the disconnect (hold_ms was 30 s)\n" + read_file(rig.broker_log));
    const bool restarted = watcher->wait_event("restarted", 120000).has_value();
    report.check(restarted, "no `restarted` event on the other client");
    const bool gone_diag = watcher->wait_diag("CML5816", 5000);
    report.check(gone_diag, "no warning CML5816 (client disconnected while holding)");

    // The restart was carried to its end: the new process is registered with its module enabled.
    bool registered = false;
    const auto end = Clock::now() + std::chrono::seconds(60);
    while (!registered && Clock::now() < end) {
        std::optional<cml::ipc::ControlResponse> reply = watcher->call("status", Value::object(), 10000);
        if (reply && reply->ok) {
            cml::Result<cml::ipc::StatusReport> parsed = cml::ipc::status_from_json(reply->result);
            const cml::ipc::StatusProcess* process = parsed ? parsed->find_process(new_pid) : nullptr;
            registered = process != nullptr && std::any_of(process->modules.begin(), process->modules.end(), [](const cml::ipc::StatusModule& m) { return m.enabled; });
        }
        if (!registered) {
            sleep_ms(200);
        }
    }
    report.check(registered, "the restarted process did not register with its module enabled");
    std::printf("WP34-HOLD disconnect: hold_ms=%d new_process_lines_while_held=%d first_tick_after_disconnect_ms=%lld stranded=%d diag=%s restarted_event=%d "
                "registered=%d new_pid=%u\n",
                kHoldMs, lines_before, first_tick.value_or(-1), first_tick ? 0 : 1, gone_diag ? "CML5816" : "none", restarted ? 1 : 0, registered ? 1 : 0, new_pid);
}

void scenario_broker_killed(Rig& rig, Report& report) {
    constexpr int kHoldMs = 60000;
    std::optional<Raw> client = Raw::connect(rig.control_pipe, 10000);
    report.require(client.has_value(), "cannot connect");
    report.require(client->call("subscribe", subscribe_args({"restarting"}), 10000).has_value(), "subscribe failed");
    (void)client->send("restart", restart_args(rig.old_pid, kHoldMs));
    std::optional<cml::ipc::ControlEvent> restarting = client->wait_event("restarting", 90000);
    report.require(restarting.has_value(), "no `restarting` event\n" + read_file(rig.broker_log));
    const std::uint32_t new_pid = pid_of(*restarting, "new_pid");
    sleep_ms(1500);
    const int lines_before = count_new_process_lines(read_file(rig.out));
    report.check(lines_before == 0, "the new process printed " + std::to_string(lines_before) + " line(s) while held");
    report.check(process_alive(new_pid), "the held process is not alive");

    TerminateProcess(rig.broker.process.get(), 1);  // the broker dies outright: no shutdown path, nothing runs in it
    WaitForSingleObject(rig.broker.process.get(), 10000);
    const Clock::time_point killed = Clock::now();
    const std::optional<long long> first_tick = wait_new_process_output(rig, killed, 20000);
    report.check(first_tick.has_value(), "the held process stayed suspended after the broker was killed (hold_ms was 60 s)");
    report.check(process_alive(new_pid), "the process is not alive after the broker was killed");
    std::printf("WP34-HOLD broker_killed: hold_ms=%d new_process_lines_while_held=%d broker_killed=1 first_tick_after_kill_ms=%lld stranded=%d new_pid=%u\n",
                kHoldMs, lines_before, first_tick.value_or(-1), first_tick ? 0 : 1, new_pid);
}

void scenario_two_holds(Rig& rig, Report& report) {
    std::optional<Raw> client = Raw::connect(rig.control_pipe, 10000);
    report.require(client.has_value(), "cannot connect");
    report.require(client->call("subscribe", subscribe_args({"restarting", "restarted"}), 10000).has_value(), "subscribe failed");

    // ---- first held restart; a blocking `wait` during the hold is refused at once ----
    const std::int64_t first_id = client->send("restart", restart_args(rig.old_pid, 30000));
    std::optional<cml::ipc::ControlEvent> first = client->wait_event("restarting", 90000);
    report.require(first.has_value(), "no `restarting` event (first)\n" + read_file(rig.broker_log));
    const std::uint32_t first_pid = pid_of(*first, "new_pid");
    const Clock::time_point wait_sent = Clock::now();
    std::optional<cml::ipc::ControlResponse> busy = client->call("wait", Value::object(), 5000);
    const long long busy_ms = ms_since(wait_sent);
    report.require(busy.has_value(), "`wait` during a held restart got no answer (the connection thread is blocked)");
    report.check(!busy->ok && diag_id_of(*busy) == "CML5803", "`wait` during a held restart is not CML5803 (" + diag_id_of(*busy) + ")");
    report.check(busy_ms < 3000, "`wait` took " + std::to_string(busy_ms) + " ms to be refused");
    std::optional<cml::ipc::ControlResponse> resumed = client->call("restart-resume", object_of({{"pid", Value::integer(first_pid)}}), 20000);
    report.require(resumed.has_value() && resumed->ok, "the first restart-resume failed");
    std::optional<cml::ipc::ControlResponse> first_done = client->wait_response(first_id, 120000);
    report.require(first_done.has_value() && first_done->ok, "the first held restart failed: " + (first_done ? diag_id_of(*first_done) : std::string("no answer")));

    // ---- the second one, right behind it on the same connection ----
    const std::int64_t second_id = client->send("restart", restart_args(first_pid, 30000));
    std::optional<cml::ipc::ControlEvent> second = client->wait_event(
        [&](const cml::ipc::ControlEvent& e) { return e.name == "restarting" && pid_of(e, "old_pid") == first_pid; }, 90000);
    report.require(second.has_value(), "no `restarting` event for the second held restart (it ran inline on the connection thread?)\n" + read_file(rig.broker_log));
    const std::uint32_t second_pid = pid_of(*second, "new_pid");
    const Clock::time_point second_held = Clock::now();
    std::optional<cml::ipc::ControlResponse> resumed2 = client->call("restart-resume", object_of({{"pid", Value::integer(second_pid)}}), 20000);
    const long long resume_ms = ms_since(second_held);
    report.require(resumed2.has_value() && resumed2->ok, "the second restart-resume failed (" + (resumed2 ? diag_id_of(*resumed2) : std::string("no answer")) + ")");
    report.check(resume_ms < 5000, "the second held process was resumed only after " + std::to_string(resume_ms) + " ms");
    std::optional<cml::ipc::ControlResponse> second_done = client->wait_response(second_id, 120000);
    report.require(second_done.has_value() && second_done->ok, "the second held restart failed: " + (second_done ? diag_id_of(*second_done) : std::string("no answer")));
    const std::optional<long long> first_tick = wait_new_process_output(rig, second_held, 30000);
    report.check(first_tick.has_value(), "the second new process printed nothing");
    std::printf("WP34-HOLD two_holds: wait_refused=%s wait_refused_ms=%lld first_pid=%u second_pid=%u second_resume_ms=%lld second_resumed_same_connection=1 new_process_ticking=%d\n",
                diag_id_of(*busy).c_str(), busy_ms, first_pid, second_pid, resume_ms, first_tick ? 1 : 0);
}

void scenario_not_held(Rig& rig, Report& report) {
    std::optional<Raw> client = Raw::connect(rig.control_pipe, 10000);
    report.require(client.has_value(), "cannot connect");
    report.require(client->call("subscribe", subscribe_args({"restarting", "restarted"}), 10000).has_value(), "subscribe failed");

    std::optional<cml::ipc::ControlResponse> unknown = client->call("restart-resume", object_of({{"pid", Value::integer(999999)}}), 10000);
    report.require(unknown.has_value(), "no answer to restart-resume for an unknown pid");
    report.check(!unknown->ok && diag_id_of(*unknown) == "CML5814", "restart-resume for a pid that is not held is not CML5814 (" + diag_id_of(*unknown) + ")");
    std::optional<cml::ipc::ControlResponse> bare = client->call("restart-resume", Value::object(), 10000);
    report.require(bare.has_value(), "no answer to restart-resume without pid");
    report.check(!bare->ok && diag_id_of(*bare) == "CML5818", "restart-resume without a pid is not CML5818 (" + diag_id_of(*bare) + ")");
    std::optional<cml::ipc::ControlResponse> bad_args = client->call("restart", object_of({{"pid", Value::integer(rig.old_pid)}, {"hold_new", Value::boolean(true)}, {"hold_ms", Value::integer(60001)}}), 10000);
    report.require(bad_args.has_value(), "no answer to restart with a bad hold_ms");
    report.check(!bad_args->ok && diag_id_of(*bad_args) == "CML5818", "restart with hold_ms 60001 is not CML5818 (" + diag_id_of(*bad_args) + ")");
    report.check(process_alive(rig.old_pid), "the old process was touched by a refused restart");

    // Without hold_new the restart is today's: no `restarting` event, the process runs at once.
    std::optional<cml::ipc::ControlResponse> plain =
        client->call("restart", object_of({{"pid", Value::integer(rig.old_pid)}, {"timeout_ms", Value::integer(60000)}, {"registration_timeout_ms", Value::integer(60000)}}), 150000);
    report.require(plain.has_value() && plain->ok, "a plain restart failed: " + (plain ? diag_id_of(*plain) : std::string("no answer")) + "\n" + read_file(rig.broker_log));
    const bool restarted = client->wait_event("restarted", 10000).has_value();
    const bool no_restarting = !client->wait_event("restarting", 300).has_value();
    report.check(restarted, "a plain restart sent no `restarted` event");
    report.check(no_restarting, "a plain restart sent a `restarting` event");
    const std::optional<long long> first_tick = wait_new_process_output(rig, Clock::now(), 30000);
    report.check(first_tick.has_value(), "the plainly restarted process printed nothing");
    std::printf("WP34-HOLD not-held: unknown_pid_diag=%s missing_pid_diag=%s bad_hold_ms_diag=%s plain_restart_ok=1 plain_restarting_event=%d plain_restarted_event=%d\n",
                diag_id_of(*unknown).c_str(), diag_id_of(*bare).c_str(), diag_id_of(*bad_args).c_str(), no_restarting ? 0 : 1, restarted ? 1 : 0);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parse_args(argc, argv, &options)) {
        std::fputs("usage: cml_it_restart_hold --broker EXE --fixture EXE --workdir DIR --scenario resume|timeout|disconnect|not-held|broker-killed|two-holds [--timeout-ms N]\n", stderr);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Report report;
    Rig rig;
    // A watchdog: whatever hangs, the test ends (the job object then kills the broker and the fixtures).
    std::thread watchdog([&options] {
        Sleep(options.timeout_ms);
        std::fprintf(stderr, "cml_it_restart_hold: the scenario did not finish within %lu ms\n", options.timeout_ms);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 3);
    });
    watchdog.detach();
    try {
        start_rig(options, &rig, &report);
        if (options.scenario == "resume") {
            scenario_resume(rig, report);
        } else if (options.scenario == "timeout") {
            scenario_timeout(rig, report);
        } else if (options.scenario == "disconnect") {
            scenario_disconnect(rig, report);
        } else if (options.scenario == "two-holds") {
            scenario_two_holds(rig, report);
        } else if (options.scenario == "broker-killed") {
            scenario_broker_killed(rig, report);
        } else {
            scenario_not_held(rig, report);
        }
    } catch (const Failure& failure) {
        std::fprintf(stderr, "FAIL: %s\n", failure.what.c_str());
        ++report.failures;
    }
    if (report.failures != 0) {
        std::fprintf(stderr, "\n---- fixture output (%s) ----\n%s\n", rig.out.string().c_str(), read_file(rig.out).c_str());
        std::fprintf(stderr, "---- broker log (%s) ----\n%s\n", rig.broker_log.string().c_str(), read_file(rig.broker_log).c_str());
        std::printf("WP34-HOLD %s: FAILED (%d check(s))\n", options.scenario.c_str(), report.failures);
        return 1;
    }
    return 0;  // the job object closes here and kills the broker and the fixtures
}
