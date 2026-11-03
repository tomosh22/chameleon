#include "restart_step.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "cml/ipc/control_ids.h"
#include "subscription.h"

namespace cmltest {

namespace {

cml::Diag invalid(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioInvalid, "scenario line " + std::to_string(line) + ": " + what,
                           "Fix the scenario file (tests/harness/README.md lists the keys of every step kind).");
}

}  // namespace

cml::Result<Step> parse_restart_step(const toml::Table& table) {
    Step step;
    step.kind = StepKind::Restart;
    step.line = table.line;
    for (const auto& entry : table.entries) {
        const std::string& key = entry.first;
        if (key != "kind" && key != "within_s" && key != "process" && key != "expect_result_key" && key != "expect_result_value") {
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

}  // namespace

RestartStepOutcome run_restart_step(ipc::ControlClient& control, const std::vector<std::string>& capabilities, std::uint32_t old_pid, const Step& step,
                                    std::uint32_t limit_ms) {
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

    ipc::json::Value args = ipc::json::Value::object();
    args.set("pid", ipc::json::Value::integer(old_pid));
    args.set("timeout_ms", ipc::json::Value::integer(limit_ms));               // before the old process is terminated
    args.set("registration_timeout_ms", ipc::json::Value::integer(limit_ms));  // after it: registration, modules, indexes, re-applying
    cml::Result<ipc::ControlResponse> response = control.call("restart", std::move(args), 2 * limit_ms + 15000);
    if (!response) {
        return fail(cml::ids::kRestartStepFailed, response.error().format());
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
        return fail(cml::ids::kRestartStepMismatch, "re-applying the session's edits to the new process failed: " + ipc::json::serialize(*error));
    }
    if (const ipc::json::Value* missing = result.find("not_enabled"); missing != nullptr && !missing->items().empty()) {
        return fail(cml::ids::kRestartStepMismatch, "modules of the old process are not enabled in the new one: " + ipc::json::serialize(*missing));
    }
    if (step.has_result_value) {
        const ipc::json::Value* member = result.find(step.result_key);
        if (member == nullptr || !member->is_integer() || member->as_integer() != step.result_value) {
            return fail(cml::ids::kRestartStepMismatch, "the restart result has " + step.result_key + " = " +
                                                             (member == nullptr ? std::string("(absent)") : ipc::json::serialize(*member)) +
                                                             ", expected " + std::to_string(step.result_value));
        }
    }

    // The event comes from the same publication as the answer; it may arrive just after it.
    const Clock::time_point until = Clock::now() + std::chrono::seconds(5);
    bool seen = false;
    while (!seen && Clock::now() < until) {
        cml::Result<std::optional<ipc::ControlEvent>> event = control.next_event(250);
        if (!event) {
            return fail(cml::ids::kRestartStepFailed, event.error().format());
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

    std::string modules;
    if (const ipc::json::Value* list = result.find("modules")) {
        for (const ipc::json::Value& m : list->items()) {
            modules += (modules.empty() ? "" : ", ") + m.as_string();
        }
    }
    out.detail = "pid " + std::to_string(old_pid) + " -> " + std::to_string(out.new_pid) + "; pre-restart hooks run " +
                 std::to_string(result.get_integer("hooks_run")) + "; re-applied " + std::to_string(result.get_integer("reapplied_files")) +
                 " file(s), patched " + std::to_string(result.get_integer("patched")) + "; enabled again: " + modules + "; " +
                 std::to_string(result.get_integer("restart_ms")) + " ms";
    return out;
}

#endif  // _WIN32

}  // namespace cmltest
