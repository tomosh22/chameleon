#include "revert_step.h"

#include "cml/ipc/control_ids.h"
#include "cml/ipc/json.h"

namespace cmltest {

namespace ipc = cml::ipc;

RevertStepResult run_revert_step(ipc::ControlClient& control, const Step& step, std::uint32_t limit_ms) {
    RevertStepResult out;
    ipc::json::Value args = ipc::json::Value::object();
    args.set("scope", ipc::json::Value::string(step.revert_scope));
    cml::Result<ipc::ControlResponse> response = control.call("revert", std::move(args), limit_ms + 5000);
    if (!response) {
        out.diag = response.error();
        return out;
    }
    if (!step.expect_error.empty()) {
        if (response->ok) {
            out.diag = cml::Diag::make(cml::ids::kScenarioFailed, "the revert succeeded, expected the refusal " + step.expect_error, "Check the scenario.");
            return out;
        }
        if (response->diag->id != step.expect_error) {
            out.diag = cml::Diag::make(cml::ids::kScenarioFailed,
                                       "the revert was refused with " + response->diag->id + ": " + response->diag->message + ", expected " + step.expect_error,
                                       "Check the scenario.");
            return out;
        }
        out.detail = "revert refused as expected: " + response->diag->id + ": " + response->diag->message;
        return out;
    }
    if (!response->ok) {
        const ipc::ControlDiag& d = *response->diag;
        out.diag = cml::Diag::make(cml::ids::kScenarioFailed, "revert " + step.revert_scope + " failed: " + d.id + ": " + d.message + " What to do: " + d.action,
                                   "See the broker log.");
        return out;
    }
    const ipc::json::Value* reverted = response->result.find("reverted");
    if (reverted == nullptr || !reverted->is_array() || reverted->items().empty()) {
        out.diag = cml::Diag::make(cml::ids::kResultMismatch, "the revert answered ok but lists no reverted module: " + ipc::json::serialize(response->result),
                                   "Check the broker.");
        return out;
    }
    out.detail = "revert " + step.revert_scope + " ok: " + ipc::json::serialize(response->result);
    return out;
}

}  // namespace cmltest
