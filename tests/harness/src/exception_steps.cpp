#include "exception_steps.h"

#if defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

#include "cml/ipc/control_ids.h"
#include "cml/util/format.h"

namespace cmltest {

namespace ipc = cml::ipc;
using Clock = std::chrono::steady_clock;

namespace {

ExceptionStepResult pass(std::string detail) {
    ExceptionStepResult r;
    r.detail = std::move(detail);
    return r;
}

ExceptionStepResult fail(std::uint32_t code, std::string detail) {
    ExceptionStepResult r;
    r.ok = false;
    r.diag_code = code;
    r.detail = std::move(detail);
    return r;
}

ExceptionStepResult fail(const cml::Diag& diag) {
    return fail(diag.code, diag.format());
}

}  // namespace

ExceptionStepResult ExceptionSteps::watch(ipc::ControlClient& control, std::uint32_t timeout_ms) {
    ipc::json::Value args = ipc::json::Value::object();
    ipc::json::Value events = ipc::json::Value::array();
    for (const char* e : {"diag", "patch_applied", "log", "exception"}) {
        events.push(ipc::json::Value::string(e));
    }
    args.set("events", std::move(events));
    cml::Result<ipc::ControlResponse> response = control.call("subscribe", std::move(args), timeout_ms);
    if (!response) {
        return fail(response.error());
    }
    if (!response->ok) {
        return fail(cml::ids::kControlCapabilityMissing, "subscribe to exception events refused: " + response->diag->id + ": " + response->diag->message);
    }
    return pass("subscribed to exception events; the agents hold faulting threads from now on");
}

ExceptionStepResult ExceptionSteps::expect(ipc::ControlClient& control, const Step& step, std::uint32_t limit_ms, bool limited_by_scenario) {
    if (limit_ms == 0) {
        return fail(cml::ids::kScenarioFailed, "the scenario's time ran out");
    }
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(limit_ms);
    std::string seen;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        const std::uint32_t slice = left <= 0 ? 0u : static_cast<std::uint32_t>(std::min<long long>(left, 250));
        cml::Result<std::optional<ipc::ControlEvent>> event = control.next_event(slice);
        if (!event) {
            return fail(event.error());
        }
        if (*event) {
            const ipc::ControlEvent& e = **event;
            seen += (seen.empty() ? "" : ", ") + e.name;
            if (e.name != "exception") {
                continue;
            }
            const std::string code = e.body.get_string("code");
            const std::string function = e.body.get_string("function");
            if (!step.exception_code.empty() && cml::upper(code) != cml::upper(step.exception_code)) {
                return fail(cml::ids::kExpectEventTimeout,
                            "an exception event arrived with code " + code + ", expected " + step.exception_code + ": " + ipc::json::serialize(e.body));
            }
            if (!step.exception_function.empty() && function.find(step.exception_function) == std::string::npos) {
                return fail(cml::ids::kExpectEventTimeout, "an exception event arrived in function '" + function + "', expected one containing '" +
                                                               step.exception_function + "': " + ipc::json::serialize(e.body));
            }
            have_current_ = true;
            current_pid_ = e.body.get_integer("pid");
            current_id_ = e.body.get_integer("exception_id");
            return pass("event " + ipc::json::serialize(e.body));
        }
        if (left <= 0) {
            if (limited_by_scenario) {
                return fail(cml::ids::kScenarioFailed, "the scenario's timeout ran out while waiting for an exception event");
            }
            return fail(cml::ids::kExpectEventTimeout, "no 'exception' event within " + std::to_string(step.within_s) + " s (events seen: " +
                                                           (seen.empty() ? "none" : seen) + ")");
        }
    }
}

ExceptionStepResult ExceptionSteps::act(ipc::ControlClient& control, const Step& step, std::uint32_t timeout_ms) {
    if (!have_current_) {
        return fail(cml::ids::kScenarioInvalid, "exception_action needs an expect_exception step before it");
    }
    ipc::json::Value args = ipc::json::Value::object();
    args.set("pid", ipc::json::Value::integer(current_pid_));
    args.set("exception_id", ipc::json::Value::integer(current_id_));
    args.set("action", ipc::json::Value::string(step.exception_action));
    cml::Result<ipc::ControlResponse> response = control.call("exception-action", std::move(args), timeout_ms + 10000);
    if (!response) {
        return fail(response.error());
    }
    if (!step.expect_error.empty()) {
        if (response->ok) {
            return fail(cml::ids::kScenarioFailed, "exception-action " + step.exception_action + " succeeded, expected the refusal " + step.expect_error);
        }
        if (response->diag->id != step.expect_error) {
            return fail(cml::ids::kScenarioFailed, "exception-action " + step.exception_action + " was refused with " + response->diag->id + ": " +
                                                       response->diag->message + ", expected " + step.expect_error);
        }
        return pass("refused as expected: " + response->diag->id + ": " + response->diag->message);
    }
    if (!response->ok) {
        return fail(cml::ids::kScenarioFailed, "exception-action " + step.exception_action + " failed: " + response->diag->id + ": " +
                                                   response->diag->message + " What to do: " + response->diag->action);
    }
    have_current_ = false;
    return pass("exception " + std::to_string(current_id_) + " of pid " + std::to_string(current_pid_) + ": " + step.exception_action);
}

ExceptionStepResult ExceptionSteps::pending(ipc::ControlClient& control, const Step& step, std::uint32_t timeout_ms) {
    cml::Result<ipc::ControlResponse> response = control.call("exceptions", ipc::json::Value::object(), timeout_ms);
    if (!response) {
        return fail(response.error());
    }
    if (!response->ok) {
        return fail(cml::ids::kScenarioFailed, "exceptions failed: " + response->diag->id + ": " + response->diag->message);
    }
    const ipc::json::Value* list = response->result.find("pending");
    const std::size_t count = list != nullptr && list->is_array() ? list->items().size() : 0;
    if (static_cast<long long>(count) != step.result_value) {
        return fail(cml::ids::kResultMismatch, std::to_string(count) + " threads are held, expected " + std::to_string(step.result_value) + ": " +
                                                   ipc::json::serialize(response->result));
    }
    return pass(std::to_string(count) + " threads held");
}

}  // namespace cmltest

#endif  // _WIN32
