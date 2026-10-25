#include "runner.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#include "cml/ipc/control.h"
#include "cml/ipc/pipe.h"
#include "cml/proc/proc.h"
#include "fixture_builder.h"
#include "rx.h"
#include "supervisor.h"
#include "unified_diff.h"

namespace cmltest {

namespace fs = std::filesystem;
namespace ipc = cml::ipc;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint32_t kPollMs = 50;
constexpr std::uint32_t kControlTimeoutMs = 15000;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string read_whole_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string tail_lines(const std::string& text, std::size_t count) {
    std::size_t pos = text.size();
    std::size_t seen = 0;
    while (pos > 0 && seen <= count) {
        --pos;
        if (text[pos] == '\n') {
            ++seen;
        }
    }
    return pos == 0 ? text : text.substr(pos + 1);
}

std::string line_around(const std::string& text, std::size_t at) {
    std::size_t begin = text.rfind('\n', at == 0 ? 0 : at - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    std::size_t end = text.find_first_of("\r\n", at);
    if (end == std::string::npos) {
        end = text.size();
    }
    std::string line = text.substr(begin, end - begin);
    if (line.size() > 160) {
        line.resize(160);
    }
    return line;
}

// A module spec is a file name or a path suffix; compared case-insensitively on '\' separators.
bool module_matches(const std::string& spec, const std::string& path) {
    std::string a = lower(spec);
    std::string b = lower(path);
    std::replace(a.begin(), a.end(), '/', '\\');
    std::replace(b.begin(), b.end(), '/', '\\');
    if (b.size() < a.size() || b.compare(b.size() - a.size(), a.size(), a) != 0) {
        return false;
    }
    return b.size() == a.size() || b[b.size() - a.size() - 1] == '\\' || a.find('\\') != std::string::npos;
}

struct StepOutcome {
    bool ok = true;
    std::string detail;
    std::string diag_id;

    static StepOutcome pass(std::string detail = std::string()) {
        StepOutcome o;
        o.detail = std::move(detail);
        return o;
    }
    static StepOutcome fail(std::uint32_t code, std::string detail) {
        StepOutcome o;
        o.ok = false;
        o.diag_id = cml::Diag::make(code, "", "").id();
        o.detail = std::move(detail);
        return o;
    }
    static StepOutcome fail(const cml::Diag& diag) {
        StepOutcome o;
        o.ok = false;
        o.diag_id = diag.id();
        o.detail = diag.format();
        return o;
    }
};

class ScenarioRun {
public:
    ScenarioRun(const Scenario& scenario, const RunnerOptions& options) : scenario_(scenario), options_(options) {}

    ScenarioReport run() {
        const Clock::time_point start = Clock::now();
        report_.scenario = scenario_.name;
        report_.generator = options_.generator;
        report_.config = options_.config;

        std::error_code ec;
        fs::create_directories(options_.artifacts_dir, ec);
        fs::create_directories(options_.work_dir, ec);

        record("build fixture", [&] { return build(); });
        // The run phase (everything after the build) is bounded by timeout_s.
        run_deadline_ = Clock::now() + std::chrono::seconds(scenario_.timeout_s);

        for (const Step& step : scenario_.steps) {
            const std::string name = step.describe();
            if (failed_) {
                add(name, StepStatus::NotRun, "an earlier step failed", std::string(), 0.0);
                continue;
            }
            if (skip_reason_) {
                add(name, StepStatus::SkippedCapability, *skip_reason_, std::string(), 0.0);
                continue;
            }
            if (const char* capability = step.required_capability()) {
                std::string problem;
                const CapabilityCheck check = check_capability(capability, &problem);
                if (check == CapabilityCheck::Missing) {
                    skip_reason_ = std::string("capability '") + capability + "' not provided by broker (lands in WP-12)";
                    add(name, StepStatus::SkippedCapability, *skip_reason_, std::string(), 0.0);
                    continue;
                }
                if (check == CapabilityCheck::Error) {
                    add(name, StepStatus::Failed, problem, "CML5406", 0.0);
                    on_failure(name + ": " + problem);
                    continue;
                }
            }
            record(name, [&] { return execute(step); });
        }

        shutdown();
        report_.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        return report_;
    }

private:
    enum class CapabilityCheck { Have, Missing, Error };

    void add(const std::string& name, StepStatus status, const std::string& detail, const std::string& diag_id, double seconds) {
        StepResult r;
        r.name = name;
        r.status = status;
        r.detail = detail;
        r.diag_id = diag_id;
        r.seconds = seconds;
        report_.steps.push_back(std::move(r));
    }

    template <class Fn>
    void record(const std::string& name, Fn fn) {
        if (failed_) {
            add(name, StepStatus::NotRun, "an earlier step failed", std::string(), 0.0);
            return;
        }
        const Clock::time_point t0 = Clock::now();
        StepOutcome outcome = fn();
        const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        if (outcome.ok) {
            add(name, StepStatus::Passed, outcome.detail, std::string(), seconds);
        } else {
            add(name, StepStatus::Failed, outcome.detail, outcome.diag_id, seconds);
            on_failure(name + ": " + outcome.detail);
        }
    }

    // First failure: write minidumps of everything still running (before anything is killed) and
    // stop the scenario. The logs are already in the artifact directory.
    void on_failure(const std::string& why) {
        failed_ = true;
        if (launched_) {
            const Supervisor::DumpResult dumps = supervisor_.dump_running(options_.artifacts_dir / "dumps");
            std::string note = "failure: " + why + "\n";
            for (const fs::path& f : dumps.files) {
                note += "dump: " + f.string() + "\n";
            }
            for (const std::string& e : dumps.errors) {
                note += "dump error: " + e + "\n";
            }
            std::ofstream out(options_.artifacts_dir / "failure.txt", std::ios::binary | std::ios::trunc);
            out << note;
        }
    }

    void shutdown() {
        control_.reset();
        supervisor_.kill_all();
    }

    std::uint32_t remaining_ms() const {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(run_deadline_ - Clock::now()).count();
        return left <= 0 ? 0u : static_cast<std::uint32_t>(left);
    }

    StepOutcome scenario_timeout() const {
        return StepOutcome::fail(cml::ids::kScenarioFailed,
                                 "the scenario's timeout_s = " + std::to_string(scenario_.timeout_s) + " s ran out");
    }

    // --- steps -----------------------------------------------------------------------------

    StepOutcome execute(const Step& step) {
        switch (step.kind) {
            case StepKind::Launch: return launch();
            case StepKind::ExpectStdout: return expect_stdout(step);
            case StepKind::ExpectModuleEnabled: return expect_module_enabled(step);
            case StepKind::Edit: return edit(step);
            case StepKind::Reload: return reload(step);
            case StepKind::ExpectDiag: return expect_diag(step);
        }
        return StepOutcome::fail(cml::ids::kScenarioFailed, "unknown step kind");
    }

    StepOutcome build() {
        cml::Result<CompilerChoice> resolved = resolve_compiler(options_.cxx);
        CompilerChoice compiler;
        if (resolved) {
            compiler = *resolved;
        } else if (options_.prebuilt_fixture.empty()) {
            return StepOutcome::fail(resolved.error());
        } else {
            compiler.display = options_.cxx;  // a prebuilt fixture needs no compiler
        }
        report_.compiler = compiler.display;
        BuildSettings settings;
        settings.repo_root = options_.repo_root;
        settings.fixture = scenario_.fixture;
        settings.work_dir = options_.work_dir;
        settings.artifacts_dir = options_.artifacts_dir;
        settings.agent_dir = options_.agent_dir;
        settings.generator = options_.generator;
        settings.vs_generator = options_.vs_generator;
        settings.config = options_.config;
        settings.compiler = compiler;
        settings.cmake = options_.cmake;
        settings.build_timeout_ms = options_.build_timeout_ms;
        settings.prebuilt_exe = options_.prebuilt_fixture;
        cml::Result<BuiltFixture> built = build_fixture(supervisor_, settings);
        if (!built) {
            return StepOutcome::fail(built.error());
        }
        built_ = *built;
        return StepOutcome::pass(options_.generator + " build of fixture_" + scenario_.fixture + " ok: " + built_.exe.string());
    }

    StepOutcome launch() {
        launched_ = true;
        const std::string unique = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
        agent_pipe_ = "\\\\.\\pipe\\cml_t_" + unique;
        control_pipe_ = "\\\\.\\pipe\\cml_tc_" + unique;
        const fs::path broker_log = options_.artifacts_dir / "broker.log";
        const fs::path agent_log = options_.artifacts_dir / "agent.log";
        fixture_stdout_ = options_.artifacts_dir / "fixture.stdout.txt";

        ChildSpec broker;
        broker.label = "broker";
        broker.exe = options_.broker_exe;
        broker.arguments = L"--headless --pipe " + quote_argument(cml::proc::wide_from_utf8(agent_pipe_)) + L" --control-pipe " +
                           quote_argument(cml::proc::wide_from_utf8(control_pipe_)) + L" --log " + quote_argument(broker_log.wstring()) +
                           L" --startup-timeout 300000 --verbose";
        broker.working_directory = options_.artifacts_dir;
        broker.output_file = options_.artifacts_dir / "broker.stdout.txt";
        cml::Result<std::size_t> broker_id = supervisor_.start(broker);
        if (!broker_id) {
            return StepOutcome::fail(broker_id.error());
        }
        broker_id_ = *broker_id;

        // Both pipes exist once the broker listens; bounded by the broker dying or the run deadline.
        const std::uint32_t wait_ms = std::min<std::uint32_t>(30000, remaining_ms());
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(wait_ms);
        while (!(ipc::pipe_exists(agent_pipe_) && ipc::pipe_exists(control_pipe_))) {
            if (supervisor_.exit_code(broker_id_)) {
                return StepOutcome::fail(cml::ids::kLaunchFailed,
                                         "the broker exited with code " + std::to_string(*supervisor_.exit_code(broker_id_)) +
                                             " before its pipes appeared; broker log:\n" + tail_lines(read_whole_file(broker_log), 20) +
                                             tail_lines(read_whole_file(options_.artifacts_dir / "broker.stdout.txt"), 20));
            }
            if (Clock::now() >= until) {
                return StepOutcome::fail(cml::ids::kLaunchFailed,
                                         "the broker's pipes did not appear within " + std::to_string(wait_ms) + " ms; broker log:\n" +
                                             tail_lines(read_whole_file(broker_log), 20));
            }
            Sleep(10);
        }

        cml::Result<ipc::ControlClient> client = ipc::ControlClient::connect(control_pipe_, kControlTimeoutMs);
        if (!client) {
            return StepOutcome::fail(client.error());
        }
        control_ = std::make_unique<ipc::ControlClient>(std::move(*client));
        cml::Result<ipc::StatusReport> status = control_->status(kControlTimeoutMs);
        if (!status) {
            return StepOutcome::fail(status.error());
        }
        capabilities_ = status->capabilities;
        broker_version_ = status->version;
        if (status->has_capability(ipc::capability::kSubscribe)) {
            ipc::json::Value args = ipc::json::Value::object();
            ipc::json::Value events = ipc::json::Value::array();
            for (const char* e : {"diag", "patch_applied", "log"}) {
                events.push(ipc::json::Value::string(e));
            }
            args.set("events", std::move(events));
            cml::Result<ipc::ControlResponse> subscribed = control_->call("subscribe", std::move(args), kControlTimeoutMs);
            if (!subscribed) {
                return StepOutcome::fail(subscribed.error());
            }
            if (!subscribed->ok) {
                return StepOutcome::fail(ipc::to_diag(*subscribed->diag));
            }
        }

        ChildSpec fixture;
        fixture.label = "fixture";
        fixture.exe = built_.exe;
        fixture.arguments = L"--agent";
        fixture.working_directory = built_.exe.parent_path();
        fixture.output_file = fixture_stdout_;
        fixture.environment[L"CML_PIPE_NAME"] = cml::proc::wide_from_utf8(agent_pipe_);
        fixture.environment[L"CML_CONTROL_PIPE"] = cml::proc::wide_from_utf8(control_pipe_);
        fixture.environment[L"CML_AGENT_LOG"] = agent_log.wstring();
        // The broker is already running; make sure the agent can never start a stray one (outside our job).
        fixture.environment[L"CML_BROKER_PATH"] = (options_.work_dir / "no_such_broker.exe").wstring();
        cml::Result<std::size_t> fixture_id = supervisor_.start(fixture);
        if (!fixture_id) {
            return StepOutcome::fail(fixture_id.error());
        }
        fixture_id_ = *fixture_id;
        return StepOutcome::pass("broker pid " + std::to_string(supervisor_.pid(broker_id_)) + " (v" + broker_version_ + ", capabilities: " +
                                 join(capabilities_) + "), fixture pid " + std::to_string(supervisor_.pid(fixture_id_)));
    }

    static std::string join(const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& s : items) {
            out += (out.empty() ? "" : ",") + s;
        }
        return out;
    }

    StepOutcome expect_stdout(const Step& step) {
        cml::Result<Regex> regex = Regex::compile(step.regex);
        if (!regex) {
            return StepOutcome::fail(regex.error());
        }
        const std::uint32_t limit_ms = std::min<std::uint32_t>(static_cast<std::uint32_t>(step.within_s) * 1000u, remaining_ms());
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(limit_ms);
        for (;;) {
            const bool exited = supervisor_.exit_code(fixture_id_).has_value();  // look BEFORE reading the output
            const std::string output = read_whole_file(fixture_stdout_);
            Regex::Match match;
            if (cursor_ <= output.size() && regex->search(output, cursor_, &match)) {
                cursor_ = match.end;
                return StepOutcome::pass("matched: " + line_around(output, match.begin));
            }
            if (regex->budget_exceeded()) {
                return StepOutcome::fail(cml::ids::kRegexInvalid, "pattern /" + step.regex + "/ exceeded the matcher's step budget (catastrophic backtracking?)");
            }
            if (exited) {
                return StepOutcome::fail(cml::ids::kExpectStdoutTimeout,
                                         "the fixture exited with code " + std::to_string(*supervisor_.exit_code(fixture_id_)) +
                                             " before /" + step.regex + "/ appeared; last output:\n" + tail_lines(output, 15));
            }
            if (Clock::now() >= until) {
                if (limit_ms < static_cast<std::uint32_t>(step.within_s) * 1000u) {
                    return scenario_timeout();
                }
                return StepOutcome::fail(cml::ids::kExpectStdoutTimeout,
                                         "/" + step.regex + "/ did not appear within " + std::to_string(step.within_s) +
                                             " s; last output:\n" + tail_lines(output, 15));
            }
            Sleep(kPollMs);
        }
    }

    StepOutcome expect_module_enabled(const Step& step) {
        const std::uint32_t limit_ms = std::min<std::uint32_t>(static_cast<std::uint32_t>(step.within_s) * 1000u, remaining_ms());
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(limit_ms);
        const std::uint32_t pid = supervisor_.pid(fixture_id_);
        std::string last;
        for (;;) {
            cml::Result<ipc::StatusReport> status = control_->status(kControlTimeoutMs);
            if (!status) {
                return StepOutcome::fail(status.error());
            }
            const ipc::StatusProcess* process = status->find_process(pid);
            if (process != nullptr) {
                for (const ipc::StatusModule& module : process->modules) {
                    if (module.enabled && module_matches(step.module, module.path)) {
                        write_status_artifact(*status);
                        return StepOutcome::pass("pid " + std::to_string(pid) + " lists " + module.path + " enabled");
                    }
                }
                last = "pid " + std::to_string(pid) + " is registered with " + std::to_string(process->modules.size()) + " modules, none matching '" +
                       step.module + "' is enabled";
            } else {
                last = "the broker does not list pid " + std::to_string(pid) + " (it knows " + std::to_string(status->processes.size()) + " processes)";
            }
            if (supervisor_.exit_code(fixture_id_)) {
                return StepOutcome::fail(cml::ids::kExpectModuleTimeout,
                                         "the fixture exited with code " + std::to_string(*supervisor_.exit_code(fixture_id_)) + "; " + last);
            }
            if (Clock::now() >= until) {
                if (limit_ms < static_cast<std::uint32_t>(step.within_s) * 1000u) {
                    return scenario_timeout();
                }
                return StepOutcome::fail(cml::ids::kExpectModuleTimeout,
                                         "module '" + step.module + "' not reported enabled within " + std::to_string(step.within_s) + " s: " + last);
            }
            Sleep(200);
        }
    }

    void write_status_artifact(const ipc::StatusReport& status) {
        std::ofstream out(options_.artifacts_dir / "status.json", std::ios::binary | std::ios::trunc);
        out << ipc::json::serialize(ipc::status_to_json(status)) << "\n";
    }

    StepOutcome edit(const Step& step) {
        const fs::path patch_file = built_.source_dir / fs::path(step.patch);
        std::ifstream in(patch_file, std::ios::binary);
        if (!in) {
            return StepOutcome::fail(cml::ids::kEditFailed, "cannot read the patch '" + patch_file.string() + "'");
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        // Applied to the COPY of the sources in the scratch tree; the repository is never touched.
        cml::Result<AppliedPatch> applied = apply_patch_to_tree(built_.source_dir, buffer.str(), 1);
        if (!applied) {
            return StepOutcome::fail(applied.error());
        }
        std::string detail = "patched";
        for (const std::string& f : applied->files) {
            detail += " " + f;
        }
        for (const std::string& n : applied->notes) {
            detail += "; " + n;
        }
        return StepOutcome::pass(detail + " in " + built_.source_dir.string());
    }

    StepOutcome reload(const Step& step) {
        const std::uint32_t limit_ms = std::min<std::uint32_t>(static_cast<std::uint32_t>(step.within_s) * 1000u, remaining_ms());
        if (limit_ms == 0) {
            return scenario_timeout();
        }
        // Output printed from now on belongs to the reloaded program.
        cursor_ = read_whole_file(fixture_stdout_).size();
        ipc::json::Value args = ipc::json::Value::object();
        args.set("timeout_ms", ipc::json::Value::integer(limit_ms));
        cml::Result<ipc::ControlResponse> response = control_->call("reload", std::move(args), limit_ms + 5000);
        if (!response) {
            return StepOutcome::fail(response.error());
        }
        if (!response->ok) {
            const ipc::ControlDiag& d = *response->diag;
            return StepOutcome::fail(cml::ids::kReloadFailed, "reload failed: " + d.id + ": " + d.message + " What to do: " + d.action);
        }
        std::string summary = ipc::json::serialize(response->result);
        if (summary.size() > 200) {
            summary.resize(200);
        }
        return StepOutcome::pass("reload ok " + summary);
    }

    StepOutcome expect_diag(const Step& step) {
        const std::uint32_t limit_ms = std::min<std::uint32_t>(static_cast<std::uint32_t>(step.within_s) * 1000u, remaining_ms());
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(limit_ms);
        std::string seen;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
            const std::uint32_t slice = left <= 0 ? 0u : static_cast<std::uint32_t>(std::min<long long>(left, 250));
            cml::Result<std::optional<ipc::ControlEvent>> event = control_->next_event(slice);
            if (!event) {
                return StepOutcome::fail(event.error());
            }
            if (*event) {
                const ipc::ControlEvent& e = **event;
                if (e.name == "diag") {
                    const std::string id = e.body.get_string("id");
                    seen += (seen.empty() ? "" : ", ") + id;
                    if (id == step.id) {
                        return StepOutcome::pass("diag " + id + ": " + e.body.get_string("message"));
                    }
                }
                continue;
            }
            if (left <= 0) {
                if (limit_ms < static_cast<std::uint32_t>(step.within_s) * 1000u) {
                    return scenario_timeout();
                }
                return StepOutcome::fail(cml::ids::kExpectDiagTimeout,
                                         "no diag event " + step.id + " within " + std::to_string(step.within_s) + " s (diag events seen: " +
                                             (seen.empty() ? "none" : seen) + ")");
            }
        }
    }

    CapabilityCheck check_capability(const char* capability, std::string* problem) {
        if (!control_) {
            *problem = "cannot check the broker's capabilities: there is no control connection (no launch step ran)";
            return CapabilityCheck::Error;
        }
        cml::Result<ipc::StatusReport> status = control_->status(kControlTimeoutMs);
        if (!status) {
            *problem = status.error().format();
            return CapabilityCheck::Error;
        }
        return status->has_capability(capability) ? CapabilityCheck::Have : CapabilityCheck::Missing;
    }

    const Scenario& scenario_;
    const RunnerOptions& options_;
    ScenarioReport report_;
    Supervisor supervisor_;
    BuiltFixture built_;
    std::unique_ptr<ipc::ControlClient> control_;
    std::vector<std::string> capabilities_;
    std::string broker_version_;
    std::string agent_pipe_;
    std::string control_pipe_;
    fs::path fixture_stdout_;
    std::size_t broker_id_ = 0;
    std::size_t fixture_id_ = 0;
    std::size_t cursor_ = 0;
    bool launched_ = false;
    bool failed_ = false;
    std::optional<std::string> skip_reason_;
    Clock::time_point run_deadline_ = Clock::now();
};

}  // namespace

ScenarioReport run_scenario(const Scenario& scenario, const RunnerOptions& options) {
    ScenarioRun run(scenario, options);
    return run.run();
}

}  // namespace cmltest

#endif  // _WIN32
