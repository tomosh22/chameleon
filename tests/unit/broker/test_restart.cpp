// Hot-restart (WP-21): the whole `restart` sequence of the broker against scripted agents and a fake process host, on
// every host. Real here: ReloadService, RestartCoordinator, Session, AgentLink, EventBus, the control line protocol.
// Faked: the two agent processes (the old one answers Restart, the new one registers by itself and enables modules when
// asked) and the operating system (open / terminate / start a process). The same sequence runs against real processes in
// the `hot_restart` scenario of cmltest.
#include "cml/core/restart.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>
#include <set>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/control_server.h"
#include "cml/core/event_bus.h"
#include "cml/core/reload_service.h"
#include "cml/core/restart_diag_ids.h"
#include "cml/core/session.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using ipc::json::Value;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

class Collect final : public EventListener {
public:
    void on_event(const Event& e) override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }
    std::vector<Event> named(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Event> out;
        for (const Event& e : events) {
            if (e.name == name) {
                out.push_back(e);
            }
        }
        return out;
    }
    std::vector<std::string> diag_ids(const std::string& severity) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const Event& e : events) {
            if (e.name == "diag" && e.body.get_string("severity") == severity) {
                out.push_back(e.body.get_string("id"));
            }
        }
        return out;
    }
    std::mutex mutex;
    std::vector<Event> events;
};

const std::string kExe = "C:\\app\\game.exe";
const std::string kDll = "C:\\app\\plugin.dll";
const std::string kOther = "C:\\app\\other.dll";

std::string narrow(const std::u16string& text) {
    std::string out;
    for (const char16_t c : text) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// "A=b\0C=d\0\0" from its entries.
std::u16string block_of(const std::vector<std::string>& entries) {
    std::u16string out;
    for (const std::string& e : entries) {
        for (const char c : e) {
            out.push_back(static_cast<char16_t>(c));
        }
        out.push_back(u'\0');
    }
    out.push_back(u'\0');
    return out;
}

// The "A=b" entries of an environment block, in order.
std::vector<std::string> entries_of(const std::u16string& block) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < block.size() && block[pos] != u'\0') {
        const std::size_t end = block.find(u'\0', pos);
        out.push_back(narrow(block.substr(pos, end - pos)));
        pos = end + 1;
    }
    return out;
}

// One scripted agent process: a Session and an AgentLink wired like a connection of the ConnectionServer.
class SimAgent {
public:
    SimAgent(ReloadService& service, std::uint64_t id) : service_(service), id_(id), session_(id, "0.1.0") {
        link_ = std::make_shared<AgentLink>(id, [this](const ipc::Message& m) { return from_broker(m); });
        session_.set_answer_sink([this](const ipc::Message& m) { return link_->deliver(m); });
        service_.attach(link_);
    }

    // What the agent sends to the broker (the order of connection_server.cpp: session, publish, service).
    void to_broker(const ipc::Message& m) {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)session_.on_message(m);
        link_->publish(session_);
        service_.on_session_changed(id_);
    }
    void register_process(std::uint32_t pid, const std::string& token, const std::vector<std::string>& loaded, const std::vector<std::string>& enable) {
        ipc::Hello hello;
        hello.request_id = next_request();
        hello.pid = pid;
        hello.restart_token = token;
        hello.agent_path = "C:\\app\\cml_agent.dll";
        hello.command_line = "game.exe";
        to_broker(hello);
        ipc::ModuleList list;
        list.request_id = next_request();
        std::uint64_t base = 0x140000000ull;
        for (const std::string& path : loaded) {
            list.modules.push_back(ipc::ModuleEntry{base, 0x10000, path});
            base += 0x100000;
        }
        to_broker(list);
        for (const std::string& path : enable) {
            ipc::EnableModule m;
            m.request_id = next_request();
            m.path = path;
            to_broker(m);
        }
    }
    std::uint64_t next_request() { return ++requests_made_; }

    std::function<void(const ipc::Message&)> on_request;  // the scripted behaviour (runs on the broker's thread, inside send)
    std::shared_ptr<AgentLink> link() const { return link_; }

private:
    Result<void> from_broker(const ipc::Message& m) {
        if (on_request) {
            on_request(m);
        }
        return Result<void>();
    }

    ReloadService& service_;
    std::uint64_t id_;
    Session session_;
    std::shared_ptr<AgentLink> link_;
    std::mutex mutex_;
    std::atomic<std::uint64_t> requests_made_{1000};
};

struct Rig;

class FakeProcess final : public RestartableProcess {
public:
    explicit FakeProcess(Rig& rig) : rig_(rig) {}
    ~FakeProcess() override;
    Result<void> terminate(std::uint32_t timeout_ms) override;
    Result<std::uint32_t> relaunch(const RelaunchSpec& spec) override;
    std::optional<std::uint32_t> relaunched_exit_code() override;
    Result<void> resume() override;  // WP-34

private:
    Rig& rig_;
    bool suspended_ = false;  // created suspended and not resumed yet: the new process has not "run" (it does not register)
    RelaunchSpec spec_;
};

class FakeHost final : public ProcessHost {
public:
    explicit FakeHost(Rig& rig) : rig_(rig) {}
    Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t pid, const std::uint64_t std_handles[3], std::uint32_t info_flags,
                                                     std::vector<Diag>* warnings) override;
    bool supports_hold() const override;

private:
    Rig& rig_;
};

struct Rig {
    // knobs
    bool answer_prepare = true;
    std::uint32_t prepare_status = 0;
    bool fail_open = false;
    bool fail_terminate = false;
    bool fail_launch = false;
    bool new_registers = true;
    bool new_exits = false;
    bool app_enables_dll = false;   // the new process enables plugin.dll by itself
    bool app_enables_exe = true;    // ... and game.exe
    int register_delay_ms = 40;
    std::uint32_t hooks_run = 3;
    int prepare_delay_ms = 0;        // the old agent takes this long to answer Restart
    bool empty_environment = false;  // the old agent captured no environment
    std::uint32_t info_flags = 0;
    std::uint32_t opened_flags = 0;
    std::function<void()> during_prepare;   // runs on the broker's thread while the old agent "prepares"
    std::vector<std::uint64_t> cancels;     // RestartCancel messages the old agent received
    std::uint32_t spoof_pid = 0;            // an unrelated process that registers with the restart token first (0: none)
    // WP-34 (restart-hold)
    bool hold_supported = false;            // the fake host can create the new process suspended
    bool fail_resume = false;               // resume() fails (the real host terminates the process then)
    bool exits_while_held = false;          // the held process is ended by somebody (a debugger, the user)
    std::atomic<int> resumes{0};            // resume() calls that did something
    std::atomic<int> destroyed_suspended{0};  // a process object destroyed while still suspended: the coordinator did not resume it itself
    std::atomic<bool> agent_started{false};   // the new process "ran" (registered an agent)
    std::vector<ipc::RestartModule> reported_modules{{1, kExe}, {1, kDll}};

    // what happened
    std::mutex mutex;
    std::vector<std::string> log;
    RelaunchSpec relaunched;
    std::vector<ipc::RestartModule> reenabled;   // modules the broker asked the new process to enable
    int reenable_requests = 0;
    std::string new_token;

    EventBus bus;
    std::shared_ptr<Collect> events = std::make_shared<Collect>();
    std::shared_ptr<SimAgent> old_agent;
    std::shared_ptr<SimAgent> new_agent;
    std::shared_ptr<SimAgent> spoof_agent;
    std::vector<std::thread> threads;
    ReloadService service;

    Rig() : service(PipelineEnv{}, &bus) {
        bus.subscribe(events);
        service.set_process_host(std::make_shared<FakeHost>(*this));
        old_agent = std::make_shared<SimAgent>(service, 1);
        old_agent->on_request = [this](const ipc::Message& m) { old_handles(m); };
        old_agent->register_process(100, "", {kExe, kDll, kOther}, {kExe, kDll});
    }
    ~Rig() {
        for (std::thread& t : threads) {
            t.join();
        }
    }

    void note(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        log.push_back(line);
    }
    std::vector<std::string> logged() {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }

    void old_handles(const ipc::Message& m) {
        if (const auto* cancel = std::get_if<ipc::RestartCancel>(&m)) {
            std::lock_guard<std::mutex> lock(mutex);
            cancels.push_back(cancel->request_id);
            return;
        }
        const auto* restart = std::get_if<ipc::Restart>(&m);
        if (restart == nullptr || !answer_prepare) {
            return;
        }
        if (during_prepare) {
            during_prepare();
        }

        ipc::RestartInfo info;
        info.request_id = restart->request_id;
        info.status = prepare_status;
        if (prepare_status != 0) {
            info.message = "no launch information";
        } else {
            info.hooks_run = hooks_run;
            info.image_path = u"C:\\app\\game.exe";
            info.command_line = u"\"C:\\app\\game.exe\" --level \"a b\"";
            info.working_directory = u"C:\\app";
            info.flags = info_flags;
            info.environment = empty_environment ? std::u16string() : block_of({"ALPHA=1", "CML_RESTART_TOKEN=stale", "ZED=2"});
            info.std_input = 4;
            info.std_output = 8;
            info.std_error = 8;
            info.modules = reported_modules;
        }
        if (prepare_delay_ms > 0) {
            // A slow application: the answer comes later, from another thread, like a SyncPoint() that is reached late.
            threads.emplace_back([this, info, delay = prepare_delay_ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                old_agent->to_broker(info);
            });
            return;
        }
        old_agent->to_broker(info);
    }

    void new_handles(const ipc::Message& m) {
        const auto* ask = std::get_if<ipc::ReenableModules>(&m);
        if (ask == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++reenable_requests;
            for (const ipc::RestartModule& module : ask->modules) {
                reenabled.push_back(module);
            }
        }
        // Like the real agent: each module becomes an EnableModule request of the application, then Reply.
        for (const ipc::RestartModule& module : ask->modules) {
            ipc::EnableModule enable;
            enable.request_id = new_agent->next_request();
            enable.options = module.options;
            enable.path = module.path;
            new_agent->to_broker(enable);
        }
        ipc::Reply reply;
        reply.request_id = ask->request_id;
        reply.message = "queued";
        new_agent->to_broker(reply);
    }

    // The relaunched process: registers after a moment, with the token from its environment.
    void start_new_process(const RelaunchSpec& spec, std::uint32_t pid) {
        std::string token;
        for (const std::string& entry : entries_of(spec.environment)) {
            if (entry.rfind("CML_RESTART_TOKEN=", 0) == 0) {
                token = entry.substr(std::string("CML_RESTART_TOKEN=").size());
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            new_token = token;
        }
        if (!new_registers || new_exits) {
            return;
        }
        if (spoof_pid != 0) {
            // A child of some other process that inherited the token: it registers first and must be ignored.
            spoof_agent = std::make_shared<SimAgent>(service, 9);
            spoof_agent->register_process(spoof_pid, token, {kExe}, {kExe});
        }
        threads.emplace_back([this, pid, token] {
            std::this_thread::sleep_for(std::chrono::milliseconds(register_delay_ms));
            new_agent = std::make_shared<SimAgent>(service, 2);
            new_agent->on_request = [this](const ipc::Message& m) { new_handles(m); };
            std::vector<std::string> enable;
            if (app_enables_exe) {
                enable.push_back(kExe);
            }
            if (app_enables_dll) {
                enable.push_back(kDll);
            }
            new_agent->register_process(pid, token, {kExe, kDll, kOther}, enable);
        });
    }
};

Result<void> FakeProcess::terminate(std::uint32_t) {
    rig_.note("terminate");
    if (rig_.fail_terminate) {
        return Diag::make(restart::diag::kTerminateFailed, "access denied", "x");
    }
    return Result<void>();
}

Result<std::uint32_t> FakeProcess::relaunch(const RelaunchSpec& spec) {
    rig_.note(spec.suspended ? "relaunch suspended" : "relaunch");
    if (rig_.fail_launch) {
        return Diag::make(restart::diag::kLaunchFailed, "CreateProcessW failed", "x");
    }
    {
        std::lock_guard<std::mutex> lock(rig_.mutex);
        rig_.relaunched = spec;
    }
    if (spec.suspended) {
        suspended_ = true;  // nothing runs: the process starts (and registers) when it is resumed
        spec_ = spec;
        return 200u;
    }
    rig_.start_new_process(spec, 200);
    return 200u;
}

std::optional<std::uint32_t> FakeProcess::relaunched_exit_code() {
    if (suspended_ && rig_.exits_while_held) {
        return 3u;
    }
    return rig_.new_exits ? std::optional<std::uint32_t>(3) : std::nullopt;
}

Result<void> FakeProcess::resume() {
    if (!suspended_) {
        return Result<void>();
    }
    suspended_ = false;
    rig_.note("resume");
    if (rig_.fail_resume) {
        return Diag::make(restart::diag::kResumeFailed, "ResumeThread failed (Win32 error 5); the process was terminated instead of being left suspended", "x");
    }
    ++rig_.resumes;
    rig_.agent_started = true;
    rig_.start_new_process(spec_, 200);
    return Result<void>();
}

FakeProcess::~FakeProcess() {
    if (suspended_) {
        ++rig_.destroyed_suspended;  // the real host resumes here as a last resort; the tests assert it never has to
        (void)resume();
    }
}

bool FakeHost::supports_hold() const {
    return rig_.hold_supported;
}

Result<std::unique_ptr<RestartableProcess>> FakeHost::open(std::uint32_t pid, const std::uint64_t std_handles[3], std::uint32_t info_flags,
                                                           std::vector<Diag>*) {
    rig_.opened_flags = info_flags;
    rig_.note("open pid=" + std::to_string(pid) + " handles=" + std::to_string(std_handles[0]) + "," + std::to_string(std_handles[1]) + "," +
              std::to_string(std_handles[2]));
    if (rig_.fail_open) {
        return Diag::make(restart::diag::kOpenFailed, "OpenProcess failed", "x");
    }
    return std::unique_ptr<RestartableProcess>(new FakeProcess(rig_));
}

Value args_for(std::uint32_t pid, std::uint32_t timeout_ms, std::uint32_t registration_ms = 5000) {
    Value args = Value::object();
    if (pid != 0) {
        args.set("pid", Value::integer(pid));
    }
    args.set("timeout_ms", Value::integer(timeout_ms));
    args.set("registration_timeout_ms", Value::integer(registration_ms));
    return args;
}

}  // namespace

TEST_CASE("restart: the old process prepares, is terminated, the new one registers, the missing module is enabled again") {
    QuietLogs quiet;
    Rig rig;
    Result<Value> result = rig.service.restart(args_for(100, 10000));
    REQUIRE(result.has_value());
    CHECK(result->get_integer("old_pid") == 100);
    CHECK(result->get_integer("new_pid") == 200);
    CHECK(result->get_integer("hooks_run") == 3);
    REQUIRE(result->find("modules") != nullptr);
    CHECK(result->find("modules")->items().size() == 2);
    CHECK(result->find("not_enabled")->items().empty());
    CHECK(result->find("reapply_error") == nullptr);

    // Order: the standard handles are taken while the old process still lives, then it dies, then the new one starts.
    CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8", "terminate", "relaunch"});

    // The new process starts with the recorded parameters and the environment plus the token, in sorted position, the stale
    // token replaced.
    CHECK(narrow(rig.relaunched.image_path) == "C:\\app\\game.exe");
    CHECK(narrow(rig.relaunched.command_line) == "\"C:\\app\\game.exe\" --level \"a b\"");
    CHECK(narrow(rig.relaunched.working_directory) == "C:\\app");
    const std::vector<std::string> env = entries_of(rig.relaunched.environment);
    REQUIRE(env.size() == 3);
    CHECK(env[0] == "ALPHA=1");
    CHECK(env[1].rfind("CML_RESTART_TOKEN=", 0) == 0);
    CHECK(env[1].size() >= std::string("CML_RESTART_TOKEN=").size() + 16);
    CHECK(env[1] != "CML_RESTART_TOKEN=stale");
    CHECK(env[2] == "ZED=2");
    CHECK(rig.relaunched.environment.size() >= 2);
    CHECK(rig.relaunched.environment.substr(rig.relaunched.environment.size() - 2) == std::u16string(2, u'\0'));
    CHECK(rig.new_token == env[1].substr(std::string("CML_RESTART_TOKEN=").size()));

    // game.exe was enabled by the application itself; only plugin.dll needed the broker's request.
    CHECK(rig.reenable_requests == 1);
    REQUIRE(rig.reenabled.size() == 1);
    CHECK(rig.reenabled[0].path == kDll);
    CHECK(rig.reenabled[0].options == 1);

    // The event, and nothing that looks like a failure.
    const std::vector<Event> restarted = rig.events->named("restarted");
    REQUIRE(restarted.size() == 1);
    CHECK(restarted[0].body.get_integer("old_pid") == 100);
    CHECK(restarted[0].body.get_integer("new_pid") == 200);
    CHECK(rig.events->diag_ids("error").empty());
}

TEST_CASE("restart: when the application enables all its modules by itself the broker asks for nothing") {
    QuietLogs quiet;
    Rig rig;
    rig.app_enables_dll = true;
    Result<Value> result = rig.service.restart(args_for(100, 10000));
    REQUIRE(result.has_value());
    CHECK(rig.reenable_requests == 0);
    CHECK(result->find("modules")->items().size() == 2);
}

TEST_CASE("restart: a module the new process cannot enable is reported, not fatal") {
    QuietLogs quiet;
    Rig rig;
    rig.reported_modules.push_back(ipc::RestartModule{0, "C:\\app\\not_loaded_yet.dll"});  // not in the new process's module list
    Result<Value> result = rig.service.restart(args_for(100, 2500));
    REQUIRE(result.has_value());
    REQUIRE(result->find("not_enabled")->items().size() == 1);
    CHECK(result->find("not_enabled")->items()[0].as_string() == "C:\\app\\not_loaded_yet.dll");
    const std::vector<std::string> warnings = rig.events->diag_ids("warning");
    CHECK(std::find(warnings.begin(), warnings.end(), "CML5809") != warnings.end());
}

TEST_CASE("restart: an agent that refuses leaves the process untouched") {
    QuietLogs quiet;
    Rig rig;
    rig.prepare_status = 4450;
    Result<Value> result = rig.service.restart(args_for(100, 5000));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kPrepareFailed);
    CHECK(result.error().message.find("CML4450") != std::string::npos);
    CHECK(result.error().message.find("not touched") != std::string::npos);
    CHECK(rig.logged().empty());  // nothing opened, nothing terminated
    CHECK(rig.events->named("restarted").empty());
    CHECK(rig.events->diag_ids("error") == std::vector<std::string>{"CML5804"});
}

TEST_CASE("restart: an agent that does not answer in time leaves the process untouched") {
    QuietLogs quiet;
    Rig rig;
    rig.answer_prepare = false;  // e.g. synchronised mode and the application never reaches SyncPoint()
    const auto start = std::chrono::steady_clock::now();
    Result<Value> result = rig.service.restart(args_for(100, 300));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kPrepareFailed);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(rig.logged().empty());
    // The agent is told to drop the request, so its pre-restart hooks do not run later for a restart nobody carries out.
    REQUIRE(rig.cancels.size() == 1);
    CHECK(rig.cancels[0] != 0);
}

TEST_CASE("restart: failures to open, terminate and start are told apart") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.fail_open = true;
        Result<Value> result = rig.service.restart(args_for(100, 5000));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kOpenFailed);
        CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8"});  // not terminated
    }
    {
        Rig rig;
        rig.fail_terminate = true;
        Result<Value> result = rig.service.restart(args_for(100, 5000));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kTerminateFailed);
        CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8", "terminate"});  // not relaunched
    }
    {
        Rig rig;
        rig.fail_launch = true;
        Result<Value> result = rig.service.restart(args_for(100, 5000));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kLaunchFailed);
        CHECK(result.error().message.find("was terminated") != std::string::npos);
    }
}

TEST_CASE("restart: a new process that never registers is reported and left running") {
    QuietLogs quiet;
    Rig rig;
    rig.new_registers = false;
    Result<Value> result = rig.service.restart(args_for(100, 30000, 400));  // the registration budget, not the preparation one
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kNoRegistration);
    CHECK(result.error().message.find("pid 200") != std::string::npos);
    CHECK(result.error().message.find("left running") != std::string::npos);
    CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8", "terminate", "relaunch"});  // the new one is not touched
    CHECK(rig.events->named("restarted").empty());
}

TEST_CASE("restart: a new process that exits before it registers is reported at once") {
    QuietLogs quiet;
    Rig rig;
    rig.new_exits = true;
    const auto start = std::chrono::steady_clock::now();
    Result<Value> result = rig.service.restart(args_for(100, 30000));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kNewProcessExited);
    CHECK(result.error().message.find("code 3") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST_CASE("restart: which process, and when it cannot be done") {
    QuietLogs quiet;
    {
        Rig rig;
        Result<Value> unknown = rig.service.restart(args_for(999, 1000));
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == restart::diag::kNoProcess);
        Value bad = Value::object();
        bad.set("pid", Value::string("x"));
        Result<Value> wrong = rig.service.restart(bad);
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == restart::diag::kBadRequest);
    }
    {
        // Two processes and no pid: ambiguous.
        Rig rig;
        SimAgent second(rig.service, 5);
        second.register_process(555, "", {kExe}, {kExe});
        Result<Value> ambiguous = rig.service.restart(args_for(0, 1000));
        REQUIRE_FALSE(ambiguous.has_value());
        CHECK(ambiguous.error().code == restart::diag::kBadRequest);
    }
    {
        // Exactly one process: the pid may be left out.
        Rig rig;
        Result<Value> only = rig.service.restart(args_for(0, 10000));
        REQUIRE(only.has_value());
        CHECK(only->get_integer("old_pid") == 100);
    }
    {
        // A host that cannot restart processes (no process host): refused, and the capability is not advertised.
        EventBus bus;
        ReloadService service{PipelineEnv{}, &bus};
        service.set_process_host(nullptr);
        Result<Value> none = service.restart(Value::object());
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().code == restart::diag::kUnavailable);
        ControlContext context;
        context.service = &service;
        const std::vector<std::string> caps = broker_capabilities(context);
        CHECK(std::find(caps.begin(), caps.end(), "restart") == caps.end());
    }
}

TEST_CASE("restart: the control command, the capability and the opt-in event") {
    QuietLogs quiet;
    Rig rig;
    ControlContext context;
    context.version = "0.1.0";
    context.service = &rig.service;
    const std::vector<std::string> caps = broker_capabilities(context);
    CHECK(std::find(caps.begin(), caps.end(), "restart") != caps.end());

    std::set<std::string> subscribed;
    context.subscribe = [&](const std::set<std::string>& names) { subscribed = names; };
    Result<ipc::json::Value> reply = ipc::json::parse(handle_control_line(R"({"id":1,"cmd":"subscribe"})", context));
    REQUIRE(reply.has_value());
    CHECK(subscribed.count("restarted") == 0);  // opt-in, like `progress`
    reply = ipc::json::parse(handle_control_line(R"({"id":2,"cmd":"subscribe","args":{"events":["restarted","diag"]}})", context));
    REQUIRE(reply.has_value());
    CHECK(reply->get_bool("ok"));
    CHECK(subscribed == std::set<std::string>{"restarted", "diag"});

    reply = ipc::json::parse(handle_control_line(R"({"id":3,"cmd":"restart","args":{"pid":100,"timeout_ms":10000}})", context));
    REQUIRE(reply.has_value());
    REQUIRE(reply->get_bool("ok"));
    const Value* result = reply->find("result");
    REQUIRE(result != nullptr);
    CHECK(result->get_integer("old_pid") == 100);
    CHECK(result->get_integer("new_pid") == 200);
}

TEST_CASE("environment_with_token: replaces any old token, keeps the block sorted and terminated") {
    const std::u16string block = block_of({"=C:=C:\\dir", "ALPHA=1", "CML_RESTART_TOKEN=old", "cml_restart_token=old2", "ZED=2"});
    const std::vector<std::string> out = entries_of(environment_with_token(block, "abc123"));
    CHECK(out == std::vector<std::string>{"=C:=C:\\dir", "ALPHA=1", "CML_RESTART_TOKEN=abc123", "ZED=2"});

    // No token yet, last position, empty block.
    CHECK(entries_of(environment_with_token(block_of({"ALPHA=1"}), "t")) == std::vector<std::string>{"ALPHA=1", "CML_RESTART_TOKEN=t"});
    CHECK(entries_of(environment_with_token(block_of({}), "t")) == std::vector<std::string>{"CML_RESTART_TOKEN=t"});
    CHECK(entries_of(environment_with_token(std::u16string(), "t")) == std::vector<std::string>{"CML_RESTART_TOKEN=t"});
    // A name that merely starts like the token is kept.
    CHECK(entries_of(environment_with_token(block_of({"CML_RESTART_TOKEN2=x"}), "t")) ==
          std::vector<std::string>{"CML_RESTART_TOKEN=t", "CML_RESTART_TOKEN2=x"});
    const std::u16string result = environment_with_token(block_of({"A=1"}), "t");
    CHECK(result.substr(result.size() - 2) == std::u16string(2, u'\0'));
}

TEST_CASE("restart: reloads are refused busy while a restart runs, and the baselines are taken after the preparation") {
    QuietLogs quiet;
    Rig rig;
    std::vector<std::string> trace;
    std::mutex trace_mutex;
    rig.service.set_restart_trace_for_testing([&](const std::string& step) {
        std::lock_guard<std::mutex> lock(trace_mutex);
        trace.push_back(step);
    });
    std::string refused;
    rig.during_prepare = [&] {
        // A reload (a save-triggered one, or `reload`) arrives while the old agent is still preparing.
        Result<Value> reload = rig.service.reload(Value::object());
        refused = reload.has_value() ? std::string("accepted") : reload.error().id();
        trace_mutex.lock();
        trace.push_back("reload-during-prepare");
        trace_mutex.unlock();
    };
    Result<Value> result = rig.service.restart(args_for(100, 10000));
    REQUIRE(result.has_value());
    CHECK(refused == "CML5304");  // the existing busy diagnostic
    const std::vector<std::string> expected{"reload-during-prepare", "prepared", "snapshot", "terminate"};
    CHECK(trace == expected);  // prepare first, then the snapshot right before the old process is terminated
    // The lock is released afterwards: a reload is no longer refused as busy (it fails for other reasons: no PDBs here).
    Result<Value> after = rig.service.reload(Value::object());
    if (!after.has_value()) {
        CHECK(after.error().id() != "CML5304");
    }
}

TEST_CASE("restart: a restart cannot start while a reload runs") {
    QuietLogs quiet;
    Rig rig;
    std::unique_ptr<ExclusiveRun> running = rig.service.lock_reloads();  // what a running reload holds
    REQUIRE(running != nullptr);
    Result<Value> result = rig.service.restart(args_for(100, 1000));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kBusy);
    CHECK(rig.logged().empty());
}

TEST_CASE("restart: a slow preparation does not eat the time the new process needs to register") {
    QuietLogs quiet;
    Rig rig;
    rig.prepare_delay_ms = 700;       // almost all of timeout_ms ...
    rig.register_delay_ms = 400;      // ... and the new process needs longer than what is left of it
    Result<Value> result = rig.service.restart(args_for(100, 900, 5000));
    REQUIRE_MESSAGE(result.has_value(), result.error().format());
    CHECK(result->get_integer("new_pid") == 200);
}

TEST_CASE("restart: the preparation is bounded by timeout_ms and the registration by registration_timeout_ms") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.prepare_delay_ms = 600;
        Result<Value> result = rig.service.restart(args_for(100, 200, 5000));  // preparation too slow: nothing was touched
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kPrepareFailed);
        CHECK(rig.logged().empty());
    }
    {
        Rig rig;
        rig.new_registers = false;
        const auto start = std::chrono::steady_clock::now();
        Result<Value> result = rig.service.restart(args_for(100, 60000, 300));  // registration budget 300 ms although timeout_ms is a minute
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kNoRegistration);
        CHECK(result.error().message.find("300 ms") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    }
}

TEST_CASE("restart: once the hooks ran, a later failure says so") {
    QuietLogs quiet;
    Rig rig;
    rig.fail_open = true;
    Result<Value> result = rig.service.restart(args_for(100, 5000));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kOpenFailed);
    CHECK(result.error().message.find("3 pre-restart hook(s) ran; the process was not restarted") != std::string::npos);
}

TEST_CASE("restart: an agent that captured no environment is refused before anything is touched") {
    QuietLogs quiet;
    Rig rig;
    rig.empty_environment = true;
    Result<Value> result = rig.service.restart(args_for(100, 5000));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kPrepareFailed);
    CHECK(result.error().message.find("environment") != std::string::npos);
    CHECK(rig.logged().empty());
}

TEST_CASE("restart: only the process the broker started is taken for the restarted one") {
    QuietLogs quiet;
    Rig rig;
    rig.spoof_pid = 999;  // a child that inherited CML_RESTART_TOKEN registers first
    Result<Value> result = rig.service.restart(args_for(100, 10000));
    REQUIRE(result.has_value());
    CHECK(result->get_integer("new_pid") == 200);
}

TEST_CASE("restart: the console flag the agent reported reaches the process host") {
    QuietLogs quiet;
    Rig rig;
    rig.info_flags = ipc::kRestartInfoHadConsole;
    Result<Value> result = rig.service.restart(args_for(100, 10000));
    REQUIRE(result.has_value());
    CHECK(rig.opened_flags == ipc::kRestartInfoHadConsole);
}

TEST_CASE("restart: a process host replaced during a restart does not pull the coordinator from under it") {
    QuietLogs quiet;
    Rig rig;
    rig.during_prepare = [&] { rig.service.set_process_host(std::make_shared<FakeHost>(rig)); };  // replaces the coordinator mid-call
    Result<Value> result = rig.service.restart(args_for(100, 10000));
    REQUIRE(result.has_value());  // the running restart kept its own coordinator
    CHECK(result->get_integer("new_pid") == 200);
}

// ---- WP-34: restart {hold_new} -------------------------------------------------------------------------------------------------
// The new process is created suspended; `restarting` announces it; `restart-resume` (or hold_ms, a vanished client, a shutdown, an
// error) lets it run. The scripted host does not start the new process (no registration) until resume(), like a suspended process.

namespace {

Value hold_args(std::uint32_t hold_ms, std::uint32_t registration_ms = 5000) {
    Value args = args_for(100, 10000, registration_ms);
    args.set("hold_new", Value::boolean(true));
    args.set("hold_ms", Value::integer(hold_ms));
    return args;
}

Value pid_args(std::int64_t pid) {
    Value args = Value::object();
    args.set("pid", Value::integer(pid));
    return args;
}

bool wait_until(const std::function<bool()>& condition, int timeout_ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// restart(...) on its own thread, as the control server runs it.
struct HeldRestart {
    HeldRestart(Rig& rig, Value args, std::function<bool()> gone = {}) {
        thread = std::thread([this, &rig, args = std::move(args), gone = std::move(gone)] { result.emplace(rig.service.restart(args, gone)); });
    }
    ~HeldRestart() {
        if (thread.joinable()) {
            thread.join();
        }
    }
    Result<Value>& join() {
        thread.join();
        return *result;
    }
    std::thread thread;
    std::optional<Result<Value>> result;
};

bool is_held(Rig& rig) {
    return !rig.events->named("restarting").empty();
}

bool has_warning(Rig& rig, const std::string& id) {
    const std::vector<std::string> ids = rig.events->diag_ids("warning");
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool has_error(Rig& rig, const std::string& id) {
    const std::vector<std::string> ids = rig.events->diag_ids("error");
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST_CASE("restart hold: the new process does not run until restart-resume; the time held does not count against registration") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    Value first_args = hold_args(30000, 300);  // registration budget 300 ms, held for much longer than that
    first_args.set("token", Value::string("tok-A"));
    HeldRestart restart(rig, first_args);
    REQUIRE(wait_until([&] { return is_held(rig); }));
    const Event restarting = rig.events->named("restarting")[0];
    CHECK(restarting.body.get_integer("old_pid") == 100);
    CHECK(restarting.body.get_integer("new_pid") == 200);
    CHECK(restarting.body.get_string("token") == "tok-A");  // echoed, so a client can tell its own held restart from another's

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK_FALSE(rig.agent_started.load());  // nothing ran while held
    CHECK(rig.events->named("restarted").empty());
    CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8", "terminate", "relaunch suspended"});
    CHECK(rig.service.restart_active());
    // Reloads stay held off for the whole restart, the held time included.
    Result<Value> reload = rig.service.reload(Value::object());
    REQUIRE_FALSE(reload.has_value());
    CHECK(reload.error().id() == "CML5304");

    Result<Value> resumed = rig.service.restart_resume(pid_args(200));
    REQUIRE_MESSAGE(resumed.has_value(), resumed.error().format());
    CHECK(resumed->get_bool("resumed"));
    CHECK(resumed->get_integer("pid") == 200);
    CHECK(resumed->get_integer("held_ms") >= 600);

    Result<Value>& result = restart.join();
    REQUIRE_MESSAGE(result.has_value(), result.error().format());  // registered within 300 ms of the resume
    CHECK(result->get_integer("new_pid") == 200);
    CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8", "terminate", "relaunch suspended", "resume"});
    CHECK(rig.resumes.load() == 1);
    CHECK(rig.destroyed_suspended.load() == 0);
    CHECK(rig.events->named("restarted").size() == 1);
    for (const std::string& id : rig.events->diag_ids("error")) {
        CHECK(id == "CML5304");  // only the reload that was refused while the process was held
    }
    CHECK_FALSE(rig.service.restart_active());
    CHECK(rig.relaunched.suspended);

    // A second restart-resume for the same pid is a diagnostic, not a crash.
    Result<Value> again = rig.service.restart_resume(pid_args(200));
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == restart::diag::kNotHeld);
    CHECK(again.error().message.find("resumed") != std::string::npos);
}

TEST_CASE("restart hold: nobody resumes, so the broker does after hold_ms") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    const auto start = std::chrono::steady_clock::now();
    Result<Value> result = rig.service.restart(hold_args(250));
    const auto took = std::chrono::steady_clock::now() - start;
    REQUIRE_MESSAGE(result.has_value(), result.error().format());
    CHECK(took >= std::chrono::milliseconds(240));
    CHECK(rig.resumes.load() == 1);
    CHECK(rig.destroyed_suspended.load() == 0);
    CHECK(has_warning(rig, "CML5813"));
    CHECK(rig.events->named("restarting").size() == 1);
    CHECK(rig.events->named("restarted").size() == 1);
    CHECK(rig.events->named("restarting")[0].body.find("token") == nullptr);  // none sent, none echoed
    // The client that comes too late gets the clean diagnostic.
    Result<Value> late = rig.service.restart_resume(pid_args(200));
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().code == restart::diag::kNotHeld);
    CHECK(late.error().message.find("hold_ms expired") != std::string::npos);
}

TEST_CASE("restart hold: a client that disconnects while its process is held does not strand it") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    std::atomic<bool> gone{false};
    HeldRestart restart(rig, hold_args(30000), [&] { return gone.load(); });
    REQUIRE(wait_until([&] { return is_held(rig); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(rig.resumes.load() == 0);
    gone = true;
    Result<Value>& result = restart.join();
    REQUIRE_MESSAGE(result.has_value(), result.error().format());  // the restart goes on to the end
    CHECK(rig.resumes.load() == 1);
    CHECK(rig.destroyed_suspended.load() == 0);
    CHECK(has_warning(rig, "CML5816"));
    CHECK(rig.events->named("restarted").size() == 1);
}

TEST_CASE("restart hold: a broker that shuts down resumes the held process first") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    HeldRestart restart(rig, hold_args(30000));
    REQUIRE(wait_until([&] { return is_held(rig); }));
    rig.service.cancel();  // what the broker does when it stops
    Result<Value>& result = restart.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kHoldAborted);
    CHECK(rig.resumes.load() == 1);
    CHECK(rig.destroyed_suspended.load() == 0);
    CHECK(rig.events->named("restarted").empty());
    CHECK(has_error(rig, "CML5815"));
}

TEST_CASE("restart hold: a process that cannot be resumed is reported to both the restart and restart-resume") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    rig.fail_resume = true;
    HeldRestart restart(rig, hold_args(30000));
    REQUIRE(wait_until([&] { return is_held(rig); }));
    Result<Value> resumed = rig.service.restart_resume(pid_args(200));
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().code == restart::diag::kResumeFailed);
    Result<Value>& result = restart.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kResumeFailed);
    CHECK(rig.destroyed_suspended.load() == 0);
}

TEST_CASE("restart hold: a held process that is ended by somebody else is reported at once") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    rig.exits_while_held = true;
    Result<Value> result = rig.service.restart(hold_args(30000));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kNewProcessExited);
    CHECK(result.error().message.find("held") != std::string::npos);
    CHECK(rig.destroyed_suspended.load() == 0);
}

TEST_CASE("restart hold: a process that does not register after the resume is left running and reported") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    rig.new_registers = false;
    HeldRestart restart(rig, hold_args(30000, 300));
    REQUIRE(wait_until([&] { return is_held(rig); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // longer than the registration budget: it must not have expired
    CHECK(rig.service.restart_resume(pid_args(200)).has_value());
    Result<Value>& result = restart.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == restart::diag::kNoRegistration);
    CHECK(rig.destroyed_suspended.load() == 0);
}

TEST_CASE("restart hold: errors before the process is created leave nothing held") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.hold_supported = true;
        rig.fail_launch = true;
        Result<Value> result = rig.service.restart(hold_args(30000));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kLaunchFailed);
        CHECK(rig.events->named("restarting").empty());
        Result<Value> resume = rig.service.restart_resume(pid_args(200));
        REQUIRE_FALSE(resume.has_value());
        CHECK(resume.error().code == restart::diag::kNotHeld);
    }
    {
        Rig rig;
        rig.hold_supported = true;
        rig.prepare_status = 1;  // the agent refuses: the process is not touched
        Result<Value> result = rig.service.restart(hold_args(30000));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == restart::diag::kPrepareFailed);
        CHECK(rig.logged().empty());
    }
}

TEST_CASE("restart hold: without hold_new nothing changes, and without host support the request is refused untouched") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.hold_supported = true;
        Result<Value> plain = rig.service.restart(args_for(100, 10000));  // no hold_new
        REQUIRE(plain.has_value());
        CHECK(rig.logged() == std::vector<std::string>{"open pid=100 handles=4,8,8", "terminate", "relaunch"});
        CHECK_FALSE(rig.relaunched.suspended);
        CHECK(rig.events->named("restarting").empty());
        Value off = args_for(100, 10000);
        off.set("hold_new", Value::boolean(false));
        Result<Value> explicit_off = rig.service.restart(off);
        REQUIRE(explicit_off.has_value());
        CHECK(rig.events->named("restarting").empty());
    }
    {
        Rig rig;  // the host cannot hold
        ControlContext context;
        context.service = &rig.service;
        const std::vector<std::string> caps = broker_capabilities(context);
        CHECK(std::find(caps.begin(), caps.end(), "restart-hold") == caps.end());
        Result<Value> refused = rig.service.restart(hold_args(1000));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == restart::diag::kHoldUnavailable);
        CHECK(rig.logged().empty());  // nothing was opened, terminated or started
        Result<ipc::json::Value> reply = ipc::json::parse(handle_control_line(R"({"id":1,"cmd":"restart-resume","args":{"pid":200}})", context));
        REQUIRE(reply.has_value());
        CHECK_FALSE(reply->get_bool("ok"));
    }
}

TEST_CASE("restart hold: bad arguments and restart-resume for a pid that is not held are diagnostics") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    Value bad_flag = args_for(100, 10000);
    bad_flag.set("hold_new", Value::string("yes"));
    Result<Value> r = rig.service.restart(bad_flag);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == restart::diag::kBadHoldArgs);
    for (const std::int64_t bad : {60001, -1}) {
        Value args = hold_args(1000);
        args.set("hold_ms", Value::integer(bad));
        r = rig.service.restart(args);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == restart::diag::kBadHoldArgs);
    }
    {
        Value zero = hold_args(1000);
        zero.set("hold_ms", Value::integer(0));  // documented range 1..60000: 0 is refused, not defaulted
        r = rig.service.restart(zero);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == restart::diag::kBadHoldArgs);
        // Without hold_new the hold arguments are not looked at at all.
    }
    {
        Rig plain;
        plain.hold_supported = true;
        Value ignored = args_for(100, 10000);
        ignored.set("hold_ms", Value::integer(99999));
        ignored.set("token", Value::integer(1));
        CHECK(plain.service.restart(ignored).has_value());
    }
    for (const bool as_text : {false, true}) {
        Value args = hold_args(1000);
        args.set("token", as_text ? Value::string(std::string(65, 'x')) : Value::integer(7));  // not a string / too long
        r = rig.service.restart(args);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == restart::diag::kBadHoldArgs);
    }
    CHECK(rig.logged().empty());

    r = rig.service.restart_resume(Value::object());  // no pid
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == restart::diag::kBadHoldArgs);
    Value text = Value::object();
    text.set("pid", Value::string("200"));
    r = rig.service.restart_resume(text);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == restart::diag::kBadHoldArgs);
    r = rig.service.restart_resume(pid_args(0));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == restart::diag::kBadHoldArgs);
    r = rig.service.restart_resume(pid_args(4242));  // never held
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == restart::diag::kNotHeld);
}

TEST_CASE("restart hold: the control commands, the capability and the opt-in event") {
    QuietLogs quiet;
    Rig rig;
    rig.hold_supported = true;
    ControlContext context;
    context.version = "0.1.0";
    context.service = &rig.service;
    std::atomic<bool> gone{false};
    context.client_gone = [&] { return gone.load(); };
    const std::vector<std::string> caps = broker_capabilities(context);
    CHECK(std::find(caps.begin(), caps.end(), "restart") != caps.end());
    CHECK(std::find(caps.begin(), caps.end(), "restart-hold") != caps.end());

    std::set<std::string> subscribed;
    context.subscribe = [&](const std::set<std::string>& names) { subscribed = names; };
    Result<ipc::json::Value> reply = ipc::json::parse(handle_control_line(R"({"id":1,"cmd":"subscribe"})", context));
    REQUIRE(reply.has_value());
    CHECK(subscribed.count("restarting") == 0);  // opt-in, like `restarted`
    reply = ipc::json::parse(handle_control_line(R"({"id":2,"cmd":"subscribe","args":{"events":["restarting","restarted"]}})", context));
    REQUIRE(reply.has_value());
    CHECK(reply->get_bool("ok"));
    CHECK(subscribed == std::set<std::string>{"restarting", "restarted"});

    std::string line;
    std::thread requester([&] {
        line = handle_control_line(R"({"id":3,"cmd":"restart","args":{"pid":100,"timeout_ms":10000,"hold_new":true,"hold_ms":30000}})", context);
    });
    REQUIRE(wait_until([&] { return is_held(rig); }));
    reply = ipc::json::parse(handle_control_line(R"({"id":4,"cmd":"restart-resume","args":{"pid":200}})", context));
    REQUIRE(reply.has_value());
    CHECK(reply->get_bool("ok"));
    CHECK(reply->find("result")->get_bool("resumed"));
    requester.join();
    reply = ipc::json::parse(line);
    REQUIRE(reply.has_value());
    CHECK(reply->get_integer("id") == 3);
    CHECK(reply->get_bool("ok"));
    CHECK(reply->find("result")->get_integer("new_pid") == 200);

    reply = ipc::json::parse(handle_control_line(R"({"id":5,"cmd":"restart-resume","args":{"pid":200}})", context));
    REQUIRE(reply.has_value());
    CHECK_FALSE(reply->get_bool("ok"));
    CHECK(reply->find("diag")->get_string("id") == "CML5814");
}
