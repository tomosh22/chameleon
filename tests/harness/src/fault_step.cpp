#include "fault_step.h"

#include <algorithm>
#include <string>
#include <utility>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/json.h"

namespace cmltest {

namespace {

cml::Diag invalid(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioInvalid, "scenario line " + std::to_string(line) + ": " + what,
                           "Fix the scenario file (tests/harness/README.md lists the keys of every step kind).");
}

}  // namespace

cml::Result<Step> parse_inject_step(const toml::Table& table) {
    Step step;
    step.kind = StepKind::InjectFailure;
    step.line = table.line;
    for (const auto& entry : table.entries) {
        const std::string& key = entry.first;
        if (key != "kind" && key != "stage" && key != "count" && key != "expect_error") {
            return invalid(entry.second.line, "unknown key '" + key + "' in an inject_failure step");
        }
    }
    const toml::Value* stage = table.find("stage");
    if (stage == nullptr) {
        return invalid(table.line, "an inject_failure step needs the key 'stage'");
    }
    const bool stage_ok = stage->kind == toml::Value::Kind::String && !stage->text.empty() && stage->text.size() <= 64 &&
                          std::all_of(stage->text.begin(), stage->text.end(),
                                      [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
    if (!stage_ok) {
        return invalid(stage->line, "'stage' must be a stage name (lower-case letters, digits, '-', '_'), e.g. \"link\", or \"none\"");
    }
    step.fault_stage = stage->text;
    if (const toml::Value* count = table.find("count")) {
        if (count->kind != toml::Value::Kind::Integer || count->integer < 1 || count->integer > 1000) {
            return invalid(count->line, "'count' must be an integer between 1 and 1000");
        }
        step.fault_count = static_cast<int>(count->integer);
    }
    if (const toml::Value* ee = table.find("expect_error")) {
        const bool shape = ee->kind == toml::Value::Kind::String && ee->text.size() == 7 && ee->text.compare(0, 3, "CML") == 0 &&
                           std::all_of(ee->text.begin() + 3, ee->text.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!shape) {
            return invalid(ee->line, "'expect_error' must look like CML5960");
        }
        step.expect_error = ee->text;
    }
    return step;
}

#if defined(_WIN32)

namespace {

namespace ipc = cml::ipc;

InjectStepOutcome fail(std::uint32_t code, std::string detail) {
    InjectStepOutcome out;
    out.ok = false;
    out.diag_code = code;
    out.detail = std::move(detail);
    return out;
}

}  // namespace

InjectStepOutcome run_inject_step(ipc::ControlClient& control, const Step& step, std::uint32_t timeout_ms) {
    const bool disarm = step.fault_stage == "none";
    // 1. The broker must list the stage.
    cml::Result<ipc::ControlResponse> stages = control.call("fault-stages", ipc::json::Value::object(), timeout_ms);
    if (!stages) {
        return fail(cml::ids::kFaultStepFailed, "fault-stages: " + stages.error().format());
    }
    if (!stages->ok) {
        return fail(cml::ids::kFaultStepFailed, "fault-stages was refused: " + stages->diag->id + ": " + stages->diag->message);
    }
    const ipc::json::Value* list = stages->result.find("stages");
    bool listed = disarm;
    std::string names;
    if (list != nullptr && list->is_array()) {
        for (const ipc::json::Value& v : list->items()) {
            names += (names.empty() ? "" : ", ") + v.as_string();
            listed = listed || v.as_string() == step.fault_stage;
        }
    }
    if (!listed && step.expect_error.empty()) {
        return fail(cml::ids::kFaultStepFailed, "the broker does not list the stage '" + step.fault_stage + "' (fault-stages: " + names + ")");
    }
    // 2. Arm it.
    ipc::json::Value args = ipc::json::Value::object();
    args.set("stage", ipc::json::Value::string(step.fault_stage));
    if (!disarm && step.fault_count != 1) {
        args.set("count", ipc::json::Value::integer(step.fault_count));
    }
    cml::Result<ipc::ControlResponse> response = control.call("inject-failure", std::move(args), timeout_ms);
    if (!response) {
        return fail(cml::ids::kFaultStepFailed, "inject-failure: " + response.error().format());
    }
    if (!step.expect_error.empty()) {
        if (response->ok) {
            return fail(cml::ids::kFaultStepFailed, "inject-failure succeeded, expected the refusal " + step.expect_error);
        }
        if (response->diag->id != step.expect_error) {
            return fail(cml::ids::kFaultStepFailed, "inject-failure was refused with " + response->diag->id + ": " + response->diag->message + ", expected " +
                                                        step.expect_error);
        }
        InjectStepOutcome out;
        out.detail = "inject-failure refused as expected: " + response->diag->id + ": " + response->diag->message;
        return out;
    }
    if (!response->ok) {
        return fail(cml::ids::kFaultStepFailed, "inject-failure '" + step.fault_stage + "' was refused: " + response->diag->id + ": " + response->diag->message +
                                                    " What to do: " + response->diag->action);
    }
    const std::string echoed = response->result.get_string("stage");
    const bool armed = response->result.get_bool("armed", !disarm);
    if (echoed != step.fault_stage || armed == disarm) {
        return fail(cml::ids::kFaultStepFailed, "inject-failure answered " + ipc::json::serialize(response->result) + " for the stage '" + step.fault_stage + "'");
    }
    InjectStepOutcome out;
    out.detail = disarm ? "all failure injection disarmed" : "armed '" + step.fault_stage + "': " + ipc::json::serialize(response->result);
    return out;
}

#endif  // _WIN32

}  // namespace cmltest
