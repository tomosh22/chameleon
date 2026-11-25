#include "restart_step.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>

#include "cml/ipc/control_ids.h"
#include "subscription.h"

namespace cmltest {

namespace {

cml::Diag invalid(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioInvalid, "scenario line " + std::to_string(line) + ": " + what,
                           "Fix the scenario file (tests/harness/README.md lists the keys of every step kind).");
}

bool is_cml_id(const std::string& text) {
    return text.size() == 7 && text.compare(0, 3, "CML") == 0 && std::all_of(text.begin() + 3, text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

cml::Result<Step> parse_restart_step(const toml::Table& table) {
    Step step;
    step.kind = StepKind::Restart;
    step.line = table.line;
    static const char* const kKeys[] = {"kind",          "within_s",          "process",         "expect_result_key",    "expect_result_value",
                                        "hold_new",      "hold_ms",           "expect_restarting", "expect_error",       "expect_error_text",
                                        "expect_warning", "expect_reapply_error", "expect_not_enabled"};
    for (const auto& entry : table.entries) {
        const std::string& key = entry.first;
        if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; })) {
            return invalid(entry.second.line, "unknown key '" + key + "' in a restart step");
        }
    }
    step.within_s = 90;
    if (const toml::Value* within = table.find("within_s")) {
        if (within->kind != toml::Value::Kind::Integer || within->integer < 1 || within->integer > 3600) {
            return invalid(within->line, "'within_s' must be an integer between 1 and 3600");
        }
        step.within_s = static_cast<int>(within->integer);
    }
    const toml::Value* key = table.find("expect_result_key");
    const toml::Value* value = table.find("expect_result_value");
    if ((key == nullptr) != (value == nullptr)) {
        return invalid(table.line, "'expect_result_key' and 'expect_result_value' go together");
    }
    if (key != nullptr) {
        const bool name_ok = key->kind == toml::Value::Kind::String && !key->text.empty() && key->text.size() <= 64 &&
                             std::all_of(key->text.begin(), key->text.end(), [](char c) {
                                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                             });
        if (!name_ok) {
            return invalid(key->line, "'expect_result_key' must be a member name (letters, digits, '_', '-')");
        }
        if (value->kind != toml::Value::Kind::Integer) {
            return invalid(value->line, "'expect_result_value' must be an integer");
        }
        step.result_key = key->text;
        step.result_value = value->integer;
        step.has_result_value = true;
    }
    // WP-43.
    if (const toml::Value* hold = table.find("hold_new")) {
        if (hold->kind != toml::Value::Kind::Boolean) {
            return invalid(hold->line, "'hold_new' must be true or false");
        }
        step.hold_new = hold->boolean;
    }
    if (const toml::Value* ms = table.find("hold_ms")) {
        if (ms->kind != toml::Value::Kind::Integer || ms->integer < 1 || ms->integer > 60000) {
            return invalid(ms->line, "'hold_ms' must be an integer from 1 to 60000");
        }
        if (!step.hold_new) {
            return invalid(ms->line, "'hold_ms' needs hold_new = true");
        }
        step.hold_ms = static_cast<int>(ms->integer);
    }
    if (const toml::Value* expect = table.find("expect_restarting")) {
        if (expect->kind != toml::Value::Kind::Boolean) {
            return invalid(expect->line, "'expect_restarting' must be true or false");
        }
        if (!step.hold_new) {
            return invalid(expect->line, "'expect_restarting' needs hold_new = true");
        }
        step.expect_restarting = expect->boolean;
    }
    for (const auto& id_key : {std::make_pair("expect_error", &step.expect_error), std::make_pair("expect_warning", &step.expect_warning),
                               std::make_pair("expect_reapply_error", &step.expect_reapply_error)}) {
        if (const toml::Value* v = table.find(id_key.first)) {
            if (v->kind != toml::Value::Kind::String || !is_cml_id(v->text)) {
                return invalid(v->line, std::string("'") + id_key.first + "' must look like CML5804");
            }
            *id_key.second = v->text;
        }
    }
    for (const auto& text_key : {std::make_pair("expect_error_text", &step.expect_error_text), std::make_pair("expect_not_enabled", &step.expect_not_enabled)}) {
        if (const toml::Value* v = table.find(text_key.first)) {
            if (v->kind != toml::Value::Kind::String || v->text.empty()) {
                return invalid(v->line, std::string("'") + text_key.first + "' must be a non-empty string");
            }
            *text_key.second = v->text;
        }
    }
    if (!step.expect_error_text.empty() && step.expect_error.empty()) {
        return invalid(table.line, "'expect_error_text' needs expect_error");
    }
    if (!step.expect_error.empty() && (step.has_result_value || !step.expect_warning.empty() || !step.expect_reapply_error.empty() || !step.expect_not_enabled.empty())) {
        return invalid(table.line, "a restart step that expects a refusal (expect_error) checks nothing of a result");
    }
    return step;
}

#if defined(_WIN32)

namespace {

using Clock = std::chrono::steady_clock;
namespace ipc = cml::ipc;

RestartStepOutcome fail(std::uint32_t code, std::string detail) {
    RestartStepOutcome out;
    out.ok = false;
    out.diag_code = code;
    out.detail = std::move(detail);
    return out;
}

// WP-43: the helper that resumes a held restart. It has its own control connection (the step's connection is blocked in the
// `restart` call), subscribes to `restarting`, and answers the event that carries its token with `restart-resume`.
class HoldResumer {
public:
    HoldResumer(const std::string& pipe, std::string token, std::uint32_t limit_ms) : token_(std::move(token)) {
        cml::Result<ipc::ControlClient> client = ipc::ControlClient::connect(pipe, 15000);
        if (!client) {
            error_ = "cannot open a second control connection: " + client.error().format();
            return;
        }
        client_.emplace(std::move(*client));
        ipc::json::Value args = ipc::json::Value::object();
        ipc::json::Value events = ipc::json::Value::array();
        events.push(ipc::json::Value::string("restarting"));
        args.set("events", std::move(events));
        cml::Result<ipc::ControlResponse> subscribed = client_->call("subscribe", std::move(args), 15000);
        if (!subscribed || !subscribed->ok) {
            error_ = "subscribe to restarting failed on the second connection";
            return;
        }
        thread_ = std::thread([this, limit_ms] { run(limit_ms); });
    }
    ~HoldResumer() { stop(); }
    HoldResumer(const HoldResumer&) = delete;
    HoldResumer& operator=(const HoldResumer&) = delete;

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    const std::string& error() const { return error_; }
    bool saw_restarting() const { return saw_; }
    std::uint32_t held_pid() const { return held_pid_; }
    const std::string& resume_answer() const { return resume_answer_; }
    bool resumed() const { return resumed_; }

private:
    void run(std::uint32_t limit_ms) {
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(limit_ms);
        while (!stop_.load() && Clock::now() < until) {
            cml::Result<std::optional<ipc::ControlEvent>> event = client_->next_event(100);
            if (!event) {
                error_ = event.error().format();
                return;
            }
            if (!*event || (*event)->name != "restarting" || (*event)->body.get_string("token") != token_) {
                continue;
            }
            saw_ = true;
            held_pid_ = static_cast<std::uint32_t>((*event)->body.get_integer("new_pid"));
            ipc::json::Value args = ipc::json::Value::object();
            args.set("pid", ipc::json::Value::integer(held_pid_));
            cml::Result<ipc::ControlResponse> answer = client_->call("restart-resume", std::move(args), 30000);
            if (!answer) {
                resume_answer_ = answer.error().format();
            } else if (!answer->ok) {
                resume_answer_ = answer->diag->id + ": " + answer->diag->message;
            } else {
                resumed_ = answer->result.get_bool("resumed");
                resume_answer_ = ipc::json::serialize(answer->result);
            }
            return;
        }
    }

    std::string token_;
    std::optional<ipc::ControlClient> client_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::string error_;
    bool saw_ = false;
    bool resumed_ = false;
    std::uint32_t held_pid_ = 0;
    std::string resume_answer_;
};

// The process's agent is connected (a process that a failed restart left running registers by itself, a moment later).
std::string wait_connected(ipc::ControlClient& control, std::uint32_t pid, std::uint32_t limit_ms) {
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(std::min<std::uint32_t>(limit_ms, 60000));
    for (;;) {
        cml::Result<ipc::StatusReport> status = control.status(15000);
        if (!status) {
            return status.error().format();
        }
        if (status->find_process(pid) != nullptr) {
            return std::string();
        }
        if (Clock::now() >= until) {
            return "process " + std::to_string(pid) + " has no connected agent (status lists " + std::to_string(status->processes.size()) + " process(es))";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace

RestartStepOutcome run_restart_step(ipc::ControlClient& control, const std::vector<std::string>& capabilities, std::uint32_t old_pid, const Step& step,
                                    std::uint32_t limit_ms, const std::string& control_pipe) {
    constexpr std::uint32_t kControlTimeoutMs = 15000;
    // The `restarted` event is opt-in: replace this connection's subscription (the runner's default set plus the event).
    {
        ipc::json::Value args = ipc::json::Value::object();
        ipc::json::Value events = ipc::json::Value::array();
        for (const std::string& e : subscription_events(capabilities)) {  // the runner's set (also what other capabilities add) ...
            events.push(ipc::json::Value::string(e));
        }
        events.push(ipc::json::Value::string("restarted"));  // ... plus ours
        args.set("events", std::move(events));
        cml::Result<ipc::ControlResponse> subscribed = control.call("subscribe", std::move(args), kControlTimeoutMs);
        if (!subscribed) {
            return fail(cml::ids::kRestartStepFailed, subscribed.error().format());
        }
        if (!subscribed->ok) {
            return fail(cml::ids::kRestartStepFailed, "subscribe to the restarted event failed: " + subscribed->diag->id + ": " + subscribed->diag->message);
        }
    }
    if (const std::string not_connected = wait_connected(control, old_pid, limit_ms); !not_connected.empty()) {
        return fail(cml::ids::kRestartStepFailed, "cannot restart: " + not_connected);
    }

    ipc::json::Value args = ipc::json::Value::object();
    args.set("pid", ipc::json::Value::integer(old_pid));
    args.set("timeout_ms", ipc::json::Value::integer(limit_ms));               // before the old process is terminated
    args.set("registration_timeout_ms", ipc::json::Value::integer(limit_ms));  // after it: registration, modules, indexes, re-applying
    std::optional<HoldResumer> resumer;
    if (step.hold_new) {
        const std::string token = "cmltest-" + std::to_string(old_pid) + "-" + std::to_string(Clock::now().time_since_epoch().count());
        args.set("hold_new", ipc::json::Value::boolean(true));
        args.set("hold_ms", ipc::json::Value::integer(step.hold_ms));
        args.set("token", ipc::json::Value::string(token));
        resumer.emplace(control_pipe, token, 2 * limit_ms + 15000);
        if (!resumer->error().empty()) {
            return fail(cml::ids::kRestartStepFailed, resumer->error());
        }
    }
    cml::Result<ipc::ControlResponse> response = control.call("restart", std::move(args), 2 * limit_ms + 15000);
    if (resumer) {
        resumer->stop();
    }
    if (!response) {
        return fail(cml::ids::kRestartStepFailed, response.error().format());
    }

    // WP-43: an expected refusal. The process to follow is the one the refusal's diag event names when it is not the old one (the
    // restart failed after the old process was gone and left the new one running).
    if (!step.expect_error.empty()) {
        if (response->ok) {
            return fail(cml::ids::kRestartStepMismatch, "the restart succeeded, expected the refusal " + step.expect_error + ": " + ipc::json::serialize(response->result));
        }
        const ipc::ControlDiag& d = *response->diag;
        if (d.id != step.expect_error) {
            return fail(cml::ids::kRestartStepMismatch, "the restart was refused with " + d.id + ": " + d.message + ", expected " + step.expect_error);
        }
        if (!step.expect_error_text.empty() && d.message.find(step.expect_error_text) == std::string::npos) {
            return fail(cml::ids::kRestartStepMismatch, "the refusal " + d.id + " does not mention '" + step.expect_error_text + "': " + d.message);
        }
        RestartStepOutcome out;
        out.old_pid = old_pid;
        out.new_pid = old_pid;
        const Clock::time_point until = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < until) {
            cml::Result<std::optional<ipc::ControlEvent>> event = control.next_event(100);
            if (!event) {
                return fail(cml::ids::kRestartStepFailed, event.error().format());
            }
            if (!*event) {
                if (control.queued_events() == 0) {
                    break;  // the diag event comes before the answer: everything is read
                }
                continue;
            }
            if ((*event)->name == "diag" && (*event)->body.get_string("id") == d.id) {
                const std::int64_t pid = (*event)->body.get_integer("pid");
                if (pid > 0) {
                    out.new_pid = static_cast<std::uint32_t>(pid);
                }
                break;
            }
        }
        out.detail = "restart refused as expected: " + d.id + ": " + d.message +
                     (out.new_pid != old_pid ? " (following the new process " + std::to_string(out.new_pid) + ")" : " (the old process " + std::to_string(old_pid) + " runs on)");
        return out;
    }

    if (!response->ok) {
        const ipc::ControlDiag& d = *response->diag;
        return fail(cml::ids::kRestartStepFailed, "restart of pid " + std::to_string(old_pid) + " failed: " + d.id + ": " + d.message + " What to do: " + d.action);
    }
    const ipc::json::Value& result = response->result;
    RestartStepOutcome out;
    out.old_pid = static_cast<std::uint32_t>(result.get_integer("old_pid"));
    out.new_pid = static_cast<std::uint32_t>(result.get_integer("new_pid"));
    if (out.old_pid != old_pid) {
        return fail(cml::ids::kRestartStepMismatch, "the restart answered for pid " + std::to_string(out.old_pid) + ", expected " + std::to_string(old_pid));
    }
    if (out.new_pid == 0 || out.new_pid == old_pid) {
        return fail(cml::ids::kRestartStepMismatch, "the restart reports new_pid " + std::to_string(out.new_pid) + " for old pid " + std::to_string(old_pid));
    }
    if (const ipc::json::Value* error = result.find("reapply_error")) {
        if (step.expect_reapply_error.empty()) {
            return fail(cml::ids::kRestartStepMismatch, "re-applying the session's edits to the new process failed: " + ipc::json::serialize(*error));
        }
        if (error->get_string("id") != step.expect_reapply_error) {
            return fail(cml::ids::kRestartStepMismatch, "reapply_error is " + ipc::json::serialize(*error) + ", expected the id " + step.expect_reapply_error);
        }
    } else if (!step.expect_reapply_error.empty()) {
        return fail(cml::ids::kRestartStepMismatch, "the result has no reapply_error, expected " + step.expect_reapply_error + ": " + ipc::json::serialize(result));
    }
    const ipc::json::Value* missing = result.find("not_enabled");
    const bool any_missing = missing != nullptr && !missing->items().empty();
    if (step.expect_not_enabled.empty()) {
        if (any_missing) {
            return fail(cml::ids::kRestartStepMismatch, "modules of the old process are not enabled in the new one: " + ipc::json::serialize(*missing));
        }
    } else {
        const bool listed = any_missing && std::any_of(missing->items().begin(), missing->items().end(), [&](const ipc::json::Value& m) {
                                return m.is_string() && m.as_string().find(step.expect_not_enabled) != std::string::npos;
                            });
        if (!listed) {
            return fail(cml::ids::kRestartStepMismatch,
                        "not_enabled does not list " + step.expect_not_enabled + ": " + (missing != nullptr ? ipc::json::serialize(*missing) : std::string("(absent)")));
        }
    }
    if (step.has_result_value) {
        const ipc::json::Value* member = result.find(step.result_key);
        if (member == nullptr || !member->is_integer() || member->as_integer() != step.result_value) {
            return fail(cml::ids::kRestartStepMismatch, "the restart result has " + step.result_key + " = " +
                                                             (member == nullptr ? std::string("(absent)") : ipc::json::serialize(*member)) +
                                                             ", expected " + std::to_string(step.result_value));
        }
    }
    std::string hold_detail;
    if (resumer) {
        if (step.expect_restarting) {
            if (!resumer->saw_restarting()) {
                return fail(cml::ids::kRestartStepMismatch, "the held restart published no `restarting` event with the step's token");
            }
            if (resumer->held_pid() != out.new_pid) {
                return fail(cml::ids::kRestartStepMismatch, "`restarting` named pid " + std::to_string(resumer->held_pid()) + ", the restart answered " + std::to_string(out.new_pid));
            }
            if (!resumer->resumed()) {
                return fail(cml::ids::kRestartStepMismatch, "restart-resume of pid " + std::to_string(out.new_pid) + " did not answer resumed:true: " + resumer->resume_answer());
            }
            hold_detail = "; held, resumed by restart-resume (" + resumer->resume_answer() + ")";
        } else {
            if (resumer->saw_restarting()) {
                return fail(cml::ids::kRestartStepMismatch, "a `restarting` event arrived, but the scenario expects the hold not to be kept");
            }
            hold_detail = "; no `restarting` (not held)";
        }
    }

    // The event comes from the same publication as the answer; it may arrive just after it. The warnings of the restart come before it.
    const Clock::time_point until = Clock::now() + std::chrono::seconds(5);
    bool seen = false;
    bool warning_seen = step.expect_warning.empty();
    std::string warnings;
    while (!seen && Clock::now() < until) {
        cml::Result<std::optional<ipc::ControlEvent>> event = control.next_event(250);
        if (!event) {
            return fail(cml::ids::kRestartStepFailed, event.error().format());
        }
        if (*event && (*event)->name == "diag" && (*event)->body.get_string("severity") == "warning") {
            const std::string id = (*event)->body.get_string("id");
            warnings += (warnings.empty() ? "" : ", ") + id;
            if (id == step.expect_warning) {
                warning_seen = true;
            }
        }
        if (*event && (*event)->name == "restarted") {
            const ipc::json::Value& body = (*event)->body;
            if (static_cast<std::uint32_t>(body.get_integer("old_pid")) != old_pid || static_cast<std::uint32_t>(body.get_integer("new_pid")) != out.new_pid) {
                return fail(cml::ids::kRestartStepMismatch, "the restarted event " + ipc::json::serialize(body) + " differs from the answer (old " +
                                                                 std::to_string(old_pid) + ", new " + std::to_string(out.new_pid) + ")");
            }
            seen = true;
        }
    }
    if (!seen) {
        return fail(cml::ids::kRestartStepMismatch, "no 'restarted' event arrived after the restart answered");
    }
    if (!warning_seen) {
        return fail(cml::ids::kRestartStepMismatch,
                    "no warning " + step.expect_warning + " came with the restart (warnings: " + (warnings.empty() ? std::string("none") : warnings) + ")");
    }

    std::string modules;
    if (const ipc::json::Value* list = result.find("modules")) {
        for (const ipc::json::Value& m : list->items()) {
            modules += (modules.empty() ? "" : ", ") + m.as_string();
        }
    }
    out.detail = "pid " + std::to_string(old_pid) + " -> " + std::to_string(out.new_pid) + "; pre-restart hooks run " +
                 std::to_string(result.get_integer("hooks_run")) + "; re-applied " + std::to_string(result.get_integer("reapplied_files")) +
                 " file(s), patched " + std::to_string(result.get_integer("patched")) + "; enabled again: " + modules + "; " +
                 std::to_string(result.get_integer("restart_ms")) + " ms" + hold_detail + (warnings.empty() ? std::string() : "; warnings: " + warnings);
    return out;
}

#endif  // _WIN32

}  // namespace cmltest
